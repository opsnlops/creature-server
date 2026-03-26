#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <whisper.h>

#include "RhubarbData.h"
#include "TextToViseme.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * WhisperLipSyncProcessor
 *
 * Generates lip sync data from WAV files using whisper.cpp for word-level
 * timestamps, then maps words to visemes via CMU Pronouncing Dictionary.
 *
 * The whisper model is loaded once at startup and reused for all requests.
 * Thread-safe: the whisper context is protected by a mutex since whisper.cpp
 * inference is not reentrant.
 */
class WhisperLipSyncProcessor {
  public:
    using ProgressCallback = std::function<void(float progress)>;

    /**
     * Initialize the processor with a whisper model and CMU dictionary.
     *
     * @param modelPath Path to the whisper GGML model file (e.g., ggml-base.en.bin)
     * @param cmuDictPath Path to the CMU Pronouncing Dictionary file
     * @return true if initialization succeeded
     */
    bool initialize(const std::filesystem::path &modelPath, const std::filesystem::path &cmuDictPath);

    /**
     * Check if the processor is initialized and ready for use.
     */
    [[nodiscard]] bool isInitialized() const;

    /**
     * Generate lip sync data for a WAV file.
     *
     * Runs whisper.cpp inference to get word-level timestamps, then converts
     * words to viseme cues via TextToViseme.
     *
     * @param wavFilePath Full path to the WAV file
     * @param transcriptText Optional transcript text (improves accuracy if provided)
     * @param progressCallback Optional progress callback (0.0 to 1.0)
     * @param parentSpan Optional parent span for observability
     * @return Result containing JSON string in Rhubarb-compatible format
     */
    Result<std::string> generateLipSync(const std::filesystem::path &wavFilePath,
                                         const std::string &transcriptText = "",
                                         ProgressCallback progressCallback = nullptr,
                                         std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Transcribe raw 16kHz mono float audio to text.
     *
     * This is a simpler interface than generateLipSync — it just returns
     * the transcribed text without word timestamps or viseme conversion.
     * Used by creature-listener to offload STT from the Pi to the server.
     *
     * @param audioData 16kHz mono float32 samples
     * @param parentSpan Optional parent span for observability
     * @return Result containing the transcribed text
     */
    Result<std::string> transcribe(const std::vector<float> &audioData,
                                    std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Get the singleton instance.
     *
     * The processor must be initialized before use via initialize().
     */
    static WhisperLipSyncProcessor &instance();

    // Non-copyable, non-movable (singleton with whisper context)
    WhisperLipSyncProcessor(const WhisperLipSyncProcessor &) = delete;
    WhisperLipSyncProcessor &operator=(const WhisperLipSyncProcessor &) = delete;

  private:
    WhisperLipSyncProcessor() = default;

    whisper_context *whisperCtx_ = nullptr;
    TextToViseme textToViseme_;
    mutable std::mutex whisperMutex_;
    bool initialized_ = false;

    /**
     * Load audio from a WAV file into a float PCM buffer suitable for whisper.cpp.
     *
     * Converts to mono 16kHz float32 as required by whisper.
     *
     * @param wavFilePath Path to the WAV file
     * @param audioDuration Output parameter for audio duration in seconds
     * @return Vector of float samples, or empty on error
     */
    static std::vector<float> loadAudioForWhisper(const std::filesystem::path &wavFilePath, double &audioDuration);

    /**
     * Convert whisper word timestamps + TextToViseme output into Rhubarb JSON format.
     *
     * @param soundFile Filename for the metadata
     * @param duration Audio duration in seconds
     * @param mouthCues Generated mouth cues
     * @return JSON string in Rhubarb-compatible format
     */
    static std::string formatAsRhubarbJson(const std::string &soundFile, double duration,
                                            const std::vector<RhubarbMouthCue> &mouthCues);
};

} // namespace creatures::voice
