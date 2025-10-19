
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "exception/exception.h"

#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"

#include "server/config/Configuration.h"

#include "model/Sound.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

#include "util/ObservabilityManager.h"
#include "util/helpers.h"

#include "SoundService.h"

namespace creatures {
extern std::shared_ptr<creatures::Configuration> config;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace fs = std::filesystem;

namespace creatures ::ws {

using oatpp::web::protocol::http::Status;

oatpp::Object<ListDto<oatpp::Object<creatures::SoundDto>>> SoundService::getAllSounds() {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    appLogger->debug("Request to return a list of the sound files");

    // Copy the path locally
    std::string path = config->getSoundFileLocation();

    // Create the response to return
    auto soundList = oatpp::Vector<oatpp::Object<creatures::SoundDto>>::createShared();

    bool error = false;
    Status status = Status::CODE_200;
    oatpp::String message;

    // Define acceptable sound file extensions
    std::unordered_set<std::string> acceptableExtensions = {".mp3", ".wav", ".flac"};

    try {
        if (fs::exists(path) && fs::is_directory(path)) {
            for (const auto &entry : fs::directory_iterator(path)) {
                const auto &filepath = entry.path();
                if (fs::is_regular_file(entry.status())) {
                    std::string extension = filepath.extension().string();
                    if (acceptableExtensions.find(extension) != acceptableExtensions.end()) {
                        auto filename = filepath.filename().string(); // Get the filename

                        // Get file size with error handling
                        uintmax_t size = 0;
                        try {
                            size = fs::file_size(filepath);
                        } catch (const fs::filesystem_error &e) {
                            appLogger->warn("Failed to get file size for {}: {}", filename, e.what());
                            continue; // Skip this file
                        }

                        // Validate file size is reasonable (prevent display of huge files)
                        constexpr uintmax_t MAX_SOUND_FILE_SIZE = 1024 * 1024 * 1024; // 1GB max
                        if (size > MAX_SOUND_FILE_SIZE) {
                            appLogger->warn("Skipping oversized sound file: {} ({} bytes)", filename, size);
                            continue;
                        }

                        std::string transcript;

                        // Create a non-const copy of filepath to modify the extension
                        auto transcriptPath = filepath;
                        transcriptPath.replace_extension(".txt");
                        if (fs::exists(transcriptPath)) {
                            transcript = transcriptPath.filename().string();
                        }

                        Sound sound{filename, (uint32_t)size, transcript};

                        appLogger->debug("Adding sound file: {} ({})", sound.fileName, sound.size);
                        soundList->emplace_back(creatures::convertSoundToDto(sound));
                    }
                }
            }

            // Sort the list by file name (case-insensitive)
            std::sort(soundList->begin(), soundList->end(),
                      [](const oatpp::Object<creatures::SoundDto> &a, const oatpp::Object<creatures::SoundDto> &b) {
                          std::string aLower = a->file_name;
                          std::string bLower = b->file_name;
                          std::transform(aLower.begin(), aLower.end(), aLower.begin(), ::tolower);
                          std::transform(bLower.begin(), bLower.end(), bLower.begin(), ::tolower);
                          return aLower < bLower;
                      });

            appLogger->debug("found {} sound files", soundList->size());

        } else {
            appLogger->warn("Sound file location not found: {}", path);

            status = Status::CODE_404;
            message = fmt::format("No files found in {}", path);
            error = true;
        }
    } catch (const fs::filesystem_error &e) {
        appLogger->error("Error reading sound file location: {}", e.what());

        status = Status::CODE_500;
        message = fmt::format("Error reading sound file location: {}", e.what());
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, message);

    // All done!
    auto list = ListDto<oatpp::Object<creatures::SoundDto>>::createShared();
    list->count = soundList->size();
    list->items = soundList;

    appLogger->debug("Returning {} sound files", list->count);
    return list;
}

/**
 * Schedule a sound to play on the next frame
 *
 * @param inSoundFile
 * @return a message telling which frame it will be played on
 */
oatpp::Object<creatures::ws::StatusDto> SoundService::playSound(const oatpp::String &inSoundFile) {

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    std::string soundFile = std::string(inSoundFile);

    appLogger->debug("Request to play sound file: {}", soundFile);

    // Fill out the full path to the file
    std::string fullFilePath = config->getSoundFileLocation() + "/" + inSoundFile;
    debug("using sound file name: {}", fullFilePath);

    // Make sure the file exists and is readable
    OATPP_ASSERT_HTTP(fileIsReadable(fullFilePath), Status::CODE_404,
                      fmt::format("Sound file not found: {}", soundFile));

    bool error = false;
    oatpp::String message;

    try {
        framenum_t frameNumber = eventLoop->getNextFrameNumber();

        // Create the event and schedule it
        auto playEvent = std::make_shared<MusicEvent>(frameNumber, fullFilePath);
        eventLoop->scheduleEvent(playEvent);

        debug("scheduled sound to play on frame {}", frameNumber);

        message = fmt::format("Scheduled {} for frame {}", soundFile, frameNumber);

    } catch (const creatures::InternalError &e) {
        message = fmt::format("Internal error: {}", e.what());
        appLogger->error(std::string(message));
        error = true;
    } catch (const creatures::DataFormatException &e) {
        message = fmt::format("Data format error: {}", e.what());
        appLogger->error(std::string(message));
        error = true;
    } catch (...) {
        message = fmt::format("Unknown error");
        appLogger->error(std::string(message));
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, Status::CODE_500, message)

    auto response = StatusDto::createShared();
    response->code = 200;
    response->message = message;
    response->status = "OK";

    debug("returning a 200");
    return response;
}

/**
 * Generate lip sync data for a sound file using Rhubarb Lip Sync
 *
 * @param inSoundFile The name of the sound file to process
 * @param allowOverwrite Whether to allow overwriting an existing JSON file
 * @param parentSpan Optional parent span for tracing
 * @return StatusDto with the result or error details
 */
oatpp::Object<creatures::ws::StatusDto> SoundService::generateLipSync(const oatpp::String &inSoundFile,
                                                                      bool allowOverwrite,
                                                                      std::shared_ptr<RequestSpan> parentSpan) {

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    std::string soundFile = std::string(inSoundFile);
    debug("generateLipSync() called for file: {}, allowOverwrite: {}", soundFile, allowOverwrite);

    if (!parentSpan) {
        warn("no parent span provided for SoundService.generateLipSync, creating a root span");
    }

    // Create operation span - connects to parent RequestSpan if provided
    auto span = creatures::observability->createOperationSpan("SoundService.generateLipSync", std::move(parentSpan));

    if (span) {
        span->setAttribute("sound.file", soundFile);
        span->setAttribute("allow_overwrite", allowOverwrite);
        debug("Created observability span for processSound");
    }

    auto response = StatusDto::createShared();

    // Get the sounds directory from config
    std::string soundsDir = config->getSoundFileLocation();
    debug("Sounds directory from config: {}", soundsDir);

    fs::path soundFilePath = fs::path(soundsDir) / soundFile;
    debug("Full sound file path: {}", soundFilePath.string());

    if (span) {
        span->setAttribute("sound.path", soundFilePath.string());
        span->setAttribute("sound.directory", soundsDir);
    }

    // 1. Check if the sound file exists
    debug("Checking if sound file exists: {}", soundFilePath.string());
    if (!fs::exists(soundFilePath)) {
        std::string errorMsg = fmt::format("Sound file '{}' not found in sounds directory '{}'", soundFile, soundsDir);
        warn(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        response->code = 404;
        response->status = "Not Found";
        response->message = errorMsg;
        return response;
    }
    debug("Sound file exists");

    // 2. Validate it's a WAV file
    debug("Checking file extension: {}", soundFilePath.extension().string());
    if (soundFilePath.extension() != ".wav") {
        std::string errorMsg = fmt::format("Sound file '{}' must be a WAV file (has extension '{}')", soundFile,
                                           soundFilePath.extension().string());
        warn(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        response->code = 422;
        response->status = "Unprocessable Entity";
        response->message = errorMsg;
        return response;
    }
    debug("File is a valid WAV file");

    // 3. Check for transcript file
    fs::path transcriptPath = soundFilePath;
    transcriptPath.replace_extension(".txt");
    debug("Checking for transcript file: {}", transcriptPath.string());
    bool hasTranscript = fs::exists(transcriptPath);

    if (span) {
        span->setAttribute("has_transcript", hasTranscript);
    }

    if (hasTranscript) {
        info("Found transcript file: {}", transcriptPath.filename().string());
    } else {
        debug("No transcript file found for {}", soundFile);
    }

    // 4. Check if JSON file already exists
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
            response->code = 422;
            response->status = "Unprocessable Entity";
            response->message = errorMsg;
            return response;
        }
        debug("Overwrite is allowed, will replace existing JSON file");
    } else {
        debug("JSON file does not exist, will create new file");
    }

    // 5. Build the Rhubarb command
    std::string rhubarbBinary = config->getRhubarbBinaryPath();
    debug("Rhubarb binary path from config: {}", rhubarbBinary);

    if (span) {
        span->setAttribute("rhubarb.binary", rhubarbBinary);
    }

    std::string command = fmt::format("{} -f json -o \"{}\"", rhubarbBinary, jsonOutputPath.string());
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

    // 6. Execute Rhubarb
    std::string commandOutput;
    int exitCode = 0;

    try {
        debug("Opening pipe to execute Rhubarb...");
        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe) {
            std::string errorMsg =
                fmt::format("Failed to execute Rhubarb binary at '{}'. "
                            "Is it installed and accessible? "
                            "Check the server configuration (--rhubarb-binary-path or RHUBARB_BINARY_PATH environment "
                            "variable). errno: {}",
                            rhubarbBinary, errno);
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            response->code = 500;
            response->status = "Internal Server Error";
            response->message = errorMsg;
            return response;
        }
        debug("Pipe opened successfully, reading output...");

        // Read command output
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
            std::string errorMsg =
                fmt::format("Rhubarb processing failed with exit code {}.\n\nCommand: {}\n\nOutput:\n{}", exitCode,
                            command, commandOutput);
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            response->code = 422;
            response->status = "Unprocessable Entity";
            response->message = errorMsg;
            return response;
        }

        info("Rhubarb processing completed successfully (exit code 0)");

    } catch (const std::exception &e) {
        std::string errorMsg = fmt::format("Exception during Rhubarb execution: {}\n\nCommand: {}\n\nOutput:\n{}",
                                           e.what(), command, commandOutput);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        response->code = 422;
        response->status = "Unprocessable Entity";
        response->message = errorMsg;
        return response;
    }

    // 7. Verify JSON file was created
    debug("Verifying JSON file was created: {}", jsonOutputPath.string());
    if (!fs::exists(jsonOutputPath)) {
        std::string errorMsg = fmt::format(
            "Rhubarb completed but did not create the expected JSON file '{}'.\n\nCommand: {}\n\nOutput:\n{}",
            jsonOutputPath.filename().string(), command, commandOutput);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        response->code = 422;
        response->status = "Unprocessable Entity";
        response->message = errorMsg;
        return response;
    }
    debug("JSON file created successfully");

    // 8. Read the JSON file, modify it to remove full path, and return its contents
    try {
        debug("Opening JSON file for reading: {}", jsonOutputPath.string());
        std::ifstream jsonFile(jsonOutputPath);
        if (!jsonFile.is_open()) {
            std::string errorMsg = fmt::format("Generated JSON file could not be read: {}", jsonOutputPath.string());
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            response->code = 500;
            response->status = "Internal Server Error";
            response->message = errorMsg;
            return response;
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
            response->code = 500;
            response->status = "Internal Server Error";
            response->message = errorMsg;
            return response;
        }

        // Strip the full path from soundFile, leave only the filename
        if (rhubarbJson.contains("metadata") && rhubarbJson["metadata"].contains("soundFile")) {
            rhubarbJson["metadata"]["soundFile"] = soundFile;
            debug("Stripped path from soundFile, now: {}", soundFile);
        }

        // Convert back to string
        std::string jsonContent = rhubarbJson.dump(2); // Pretty print with 2-space indent
        debug("JSON file processed successfully ({} bytes)", jsonContent.size());

        info("Successfully processed {} with Rhubarb, generated {} ({} bytes)", soundFile,
             jsonOutputPath.filename().string(), jsonContent.size());

        if (span) {
            span->setAttribute("json.output_file", jsonOutputPath.filename().string());
            span->setAttribute("json.output_size", static_cast<int64_t>(jsonContent.size()));
            span->setSuccess();
        }

        response->code = 200;
        response->status = "OK";
        response->message = jsonContent;
        debug("Returning success response with JSON content");
        return response;

    } catch (const std::exception &e) {
        std::string errorMsg = fmt::format("Error reading generated JSON file: {}", e.what());
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        response->code = 500;
        response->status = "Internal Server Error";
        response->message = errorMsg;
        return response;
    }
}

} // namespace creatures::ws