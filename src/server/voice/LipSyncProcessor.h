
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * LipSyncProcessor
 *
 * Handles the generation of lip sync data for sound files.
 * Dispatches to either WhisperLipSyncProcessor (fast, library-based) or
 * Rhubarb Lip Sync (legacy, subprocess-based) depending on configuration.
 *
 * Can be used both synchronously (for direct API calls) and asynchronously
 * (for background job processing).
 */
class LipSyncProcessor {
  public:
    /**
     * Progress callback function type
     *
     * Called during processing to report progress (0.0 to 1.0)
     * Can be used by job workers to update job status
     */
    using ProgressCallback = std::function<void(float progress)>;

    /**
     * Generate lip sync data for a sound file
     *
     * Dispatches to either whisper.cpp or Rhubarb based on the configured
     * lip sync engine. Both paths produce identical Rhubarb-compatible JSON output.
     *
     * @param soundFile The name of the sound file (not the full path)
     * @param soundsDir The directory containing sound files
     * @param rhubarbBinaryPath Path to the Rhubarb binary (used only when engine=rhubarb)
     * @param allowOverwrite Whether to overwrite existing JSON files
     * @param progressCallback Optional callback for progress updates
     * @param parentSpan Optional parent span for observability
     * @return Result containing the JSON content on success, or error message on failure
     */
    static Result<std::string> generateLipSync(const std::string &soundFile, const std::string &soundsDir,
                                                const std::string &rhubarbBinaryPath, bool allowOverwrite = false,
                                                ProgressCallback progressCallback = nullptr,
                                                std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Initialize the whisper.cpp lip sync engine.
     *
     * Should be called once at startup if the whisper engine is configured.
     * Safe to call multiple times (subsequent calls are no-ops).
     *
     * @param whisperModelPath Path to the whisper GGML model file
     * @param cmuDictPath Path to the CMU Pronouncing Dictionary file
     * @return true if initialization succeeded
     */
    static bool initializeWhisperEngine(const std::string &whisperModelPath, const std::string &cmuDictPath);

  private:
    /**
     * Generate lip sync using the Rhubarb subprocess (legacy path).
     */
    static Result<std::string> generateWithRhubarb(const std::string &soundFile, const std::string &soundsDir,
                                                    const std::string &rhubarbBinaryPath, bool allowOverwrite,
                                                    ProgressCallback progressCallback,
                                                    std::shared_ptr<OperationSpan> parentSpan);

    /**
     * Generate lip sync using whisper.cpp (fast path).
     */
    static Result<std::string> generateWithWhisper(const std::string &soundFile, const std::string &soundsDir,
                                                    bool allowOverwrite, ProgressCallback progressCallback,
                                                    std::shared_ptr<OperationSpan> parentSpan);

    /**
     * Validate the sound file exists and is a WAV file
     */
    static Result<bool> validateSoundFile(const std::filesystem::path &soundFilePath,
                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Execute the Rhubarb command and capture output
     */
    static Result<std::string> executeRhubarb(const std::string &command, const std::string &rhubarbBinaryPath,
                                               std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                               ProgressCallback progressCallback = nullptr);

    /**
     * Read and process the generated JSON file
     */
    static Result<std::string> readAndProcessJson(const std::filesystem::path &jsonOutputPath,
                                                   const std::string &soundFile,
                                                   std::shared_ptr<OperationSpan> parentSpan = nullptr);
};

} // namespace creatures::voice
