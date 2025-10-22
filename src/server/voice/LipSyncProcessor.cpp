
#include "LipSyncProcessor.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace fs = std::filesystem;

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

Result<std::string> LipSyncProcessor::generateLipSync(const std::string &soundFile, const std::string &soundsDir,
                                                       const std::string &rhubarbBinaryPath, bool allowOverwrite,
                                                       ProgressCallback progressCallback,
                                                       std::shared_ptr<OperationSpan> parentSpan) {

    debug("LipSyncProcessor::generateLipSync() called for file: {}, allowOverwrite: {}", soundFile, allowOverwrite);

    if (!parentSpan) {
        warn("no parent span provided for LipSyncProcessor.generateLipSync, creating a root span");
    }

    auto span = observability->createChildOperationSpan("LipSyncProcessor.generateLipSync", parentSpan);
    if (span) {
        span->setAttribute("sound.file", soundFile);
        span->setAttribute("allow_overwrite", allowOverwrite);
    }

    fs::path soundFilePath = fs::path(soundsDir) / soundFile;
    debug("Full sound file path: {}", soundFilePath.string());

    if (span) {
        span->setAttribute("sound.path", soundFilePath.string());
        span->setAttribute("sound.directory", soundsDir);
    }

    // Validate the sound file
    auto validationResult = validateSoundFile(soundFilePath, span);
    if (!validationResult.isSuccess()) {
        auto error = validationResult.getError().value();
        if (span) {
            span->setError(error.getMessage());
        }
        return error;
    }

    if (progressCallback) {
        progressCallback(0.1f);
    }

    // Check for transcript file
    fs::path transcriptPath = soundFilePath;
    transcriptPath.replace_extension(".txt");
    bool hasTranscript = fs::exists(transcriptPath);

    if (span) {
        span->setAttribute("has_transcript", hasTranscript);
    }

    if (hasTranscript) {
        info("Found transcript file: {}", transcriptPath.filename().string());
    } else {
        debug("No transcript file found for {}", soundFile);
    }

    if (progressCallback) {
        progressCallback(0.15f);
    }

    // Check if JSON file already exists
    fs::path jsonOutputPath = soundFilePath;
    jsonOutputPath.replace_extension(".json");
    debug("Output JSON path will be: {}", jsonOutputPath.string());

    if (fs::exists(jsonOutputPath)) {
        debug("JSON file already exists: {}", jsonOutputPath.string());
        if (!allowOverwrite) {
            std::string errorMsg =
                fmt::format("JSON file '{}' already exists. Set 'allow_overwrite' to true to overwrite it.",
                            jsonOutputPath.filename().string());
            warn(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            return ServerError(ServerError::InvalidData, errorMsg);
        }
        debug("Overwrite is allowed, will replace existing JSON file");
    } else {
        debug("JSON file does not exist, will create new file");
    }

    if (progressCallback) {
        progressCallback(0.2f);
    }

    // Build the Rhubarb command
    debug("Rhubarb binary path from config: {}", rhubarbBinaryPath);

    if (span) {
        span->setAttribute("rhubarb.binary", rhubarbBinaryPath);
    }

    std::string command = fmt::format("{} -f json -o \"{}\"", rhubarbBinaryPath, jsonOutputPath.string());
    debug("Building Rhubarb command...");

    // Add transcript if available
    if (hasTranscript) {
        command += fmt::format(" --dialogFile \"{}\"", transcriptPath.string());
        debug("Added transcript file to command");
    }

    // Add the input WAV file
    command += fmt::format(" \"{}\"", soundFilePath.string());

    // Redirect stderr to stdout to capture all output
    command += " 2>&1";

    info("Executing Rhubarb command: {}", command);

    if (progressCallback) {
        progressCallback(0.25f);
    }

    // Execute Rhubarb
    auto executionResult = executeRhubarb(command, rhubarbBinaryPath, span);
    if (!executionResult.isSuccess()) {
        auto error = executionResult.getError().value();
        if (span) {
            span->setError(error.getMessage());
        }
        return error;
    }

    info("Rhubarb processing completed successfully (exit code 0)");

    if (progressCallback) {
        progressCallback(0.8f);
    }

    // Verify JSON file was created
    debug("Verifying JSON file was created: {}", jsonOutputPath.string());
    if (!fs::exists(jsonOutputPath)) {
        auto rhubarbOutput = executionResult.getValue().value();
        std::string errorMsg =
            fmt::format("Rhubarb completed but did not create the expected JSON file '{}'.\n\nCommand: {}\n\nOutput:\n{}",
                        jsonOutputPath.filename().string(), command, rhubarbOutput);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::InternalError, errorMsg);
    }
    debug("JSON file created successfully");

    if (progressCallback) {
        progressCallback(0.9f);
    }

    // Read and process the JSON file
    auto jsonResult = readAndProcessJson(jsonOutputPath, soundFile, span);
    if (!jsonResult.isSuccess()) {
        auto error = jsonResult.getError().value();
        if (span) {
            span->setError(error.getMessage());
        }
        return error;
    }

    auto jsonContent = jsonResult.getValue().value();
    info("Successfully processed {} with Rhubarb, generated {} ({} bytes)", soundFile,
         jsonOutputPath.filename().string(), jsonContent.size());

    if (span) {
        span->setAttribute("json.output_file", jsonOutputPath.filename().string());
        span->setAttribute("json.output_size", static_cast<int64_t>(jsonContent.size()));
        span->setSuccess();
    }

    if (progressCallback) {
        progressCallback(1.0f);
    }

    return jsonContent;
}

Result<bool> LipSyncProcessor::validateSoundFile(const std::filesystem::path &soundFilePath,
                                                  std::shared_ptr<OperationSpan> parentSpan) {

    auto span = observability->createChildOperationSpan("LipSyncProcessor.validateSoundFile", parentSpan);
    if (span) {
        span->setAttribute("sound.path", soundFilePath.string());
    }

    // Check if the sound file exists
    debug("Checking if sound file exists: {}", soundFilePath.string());
    if (!fs::exists(soundFilePath)) {
        std::string errorMsg = fmt::format("Sound file '{}' not found", soundFilePath.filename().string());
        warn(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::NotFound, errorMsg);
    }
    debug("Sound file exists");

    // Validate it's a WAV file
    debug("Checking file extension: {}", soundFilePath.extension().string());
    if (soundFilePath.extension() != ".wav") {
        std::string errorMsg = fmt::format("Sound file '{}' must be a WAV file (has extension '{}')",
                                           soundFilePath.filename().string(), soundFilePath.extension().string());
        warn(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::InvalidData, errorMsg);
    }
    debug("File is a valid WAV file");

    if (span) {
        span->setSuccess();
    }

    return true;
}

Result<std::string> LipSyncProcessor::executeRhubarb(const std::string &command, const std::string &rhubarbBinaryPath,
                                                      std::shared_ptr<OperationSpan> parentSpan) {

    auto span = observability->createChildOperationSpan("LipSyncProcessor.executeRhubarb", parentSpan);
    if (span) {
        span->setAttribute("rhubarb.command", command);
    }

    std::string commandOutput;
    int exitCode = 0;

    try {
        debug("Opening pipe to execute Rhubarb...");
        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe) {
            std::string errorMsg = fmt::format("Failed to execute Rhubarb binary at '{}'. "
                                               "Is it installed and accessible? "
                                               "Check the server configuration (--rhubarb-binary-path or "
                                               "RHUBARB_BINARY_PATH environment variable). errno: {}",
                                               rhubarbBinaryPath, errno);
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            return ServerError(ServerError::InternalError, errorMsg);
        }
        debug("Pipe opened successfully, reading output...");

        // Read command output
        // TODO: Parse machine-readable output for progress updates
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            commandOutput += buffer;
        }
        debug("Command output captured ({} bytes)", commandOutput.length());

        exitCode = pclose(pipe);
        debug("Rhubarb process exited with code: {}", exitCode);

        if (span) {
            span->setAttribute("rhubarb.exit_code", static_cast<int64_t>(exitCode));
            span->setAttribute("rhubarb.output_length", static_cast<int64_t>(commandOutput.length()));
        }

        // Check if command succeeded
        if (exitCode != 0) {
            std::string errorMsg = fmt::format("Rhubarb processing failed with exit code {}.\n\nCommand: {}\n\nOutput:\n{}",
                                               exitCode, command, commandOutput);
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            return ServerError(ServerError::InternalError, errorMsg);
        }

        if (span) {
            span->setSuccess();
        }

        return commandOutput;

    } catch (const std::exception &e) {
        std::string errorMsg =
            fmt::format("Exception during Rhubarb execution: {}\n\nCommand: {}\n\nOutput:\n{}", e.what(), command, commandOutput);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::InternalError, errorMsg);
    }
}

Result<std::string> LipSyncProcessor::readAndProcessJson(const std::filesystem::path &jsonOutputPath,
                                                          const std::string &soundFile,
                                                          std::shared_ptr<OperationSpan> parentSpan) {

    auto span = observability->createChildOperationSpan("LipSyncProcessor.readAndProcessJson", parentSpan);
    if (span) {
        span->setAttribute("json.path", jsonOutputPath.string());
    }

    debug("Opening JSON file for reading: {}", jsonOutputPath.string());
    std::ifstream jsonFile(jsonOutputPath);
    if (!jsonFile.is_open()) {
        std::string errorMsg = fmt::format("Generated JSON file could not be read: {}", jsonOutputPath.string());
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::InternalError, errorMsg);
    }

    debug("Reading and parsing JSON file contents...");
    nlohmann::json rhubarbJson;
    try {
        jsonFile >> rhubarbJson;
    } catch (const nlohmann::json::exception &e) {
        std::string errorMsg = fmt::format("Failed to parse Rhubarb JSON output: {}", e.what());
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::InternalError, errorMsg);
    }

    // Strip the full path from soundFile, leave only the filename
    if (rhubarbJson.contains("metadata") && rhubarbJson["metadata"].contains("soundFile")) {
        rhubarbJson["metadata"]["soundFile"] = soundFile;
        debug("Stripped path from soundFile, now: {}", soundFile);
    }

    // Convert back to string
    std::string jsonContent = rhubarbJson.dump(2); // Pretty print with 2-space indent
    debug("JSON file processed successfully ({} bytes)", jsonContent.size());

    if (span) {
        span->setAttribute("json.size", static_cast<int64_t>(jsonContent.size()));
        span->setSuccess();
    }

    return jsonContent;
}

} // namespace creatures::voice
