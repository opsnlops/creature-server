#include "StreamingSpeechGenerationManager.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/PcmWavWriter.h"

namespace creatures {
extern std::shared_ptr<creatures::voice::CreatureVoices> voiceService;
}

namespace creatures::voice {

Result<StreamingSpeechResult> StreamingSpeechGenerationManager::generate(const SpeechGenerationRequest &request) {
    if (request.creatureId.empty()) {
        return Result<StreamingSpeechResult>{
            ServerError(ServerError::InvalidData, "Streaming speech generation requires a creature_id")};
    }

    if (request.text.empty()) {
        return Result<StreamingSpeechResult>{
            ServerError(ServerError::InvalidData, "Streaming speech generation text may not be empty")};
    }

    auto span = creatures::observability->createChildOperationSpan("StreamingSpeechGenerationManager.generate",
                                                                    request.parentSpan);
    if (span) {
        span->setAttribute("creature.id", request.creatureId);
        span->setAttribute("text.length", static_cast<int64_t>(request.text.size()));
        span->setAttribute("tts.method", std::string("elevenlabs_websocket"));
    }

    try {
        auto outputDir = request.outputDirectory.empty()
                             ? std::filesystem::path(creatures::config->getSoundFileLocation())
                             : request.outputDirectory;
        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec) {
            std::string errorMsg =
                fmt::format("Unable to create output directory {}: {}", outputDir.string(), ec.message());
            if (span) {
                span->setError(errorMsg);
            }
            return Result<StreamingSpeechResult>{ServerError(ServerError::InternalError, errorMsg)};
        }

        // Fetch creature from database
        auto creatureJsonResult = creatures::db->getCreatureJson(request.creatureId, span);
        if (!creatureJsonResult.isSuccess()) {
            auto error = creatureJsonResult.getError().value();
            if (span) {
                span->setError(error.getMessage());
            }
            return Result<StreamingSpeechResult>{error};
        }
        auto creatureJson = creatureJsonResult.getValue().value();

        auto creatureResult = creatures::db->getCreature(request.creatureId, span);
        if (!creatureResult.isSuccess()) {
            auto error = creatureResult.getError().value();
            if (span) {
                span->setError(error.getMessage());
            }
            return Result<StreamingSpeechResult>{error};
        }
        auto creatureModel = creatureResult.getValue().value();

        if (!creatureJson.contains("voice") || creatureJson["voice"].is_null()) {
            std::string errorMsg =
                fmt::format("No voice configuration found for creature {}", request.creatureId);
            if (span) {
                span->setError(errorMsg);
            }
            return Result<StreamingSpeechResult>{ServerError(ServerError::InvalidData, errorMsg)};
        }

        uint16_t audioChannel = 1;
        try {
            if (creatureJson.contains("audio_channel") && !creatureJson["audio_channel"].is_null()) {
                audioChannel = creatureJson["audio_channel"].get<uint16_t>();
            }
        } catch (const std::exception &e) {
            warn("Failed to parse audio_channel for creature {}: {}", request.creatureId, e.what());
        }

        // Extract voice config
        auto voiceConfig = creatureJson["voice"];
        std::string voiceId = voiceConfig["voice_id"].get<std::string>();
        std::string modelId = voiceConfig["model_id"].get<std::string>();
        float stability = voiceConfig["stability"].get<float>();
        float similarityBoost = voiceConfig["similarity_boost"].get<float>();

        // Validate that the configured model supports WebSocket streaming.
        // eleven_v3 and eleven_multilingual_v2 do NOT support the stream-input endpoint
        // and will return 403. The creature's voice config must use a streaming-compatible
        // model like eleven_turbo_v2_5 or eleven_flash_v2_5.
        static const std::vector<std::string> nonStreamingModels = {"eleven_v3", "eleven_multilingual_v2",
                                                                     "eleven_monolingual_v1",
                                                                     "eleven_multilingual_v1"};
        for (const auto &blocked : nonStreamingModels) {
            if (modelId == blocked) {
                std::string errorMsg = fmt::format(
                    "Creature '{}' is configured with model '{}' which does not support "
                    "WebSocket streaming. Change the creature's voice model to "
                    "'eleven_turbo_v2_5' or 'eleven_flash_v2_5' in the creature config, "
                    "then try again.",
                    request.creatureId, modelId);
                warn(errorMsg);
                if (span) {
                    span->setError(errorMsg);
                }
                return Result<StreamingSpeechResult>{ServerError(ServerError::InvalidData, errorMsg)};
            }
        }

        if (span) {
            span->setAttribute("voice.id", voiceId);
            span->setAttribute("voice.model", modelId);
            span->setAttribute("audio.channel", static_cast<int64_t>(audioChannel));
        }

        // Raw mono 48 kHz S16 PCM straight from ElevenLabs (Pro plan; issue
        // #12). Skips the MP3 → ffmpeg decode round-trip the old path needed.
        std::string outputFormat = "pcm_48000";

        // Call ElevenLabs streaming API
        StreamingTTSClient client;
        auto ttsResult =
            client.generateSpeech(creatures::config->getVoiceApiKey(), voiceId, modelId, request.text, outputFormat,
                                   stability, similarityBoost, nullptr, span);

        if (!ttsResult.isSuccess()) {
            auto error = ttsResult.getError().value();
            if (span) {
                span->setError(error.getMessage());
            }
            return Result<StreamingSpeechResult>{error};
        }

        auto ttsData = ttsResult.getValue().value();

        if (ttsData.audioData.empty()) {
            std::string errorMsg = "ElevenLabs streaming returned no audio data";
            if (span) {
                span->setError(errorMsg);
            }
            return Result<StreamingSpeechResult>{ServerError(ServerError::InternalError, errorMsg)};
        }

        // Write transcript
        auto transcriptPath = outputDir / "transcript.txt";
        {
            std::ofstream transcriptFile(transcriptPath);
            transcriptFile << request.text;
        }

        // Convert audio to 17-channel WAV
        auto wavPath = outputDir / "speech.wav";

        // PCM only as of #12 — decodeMp3ToMultichannelWav was retired with
        // AudioConverter; if a future caller needs MP3 it'd have to bring a
        // decoder back. Treating any non-pcm format here as a setup bug
        // rather than silently failing later.
        if (outputFormat.rfind("pcm_", 0) != 0) {
            std::string errorMsg =
                fmt::format("StreamingSpeechGenerationManager: only pcm_* output formats are supported "
                            "(got '{}'). See issue #12.",
                            outputFormat);
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            return Result<StreamingSpeechResult>{ServerError(ServerError::InvalidData, errorMsg)};
        }
        auto writeResult = writePcmToMultichannelWav(ttsData.audioData, wavPath, audioChannel, 48000);
        if (!writeResult.isSuccess()) {
            if (span) {
                span->setError(writeResult.getError()->getMessage());
            }
            return Result<StreamingSpeechResult>{writeResult.getError().value()};
        }

        // Convert alignment data to lip sync cues using TextToViseme
        auto lipSyncSpan = creatures::observability->createChildOperationSpan(
            "StreamingSpeechGenerationManager.alignmentToLipSync", span);

        // Initialize TextToViseme (uses CMU dict if available)
        TextToViseme textToViseme;
        auto cmuDictPath = creatures::config->getCmuDictPath();
        if (!cmuDictPath.empty()) {
            textToViseme.loadCmuDict(cmuDictPath);
        }

        std::vector<RhubarbMouthCue> mouthCues;
        if (!ttsData.charTimings.empty()) {
            mouthCues = textToViseme.charTimingsToMouthCues(ttsData.charTimings);
            info("Generated {} mouth cues from {} alignment characters", mouthCues.size(),
                 ttsData.charTimings.size());
        } else {
            // Fallback: no alignment data, generate basic cues from text
            warn("No alignment data from ElevenLabs, generating basic lip sync from text timing");
            // Create a simple open/close pattern based on word boundaries
            double totalDuration = ttsData.audioDurationSeconds;
            std::istringstream iss(request.text);
            std::string word;
            std::vector<TextToViseme::WordTiming> wordTimings;
            int wordCount = 0;
            while (iss >> word) {
                wordCount++;
            }
            if (wordCount > 0) {
                double wordDuration = totalDuration / static_cast<double>(wordCount);
                double time = 0.0;
                std::istringstream iss2(request.text);
                while (iss2 >> word) {
                    TextToViseme::WordTiming wt;
                    wt.word = word;
                    wt.startTime = time;
                    wt.endTime = time + wordDuration * 0.8; // 80% speaking, 20% gap
                    wordTimings.push_back(wt);
                    time += wordDuration;
                }
                mouthCues = textToViseme.wordsToMouthCues(wordTimings);
            }
        }

        if (lipSyncSpan) {
            lipSyncSpan->setAttribute("mouth_cues.count", static_cast<int64_t>(mouthCues.size()));
            lipSyncSpan->setSuccess();
        }

        // Build RhubarbSoundData (reusing existing data structure)
        RhubarbSoundData lipSyncData;
        lipSyncData.metadata.soundFile = wavPath.filename().string();
        lipSyncData.metadata.duration = ttsData.audioDurationSeconds;
        lipSyncData.mouthCues = mouthCues;

        // Format as JSON (Rhubarb-compatible)
        nlohmann::json lipSyncJson;
        lipSyncJson["metadata"]["soundFile"] = lipSyncData.metadata.soundFile;
        lipSyncJson["metadata"]["duration"] = lipSyncData.metadata.duration;
        lipSyncJson["mouthCues"] = nlohmann::json::array();
        for (const auto &cue : mouthCues) {
            nlohmann::json cueJson;
            cueJson["start"] = cue.start;
            cueJson["end"] = cue.end;
            cueJson["value"] = cue.value;
            lipSyncJson["mouthCues"].push_back(cueJson);
        }

        // Build result
        StreamingSpeechResult result;
        result.wavPath = wavPath;
        result.transcriptPath = transcriptPath;
        result.lipSyncData = lipSyncData;
        result.lipSyncJson = lipSyncJson.dump(2);
        result.audioDurationSeconds = ttsData.audioDurationSeconds;
        result.audioChannel = audioChannel;
        result.creature = creatureModel;
        result.creatureJson = creatureJson;

        if (span) {
            span->setSuccess();
        }

        return result;

    } catch (const std::exception &e) {
        if (span) {
            span->recordException(e);
            span->setError(e.what());
        }
        return Result<StreamingSpeechResult>{ServerError(ServerError::InternalError, e.what())};
    }
}

// writePcmToMultichannelWav + decodeMp3ToMultichannelWav moved out as part
// of issue #12. The first is now a free function in PcmWavWriter.h that all
// MP3-replacing call sites share; the MP3 decode path was retired with
// AudioConverter (Phase C of #12).

} // namespace creatures::voice
