
#include "LipSyncProcessor.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "WhisperLipSyncProcessor.h"
#include "server/config/Configuration.h"
#include "server/namespace-stuffs.h"
#include "util/ChildProcess.h"
#include "util/ObservabilityManager.h"

namespace fs = std::filesystem;

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::voice {

bool LipSyncProcessor::initializeWhisperEngine(const std::string &whisperModelPath, const std::string &cmuDictPath) {
    if (whisperModelPath.empty()) {
        warn("Whisper model path is empty, whisper engine will not be available");
        return false;
    }
    if (cmuDictPath.empty()) {
        warn("CMU dictionary path is empty, whisper engine will not be available");
        return false;
    }

    info("Initializing whisper lip sync engine...");
    return WhisperLipSyncProcessor::instance().initialize(whisperModelPath, cmuDictPath);
}

Result<std::string> LipSyncProcessor::generateLipSync(const std::string &soundFile, const std::string &soundsDir,
                                                      const std::string &rhubarbBinaryPath, bool allowOverwrite,
                                                      ProgressCallback progressCallback,
                                                      std::shared_ptr<OperationSpan> parentSpan) {

    // Dispatch based on configured engine
    std::string engine = config->getLipSyncEngine();

    if (engine == "whisper" && WhisperLipSyncProcessor::instance().isInitialized()) {
        info("Using whisper.cpp lip sync engine for {}", soundFile);
        return generateWithWhisper(soundFile, soundsDir, allowOverwrite, progressCallback, parentSpan);
    }

    if (engine == "whisper" && !WhisperLipSyncProcessor::instance().isInitialized()) {
        warn("Whisper engine configured but not initialized, falling back to Rhubarb for {}", soundFile);
    }

    debug("Using Rhubarb lip sync engine for {}", soundFile);
    return generateWithRhubarb(soundFile, soundsDir, rhubarbBinaryPath, allowOverwrite, progressCallback, parentSpan);
}

Result<std::string> LipSyncProcessor::generateWithWhisper(const std::string &soundFile, const std::string &soundsDir,
                                                          bool allowOverwrite, ProgressCallback progressCallback,
                                                          std::shared_ptr<OperationSpan> parentSpan) {

    auto span = observability->createChildOperationSpan("LipSyncProcessor.generateWithWhisper", parentSpan);
    if (span) {
        span->setAttribute("sound.file", soundFile);
        span->setAttribute("engine", std::string("whisper"));
    }

    fs::path soundFilePath = fs::path(soundsDir) / soundFile;

    // Validate the sound file
    auto validationResult = validateSoundFile(soundFilePath, span);
    if (!validationResult.isSuccess()) {
        auto error = validationResult.getError().value();
        if (span) {
            span->setError(error.getMessage());
        }
        return error;
    }

    // Check if JSON file already exists
    fs::path jsonOutputPath = soundFilePath;
    jsonOutputPath.replace_extension(".json");

    if (fs::exists(jsonOutputPath) && !allowOverwrite) {
        std::string errorMsg =
            fmt::format("JSON file '{}' already exists. Set 'allow_overwrite' to true to overwrite it.",
                        jsonOutputPath.filename().string());
        warn(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::InvalidData, errorMsg);
    }

    // Check for transcript file (optional, improves whisper accuracy)
    fs::path transcriptPath = soundFilePath;
    transcriptPath.replace_extension(".txt");
    std::string transcriptText;
    if (fs::exists(transcriptPath)) {
        std::ifstream transcriptFile(transcriptPath);
        if (transcriptFile.is_open()) {
            std::string line;
            while (std::getline(transcriptFile, line)) {
                if (!transcriptText.empty()) {
                    transcriptText += " ";
                }
                transcriptText += line;
            }
            info("Loaded transcript for whisper: {} chars", transcriptText.size());
        }
    }

    // Run whisper lip sync
    auto result =
        WhisperLipSyncProcessor::instance().generateLipSync(soundFilePath, transcriptText, progressCallback, span);

    if (result.isSuccess()) {
        auto jsonContent = result.getValue().value();
        info("Whisper lip sync completed for {}: {} bytes", soundFile, jsonContent.size());
        if (span) {
            span->setSuccess();
        }
    } else if (span) {
        span->setError(result.getError()->getMessage());
    }

    return result;
}

Result<std::string> LipSyncProcessor::generateWithRhubarb(const std::string &soundFile, const std::string &soundsDir,
                                                          const std::string &rhubarbBinaryPath, bool allowOverwrite,
                                                          ProgressCallback progressCallback,
                                                          std::shared_ptr<OperationSpan> parentSpan) {

    debug("LipSyncProcessor::generateWithRhubarb() called for file: {}, allowOverwrite: {}", soundFile, allowOverwrite);

    if (!parentSpan) {
        warn("no parent span provided for LipSyncProcessor.generateLipSync, creating a root span");
    }

    auto span = observability->createChildOperationSpan("LipSyncProcessor.generateWithRhubarb", parentSpan);
    if (span) {
        span->setAttribute("sound.file", soundFile);
        span->setAttribute("allow_overwrite", allowOverwrite);
        span->setAttribute("engine", std::string("rhubarb"));
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

    // Initial progress: validation complete
    if (progressCallback) {
        progressCallback(0.05f);
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

    // Pre-execution progress: ready to start Rhubarb
    if (progressCallback) {
        progressCallback(0.10f);
    }

    // Build the Rhubarb command
    debug("Rhubarb binary path from config: {}", rhubarbBinaryPath);

    if (span) {
        span->setAttribute("rhubarb.binary", rhubarbBinaryPath);
        span->setAttribute("rhubarb.input_file", soundFilePath.filename().string());
        span->setAttribute("rhubarb.output_file", jsonOutputPath.filename().string());
    }

    // Build the argv array. Each element is a separate argument; no shell
    // is involved, so filenames cannot inject metacharacters.
    std::vector<std::string> args = {"-f", "json", "--machineReadable", "-o", jsonOutputPath.string()};
    if (hasTranscript) {
        args.push_back("--dialogFile");
        args.push_back(transcriptPath.string());
        debug("Added transcript file to command");
    }
    args.push_back(soundFilePath.string());

    info("Executing Rhubarb: {} (with {} args, input={})", rhubarbBinaryPath, args.size(),
         soundFilePath.filename().string());

    // Execute Rhubarb with real-time progress parsing
    auto executionResult = executeRhubarb(rhubarbBinaryPath, args, span, progressCallback);
    if (!executionResult.isSuccess()) {
        auto error = executionResult.getError().value();
        if (span) {
            span->setError(error.getMessage());
        }
        return error;
    }

    info("Rhubarb processing completed successfully (exit code 0)");

    // Rhubarb complete, now reading output
    if (progressCallback) {
        progressCallback(0.96f);
    }

    // Verify JSON file was created
    debug("Verifying JSON file was created: {}", jsonOutputPath.string());
    if (!fs::exists(jsonOutputPath)) {
        auto rhubarbOutput = executionResult.getValue().value();
        std::string errorMsg =
            fmt::format("Rhubarb completed but did not create the expected JSON file '{}'.\n\nOutput:\n{}",
                        jsonOutputPath.filename().string(), rhubarbOutput);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return ServerError(ServerError::InternalError, errorMsg);
    }
    debug("JSON file created successfully");

    // Read and process the JSON file
    if (progressCallback) {
        progressCallback(0.98f);
    }

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

    // Final progress: complete
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

Result<std::string> LipSyncProcessor::executeRhubarb(const std::string &rhubarbBinaryPath,
                                                     const std::vector<std::string> &args,
                                                     std::shared_ptr<OperationSpan> parentSpan,
                                                     ProgressCallback progressCallback) {

    auto span = observability->createChildOperationSpan("LipSyncProcessor.executeRhubarb", parentSpan);
    if (span) {
        span->setAttribute("rhubarb.binary", rhubarbBinaryPath);
        span->setAttribute("rhubarb.machine_readable", true);
        span->setAttribute("rhubarb.arg_count", static_cast<int64_t>(args.size()));
    }

    // Parse each emitted line as JSON for progress updates, but always
    // accumulate the raw output into the helper's result so we can
    // include it in error messages.
    auto lineCallback = [&](const std::string &line) {
        if (!progressCallback) {
            return;
        }
        try {
            std::string trimmed = line;
            if (!trimmed.empty() && trimmed.back() == '\n') {
                trimmed.pop_back();
            }
            auto json = nlohmann::json::parse(trimmed);
            if (json.contains("type") && json["type"] == "progress" && json.contains("value")) {
                float progress = json["value"].get<float>();
                float scaledProgress = 0.25f + (progress * 0.70f);
                debug("Rhubarb progress: {:.1f}% (scaled to {:.1f}%)", progress * 100.0f, scaledProgress * 100.0f);
                progressCallback(scaledProgress);
                if (span) {
                    span->setAttribute("rhubarb.progress", static_cast<double>(progress));
                }
            }
        } catch (const nlohmann::json::exception &) {
            // Not valid JSON or doesn't have expected fields, skip
        }
    };

    try {
        auto spawnResult = util::runChildProcess(rhubarbBinaryPath, args, /*mergeStderrToStdout=*/true, lineCallback);
        if (!spawnResult.isSuccess()) {
            auto err = spawnResult.getError().value();
            std::string errorMsg = fmt::format("Failed to execute Rhubarb binary at '{}'. "
                                               "Is it installed and accessible? "
                                               "Check the server configuration (--rhubarb-binary-path or "
                                               "RHUBARB_BINARY_PATH environment variable). Detail: {}",
                                               rhubarbBinaryPath, err.getMessage());
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            return ServerError(ServerError::InternalError, errorMsg);
        }
        auto child = spawnResult.getValue().value();
        debug("Rhubarb output captured ({} bytes), exit code {}", child.output.size(), child.exitCode);

        if (span) {
            span->setAttribute("rhubarb.exit_code", static_cast<int64_t>(child.exitCode));
            span->setAttribute("rhubarb.output_length", static_cast<int64_t>(child.output.length()));
        }

        if (child.exitCode != 0) {
            std::string errorMsg = fmt::format("Rhubarb processing failed with exit code {}.\n\nOutput:\n{}",
                                               child.exitCode, child.output);
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            return ServerError(ServerError::InternalError, errorMsg);
        }

        if (span) {
            span->setSuccess();
        }

        return child.output;

    } catch (const std::exception &e) {
        std::string errorMsg = fmt::format("Exception during Rhubarb execution: {}", e.what());
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
