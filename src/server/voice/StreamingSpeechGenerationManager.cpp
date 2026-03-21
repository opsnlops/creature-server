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
#include "server/voice/AudioConverter.h"

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

        if (span) {
            span->setAttribute("voice.id", voiceId);
            span->setAttribute("voice.model", modelId);
            span->setAttribute("audio.channel", static_cast<int64_t>(audioChannel));
        }

        // Determine audio format
        // PCM is preferred (no decode step) but requires a paid ElevenLabs plan
        // Fall back to MP3 which is universally available
        std::string outputFormat = "mp3_44100_192"; // Safe default

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

        if (outputFormat == "pcm_48000") {
            auto writeResult = writePcmToMultichannelWav(ttsData.audioData, wavPath, audioChannel, 48000);
            if (!writeResult.isSuccess()) {
                if (span) {
                    span->setError(writeResult.getError()->getMessage());
                }
                return Result<StreamingSpeechResult>{writeResult.getError().value()};
            }
        } else {
            // MP3 format - need to decode first via ffmpeg
            auto decodeResult = decodeMp3ToMultichannelWav(ttsData.audioData, wavPath, audioChannel, span);
            if (!decodeResult.isSuccess()) {
                if (span) {
                    span->setError(decodeResult.getError()->getMessage());
                }
                return Result<StreamingSpeechResult>{decodeResult.getError().value()};
            }
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

Result<size_t> StreamingSpeechGenerationManager::writePcmToMultichannelWav(const std::vector<uint8_t> &pcmData,
                                                                           const std::filesystem::path &wavPath,
                                                                           uint16_t audioChannel,
                                                                           uint32_t sampleRate) {
    // PCM data is mono 16-bit, we need to place it in a 17-channel WAV
    const uint16_t totalChannels = RTP_STREAMING_CHANNELS;
    const uint16_t bitsPerSample = 16;
    const uint16_t bytesPerSample = bitsPerSample / 8;

    // Number of samples per channel
    size_t monoSamples = pcmData.size() / bytesPerSample;

    // Total data size: monoSamples * totalChannels * bytesPerSample
    size_t dataSize = monoSamples * totalChannels * bytesPerSample;

    std::ofstream file(wavPath, std::ios::binary);
    if (!file.is_open()) {
        return Result<size_t>{ServerError(ServerError::InternalError,
                                           fmt::format("Cannot create WAV file: {}", wavPath.string()))};
    }

    // RIFF header
    uint32_t fileSize = static_cast<uint32_t>(36 + dataSize);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char *>(&fileSize), 4);
    file.write("WAVE", 4);

    // fmt chunk
    file.write("fmt ", 4);
    uint32_t fmtSize = 16;
    file.write(reinterpret_cast<const char *>(&fmtSize), 4);
    uint16_t audioFormat = 1; // PCM
    file.write(reinterpret_cast<const char *>(&audioFormat), 2);
    file.write(reinterpret_cast<const char *>(&totalChannels), 2);
    file.write(reinterpret_cast<const char *>(&sampleRate), 4);
    uint32_t byteRate = sampleRate * totalChannels * bytesPerSample;
    file.write(reinterpret_cast<const char *>(&byteRate), 4);
    uint16_t blockAlign = totalChannels * bytesPerSample;
    file.write(reinterpret_cast<const char *>(&blockAlign), 2);
    file.write(reinterpret_cast<const char *>(&bitsPerSample), 2);

    // data chunk
    file.write("data", 4);
    auto dataSizeU32 = static_cast<uint32_t>(dataSize);
    file.write(reinterpret_cast<const char *>(&dataSizeU32), 4);

    // Write interleaved samples: silence on all channels except target
    // Channel index is 0-based, audioChannel is 1-based
    uint16_t targetIdx = audioChannel - 1;
    int16_t silence = 0;

    const auto *monoPtr = reinterpret_cast<const int16_t *>(pcmData.data());
    for (size_t i = 0; i < monoSamples; ++i) {
        for (uint16_t ch = 0; ch < totalChannels; ++ch) {
            if (ch == targetIdx) {
                file.write(reinterpret_cast<const char *>(&monoPtr[i]), 2);
            } else {
                file.write(reinterpret_cast<const char *>(&silence), 2);
            }
        }
    }

    file.close();

    size_t totalSize = 44 + dataSize; // 44 byte header + data
    debug("Written 17-channel WAV: {} samples on channel {}, {} bytes total", monoSamples, audioChannel, totalSize);

    return totalSize;
}

Result<size_t> StreamingSpeechGenerationManager::decodeMp3ToMultichannelWav(
    const std::vector<uint8_t> &mp3Data, const std::filesystem::path &wavPath, uint16_t audioChannel,
    std::shared_ptr<OperationSpan> parentSpan) {

    auto span = creatures::observability->createChildOperationSpan(
        "StreamingSpeechGenerationManager.decodeMp3ToWav", parentSpan);

    // Write MP3 to temp file
    auto mp3TempPath = wavPath;
    mp3TempPath.replace_extension(".tmp.mp3");

    {
        std::ofstream mp3File(mp3TempPath, std::ios::binary);
        if (!mp3File.is_open()) {
            std::string msg = fmt::format("Cannot create temp MP3 file: {}", mp3TempPath.string());
            if (span) {
                span->setError(msg);
            }
            return Result<size_t>{ServerError(ServerError::InternalError, msg)};
        }
        mp3File.write(reinterpret_cast<const char *>(mp3Data.data()), static_cast<std::streamsize>(mp3Data.size()));
    }

    // Use AudioConverter to convert MP3 → 17-channel WAV
    auto convertResult = AudioConverter::convertMp3ToWav(mp3TempPath, wavPath,
                                                          creatures::config->getFfmpegBinaryPath(), audioChannel,
                                                          48000, span);

    // Clean up temp MP3
    std::error_code ec;
    std::filesystem::remove(mp3TempPath, ec);

    if (!convertResult.isSuccess()) {
        if (span) {
            span->setError(convertResult.getError()->getMessage());
        }
        return convertResult;
    }

    if (span) {
        span->setSuccess();
    }

    return convertResult;
}

} // namespace creatures::voice
