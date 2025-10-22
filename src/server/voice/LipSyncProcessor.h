
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
 * Handles the generation of lip sync data using Rhubarb Lip Sync.
 * This class contains the business logic for processing sound files,
 * separate from the web API layer.
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
     * @param soundFile The name of the sound file (not the full path)
     * @param soundsDir The directory containing sound files
     * @param rhubarbBinaryPath Path to the Rhubarb binary
     * @param allowOverwrite Whether to overwrite existing JSON files
     * @param progressCallback Optional callback for progress updates
     * @param parentSpan Optional parent span for observability
     * @return Result containing the JSON content on success, or error message on failure
     */
    static Result<std::string> generateLipSync(const std::string &soundFile, const std::string &soundsDir,
                                                const std::string &rhubarbBinaryPath, bool allowOverwrite = false,
                                                ProgressCallback progressCallback = nullptr,
                                                std::shared_ptr<OperationSpan> parentSpan = nullptr);

  private:
    /**
     * Validate the sound file exists and is a WAV file
     *
     * @param soundFilePath Path to the sound file
     * @param parentSpan Optional parent span for observability
     * @return Result<bool> indicating success or error
     */
    static Result<bool> validateSoundFile(const std::filesystem::path &soundFilePath,
                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Execute the Rhubarb command and capture output
     *
     * @param command The command to execute
     * @param rhubarbBinaryPath Path to the binary (for error messages)
     * @param parentSpan Optional parent span for observability
     * @param progressCallback Optional callback for real-time progress updates
     * @return Result containing the command output on success
     */
    static Result<std::string> executeRhubarb(const std::string &command, const std::string &rhubarbBinaryPath,
                                               std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                               ProgressCallback progressCallback = nullptr);

    /**
     * Read and process the generated JSON file
     *
     * @param jsonOutputPath Path to the JSON file
     * @param soundFile Original sound file name (for metadata correction)
     * @param parentSpan Optional parent span for observability
     * @return Result containing the processed JSON content
     */
    static Result<std::string> readAndProcessJson(const std::filesystem::path &jsonOutputPath,
                                                   const std::string &soundFile,
                                                   std::shared_ptr<OperationSpan> parentSpan = nullptr);
};

} // namespace creatures::voice
