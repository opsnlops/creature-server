#include "WhisperLipSyncProcessor.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <whisper.h>

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

WhisperLipSyncProcessor &WhisperLipSyncProcessor::instance() {
    static WhisperLipSyncProcessor instance;
    return instance;
}

bool WhisperLipSyncProcessor::isInitialized() const { return initialized_; }

bool WhisperLipSyncProcessor::initialize(const std::filesystem::path &modelPath,
                                          const std::filesystem::path &cmuDictPath) {
    std::lock_guard<std::mutex> lock(whisperMutex_);

    if (initialized_) {
        warn("WhisperLipSyncProcessor already initialized, skipping");
        return true;
    }

    // Load the CMU dictionary
    if (!textToViseme_.loadCmuDict(cmuDictPath)) {
        error("Failed to load CMU dictionary from {}", cmuDictPath.string());
        return false;
    }
    info("CMU dictionary loaded: {} words", textToViseme_.wordCount());

    // Load the whisper model
    if (!std::filesystem::exists(modelPath)) {
        error("Whisper model file not found: {}", modelPath.string());
        return false;
    }

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false; // CPU-only on server (AVX2 is fast enough)

    whisperCtx_ = whisper_init_from_file_with_params(modelPath.string().c_str(), cparams);
    if (!whisperCtx_) {
        error("Failed to initialize whisper model from {}", modelPath.string());
        return false;
    }

    initialized_ = true;
    info("WhisperLipSyncProcessor initialized with model: {}", modelPath.filename().string());
    return true;
}

std::vector<float> WhisperLipSyncProcessor::loadAudioForWhisper(const std::filesystem::path &wavFilePath,
                                                                 double &audioDuration) {
    audioDuration = 0.0;

    std::ifstream file(wavFilePath, std::ios::binary);
    if (!file.is_open()) {
        error("Failed to open WAV file: {}", wavFilePath.string());
        return {};
    }

    // Read WAV header
    char riffHeader[4];
    file.read(riffHeader, 4);
    if (std::strncmp(riffHeader, "RIFF", 4) != 0) {
        error("Not a valid WAV file (missing RIFF header): {}", wavFilePath.string());
        return {};
    }

    uint32_t fileSize = 0;
    file.read(reinterpret_cast<char *>(&fileSize), 4);

    char waveHeader[4];
    file.read(waveHeader, 4);
    if (std::strncmp(waveHeader, "WAVE", 4) != 0) {
        error("Not a valid WAV file (missing WAVE header): {}", wavFilePath.string());
        return {};
    }

    // Find the fmt and data chunks
    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    bool foundFmt = false;

    while (file.good()) {
        char chunkId[4];
        uint32_t chunkSize = 0;

        file.read(chunkId, 4);
        file.read(reinterpret_cast<char *>(&chunkSize), 4);

        if (!file.good()) {
            break;
        }

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            file.read(reinterpret_cast<char *>(&audioFormat), 2);
            file.read(reinterpret_cast<char *>(&numChannels), 2);
            file.read(reinterpret_cast<char *>(&sampleRate), 4);
            uint32_t byteRate = 0;
            file.read(reinterpret_cast<char *>(&byteRate), 4);
            uint16_t blockAlign = 0;
            file.read(reinterpret_cast<char *>(&blockAlign), 2);
            file.read(reinterpret_cast<char *>(&bitsPerSample), 2);

            // Handle WAVE_FORMAT_EXTENSIBLE (0xFFFE / 65534)
            // ffmpeg outputs this for multi-channel WAVs. The actual format
            // is in the SubFormat GUID at the end of the extended header.
            // For our purposes, if bitsPerSample is 16, it's PCM data.
            if (audioFormat == 0xFFFE && chunkSize > 16) {
                // Read cbSize (2 bytes), validBitsPerSample (2 bytes), channelMask (4 bytes)
                uint16_t cbSize = 0;
                file.read(reinterpret_cast<char *>(&cbSize), 2);
                uint16_t validBitsPerSample = 0;
                file.read(reinterpret_cast<char *>(&validBitsPerSample), 2);
                uint32_t channelMask = 0;
                file.read(reinterpret_cast<char *>(&channelMask), 4);

                // Read first 2 bytes of SubFormat GUID to get the actual format
                uint16_t subFormat = 0;
                file.read(reinterpret_cast<char *>(&subFormat), 2);

                if (subFormat == 1) { // KSDATAFORMAT_SUBTYPE_PCM
                    audioFormat = 1; // Treat as regular PCM
                    debug("WAVE_FORMAT_EXTENSIBLE with PCM subformat, {} channels", numChannels);
                }

                // Skip remaining SubFormat GUID bytes (14) + any extra
                size_t bytesRead = 16 + 2 + 2 + 4 + 2; // fmt fields + cbSize + validBits + mask + subFormat
                if (chunkSize > bytesRead) {
                    file.seekg(chunkSize - bytesRead, std::ios::cur);
                }
            } else if (chunkSize > 16) {
                // Skip any extra format bytes for other formats
                file.seekg(chunkSize - 16, std::ios::cur);
            }
            foundFmt = true;
        } else if (std::strncmp(chunkId, "data", 4) == 0 && foundFmt) {
            if (audioFormat != 1) { // PCM (or EXTENSIBLE resolved to PCM above)
                error("WAV file is not PCM format (format={}): {}", audioFormat, wavFilePath.string());
                return {};
            }

            size_t totalSamples = chunkSize / (bitsPerSample / 8);
            size_t samplesPerChannel = totalSamples / numChannels;

            audioDuration = static_cast<double>(samplesPerChannel) / static_cast<double>(sampleRate);

            // Read raw PCM data
            std::vector<int16_t> pcmData(totalSamples);
            if (bitsPerSample == 16) {
                file.read(reinterpret_cast<char *>(pcmData.data()), chunkSize);
            } else {
                error("Unsupported bits per sample: {}", bitsPerSample);
                return {};
            }

            // Convert to mono float32 at original sample rate first
            std::vector<float> monoFloat(samplesPerChannel);
            for (size_t i = 0; i < samplesPerChannel; ++i) {
                float sample = 0.0f;
                for (uint16_t ch = 0; ch < numChannels; ++ch) {
                    sample += static_cast<float>(pcmData[i * numChannels + ch]) / 32768.0f;
                }
                monoFloat[i] = sample / static_cast<float>(numChannels);
            }

            // Resample to 16kHz if needed (whisper requires 16kHz)
            if (sampleRate == WHISPER_SAMPLE_RATE) {
                return monoFloat;
            }

            // Simple linear interpolation resampling
            double ratio = static_cast<double>(WHISPER_SAMPLE_RATE) / static_cast<double>(sampleRate);
            size_t outputSamples = static_cast<size_t>(std::ceil(static_cast<double>(samplesPerChannel) * ratio));
            std::vector<float> resampled(outputSamples);

            for (size_t i = 0; i < outputSamples; ++i) {
                double srcIdx = static_cast<double>(i) / ratio;
                size_t idx0 = static_cast<size_t>(srcIdx);
                size_t idx1 = std::min(idx0 + 1, samplesPerChannel - 1);
                double frac = srcIdx - static_cast<double>(idx0);
                resampled[i] =
                    static_cast<float>((1.0 - frac) * static_cast<double>(monoFloat[idx0]) +
                                       frac * static_cast<double>(monoFloat[idx1]));
            }

            debug("Loaded WAV: {}ch {}Hz {}bit -> mono 16kHz, {} samples, {:.2f}s", numChannels, sampleRate,
                  bitsPerSample, resampled.size(), audioDuration);

            return resampled;
        } else {
            // Skip unknown chunk
            file.seekg(chunkSize, std::ios::cur);
        }
    }

    error("WAV file missing fmt or data chunk: {}", wavFilePath.string());
    return {};
}

std::string WhisperLipSyncProcessor::formatAsRhubarbJson(const std::string &soundFile, double duration,
                                                          const std::vector<RhubarbMouthCue> &mouthCues) {
    nlohmann::json result;

    // Metadata section (matches Rhubarb format)
    result["metadata"]["soundFile"] = soundFile;
    result["metadata"]["duration"] = duration;

    // Mouth cues section
    result["mouthCues"] = nlohmann::json::array();
    for (const auto &cue : mouthCues) {
        nlohmann::json cueJson;
        cueJson["start"] = cue.start;
        cueJson["end"] = cue.end;
        cueJson["value"] = cue.value;
        result["mouthCues"].push_back(cueJson);
    }

    return result.dump(2);
}

Result<std::string> WhisperLipSyncProcessor::generateLipSync(const std::filesystem::path &wavFilePath,
                                                              const std::string &transcriptText,
                                                              ProgressCallback progressCallback,
                                                              std::shared_ptr<OperationSpan> parentSpan) {
    if (!initialized_) {
        return ServerError(ServerError::InternalError, "WhisperLipSyncProcessor not initialized");
    }

    auto span = creatures::observability->createChildOperationSpan("WhisperLipSyncProcessor.generateLipSync",
                                                                    parentSpan);
    if (span) {
        span->setAttribute("sound.file", wavFilePath.string());
        span->setAttribute("has_transcript", !transcriptText.empty());
    }

    if (progressCallback) {
        progressCallback(0.05f);
    }

    // Load audio
    double audioDuration = 0.0;
    std::vector<float> audioData;
    {
        auto loadSpan =
            creatures::observability->createChildOperationSpan("WhisperLipSyncProcessor.loadAudio", span);
        if (loadSpan) {
            loadSpan->setAttribute("sound.file", wavFilePath.string());
        }

        audioData = loadAudioForWhisper(wavFilePath, audioDuration);

        if (audioData.empty()) {
            std::string errorMsg = fmt::format("Failed to load audio from {}", wavFilePath.string());
            if (loadSpan) {
                loadSpan->setError(errorMsg);
            }
            if (span) {
                span->setError(errorMsg);
            }
            return ServerError(ServerError::InternalError, errorMsg);
        }

        if (loadSpan) {
            loadSpan->setAttribute("audio.duration_s", audioDuration);
            loadSpan->setAttribute("audio.samples", static_cast<int64_t>(audioData.size()));
            loadSpan->setSuccess();
        }
    }

    if (span) {
        span->setAttribute("audio.duration_s", audioDuration);
        span->setAttribute("audio.samples", static_cast<int64_t>(audioData.size()));
    }

    if (progressCallback) {
        progressCallback(0.15f);
    }

    // Run whisper inference (mutex-protected)
    std::vector<TextToViseme::WordTiming> wordTimings;
    {
        std::lock_guard<std::mutex> lock(whisperMutex_);

        struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.print_progress = false;
        params.print_special = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.language = "en";
        params.token_timestamps = true; // Enable word-level timestamps
        params.max_len = 0;             // No max segment length (we want full segments)
        params.tdrz_enable = false;

        // If we have a transcript, use it as initial prompt to guide recognition
        if (!transcriptText.empty()) {
            params.initial_prompt = transcriptText.c_str();
        }

        auto inferSpan =
            creatures::observability->createChildOperationSpan("WhisperLipSyncProcessor.inference", span);

        int result = whisper_full(whisperCtx_, params, audioData.data(), static_cast<int>(audioData.size()));

        if (result != 0) {
            std::string errorMsg = fmt::format("Whisper inference failed with code {}", result);
            if (inferSpan) {
                inferSpan->setError(errorMsg);
            }
            if (span) {
                span->setError(errorMsg);
            }
            return ServerError(ServerError::InternalError, errorMsg);
        }

        if (inferSpan) {
            inferSpan->setSuccess();
        }

        if (progressCallback) {
            progressCallback(0.70f);
        }

        // Extract word-level timestamps from whisper output
        int numSegments = whisper_full_n_segments(whisperCtx_);

        if (span) {
            span->setAttribute("whisper.segments", static_cast<int64_t>(numSegments));
        }

        for (int seg = 0; seg < numSegments; ++seg) {
            int numTokens = whisper_full_n_tokens(whisperCtx_, seg);

            for (int tok = 0; tok < numTokens; ++tok) {
                auto tokenData = whisper_full_get_token_data(whisperCtx_, seg, tok);
                const char *tokenText = whisper_full_get_token_text(whisperCtx_, seg, tok);

                if (!tokenText || tokenText[0] == '\0') {
                    continue;
                }

                // Skip special tokens (they start with '[' or '<')
                if (tokenText[0] == '[' || tokenText[0] == '<') {
                    continue;
                }

                std::string text(tokenText);

                // Strip leading/trailing whitespace from token
                size_t start = text.find_first_not_of(" \t\n\r");
                size_t end = text.find_last_not_of(" \t\n\r");
                if (start == std::string::npos) {
                    continue;
                }
                text = text.substr(start, end - start + 1);

                if (text.empty()) {
                    continue;
                }

                double tokenStart = static_cast<double>(tokenData.t0) / 100.0; // centiseconds to seconds
                double tokenEnd = static_cast<double>(tokenData.t1) / 100.0;

                // Ensure reasonable bounds
                if (tokenStart < 0.0) {
                    tokenStart = 0.0;
                }
                if (tokenEnd <= tokenStart) {
                    tokenEnd = tokenStart + 0.05; // minimum 50ms per token
                }
                if (tokenEnd > audioDuration) {
                    tokenEnd = audioDuration;
                }

                TextToViseme::WordTiming wt;
                wt.word = text;
                wt.startTime = tokenStart;
                wt.endTime = tokenEnd;
                wordTimings.push_back(wt);
            }
        }
    }

    if (progressCallback) {
        progressCallback(0.80f);
    }

    if (span) {
        span->setAttribute("whisper.word_count", static_cast<int64_t>(wordTimings.size()));
    }

    debug("Whisper found {} word tokens in {:.2f}s audio", wordTimings.size(), audioDuration);

    // Convert word timings to mouth cues using TextToViseme
    std::vector<RhubarbMouthCue> mouthCues;
    {
        auto visemeSpan =
            creatures::observability->createChildOperationSpan("WhisperLipSyncProcessor.wordsToVisemes", span);

        mouthCues = textToViseme_.wordsToMouthCues(wordTimings);

        if (visemeSpan) {
            visemeSpan->setAttribute("input.word_count", static_cast<int64_t>(wordTimings.size()));
            visemeSpan->setAttribute("output.cue_count", static_cast<int64_t>(mouthCues.size()));
            visemeSpan->setSuccess();
        }
    }

    if (span) {
        span->setAttribute("mouth_cues.count", static_cast<int64_t>(mouthCues.size()));
    }

    if (progressCallback) {
        progressCallback(0.90f);
    }

    // Format as Rhubarb-compatible JSON
    auto jsonContent = formatAsRhubarbJson(wavFilePath.filename().string(), audioDuration, mouthCues);

    // Write JSON file alongside the WAV file
    auto jsonOutputPath = wavFilePath;
    jsonOutputPath.replace_extension(".json");

    std::ofstream jsonFile(jsonOutputPath);
    if (jsonFile.is_open()) {
        jsonFile << jsonContent;
        jsonFile.close();
        debug("Written lip sync JSON to {}", jsonOutputPath.string());
    } else {
        warn("Could not write lip sync JSON to {}", jsonOutputPath.string());
    }

    if (span) {
        span->setAttribute("json.output_size", static_cast<int64_t>(jsonContent.size()));
        span->setSuccess();
    }

    if (progressCallback) {
        progressCallback(1.0f);
    }

    info("Whisper lip sync complete: {} cues from {} words, {:.2f}s audio", mouthCues.size(), wordTimings.size(),
         audioDuration);

    return jsonContent;
}

} // namespace creatures::voice
