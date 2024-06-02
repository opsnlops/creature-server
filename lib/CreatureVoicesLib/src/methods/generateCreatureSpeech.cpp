
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>


#include "model/CreatureSpeechResponse.h"
#include "model/CreatureSpeechRequest.h"
#include "model/HttpMethod.h"
#include "VoiceResult.h"
#include "CreatureVoices.h"

using json = nlohmann::json;

namespace creatures::voice {

    /*
     * Notes:
     *
     * This is defined in: https://elevenlabs.io/docs/api-reference/text-to-speech
     *
     */


    VoiceResult<CreatureSpeechResponse> CreatureVoices::generateCreatureSpeech(const std::filesystem::path &fileSavePath,
                                                                               const CreatureSpeechRequest &speechRequest) {

        // Before we get going, let's validate our inputs. Since the API charges per character, we should make sure
        // that everything is good to go.
        if(fileSavePath.empty()) {
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, "File save path cannot be empty")};
        }

        if(speechRequest.text.empty()) {
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, "Text cannot be empty")};
        }

        if(speechRequest.voice_id.empty()) {
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, "Voice ID cannot be empty")};
        }

        if(speechRequest.model_id.empty()) {
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, "Model ID cannot be empty")};
        }

        if(speechRequest.stability < 0 || speechRequest.stability > 1) {
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, "Stability must be between 0 and 1")};
        }

        if(speechRequest.similarity_boost < 0 || speechRequest.similarity_boost > 1) {
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, "Similarity boost must be between 0 and 1")};
        }


        // Make sure that the file path exists and can be written to
        if (!std::filesystem::exists(fileSavePath)) {
            std::string errorMessage = fmt::format("File path does not exist: {}", fileSavePath.string());
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, errorMessage)};
        }

        if (!std::filesystem::is_directory(fileSavePath)) {
            std::string errorMessage = fmt::format("File path is not a directory: {}", fileSavePath.string());
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, errorMessage)};
        }

        // Let's test that it's writable by writing out the transcript
        auto fileBaseName = makeFileName(speechRequest);
        auto transcriptPath = fileSavePath / fmt::format("{}.txt", fileBaseName);

        info("Transcript path: {}", transcriptPath.string());

        std::ofstream transcriptFile(transcriptPath);
        if (!transcriptFile.is_open()) {
            std::string errorMessage = fmt::format("Failed to open transcript file for writing: {}", transcriptPath.string());
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, errorMessage)};
        }

        transcriptFile << speechRequest.text;
        if(!transcriptFile.good()) {
            std::string errorMessage = fmt::format("Failed to write to transcript file: {}", transcriptPath.string());
            transcriptFile.close();
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InvalidData, errorMessage)};
        }
        transcriptFile.close();


        // If we've made it this far, we're good to go. Let's generate the sound file.
        auto soundFilePath = fileSavePath / fmt::format("{}.mp3", fileBaseName);
        debug("Generating speech for creature: {} to file {}", speechRequest.creature_name, soundFilePath.string());



        const std::string url = fmt::format("/v1/text-to-speech/{}", speechRequest.voice_id);
        auto curlHandle = createCurlHandle(url);
        curlHandle.addHeader("Content-Type: application/json");
        curlHandle.addHeader("Accept: audio/mpeg");

        // Create the JSON request according to the API
        json requestJson = {
                {"text", speechRequest.text},
                {"model_id", speechRequest.model_id},
                {"voice_settings", {
                    {"stability", speechRequest.stability},
                    {"similarity_boost", speechRequest.similarity_boost}
                }}
        };
        std::string requestBody = requestJson.dump();
        debug("Request body: {}", requestBody);


        auto result = performRequest(curlHandle, apiKey, HttpMethod::POST, requestBody);
        if(!result.isSuccess()) {
            auto error = result.getError();
            std::string errorMessage = fmt::format("Unable to generate audio file: {}", error->getMessage());
            return VoiceResult<CreatureSpeechResponse>{VoiceError(error->getCode(), error->getMessage())};
        }

        // Load the sound file into memory
        auto httpResponse = result.getValue().value();
        debug("loaded {} bytes of audio data", httpResponse.size());

        debug("attempting to open file: {}", soundFilePath.string());
        std::ofstream soundFile(soundFilePath, std::ios::binary);
        if (!soundFile.is_open()) {
            std::string errorMessage = fmt::format("Failed to open sound file for writing: {}", soundFilePath.string());
            error(errorMessage);
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InternalError, errorMessage)};
        }

        debug("writing sound data...");
        soundFile << httpResponse;
        if(!soundFile.good()) {
            std::string errorMessage = fmt::format("Failed to write to sound file: {}", soundFilePath.string());
            error(errorMessage);
            soundFile.close();
            return VoiceResult<CreatureSpeechResponse>{VoiceError(VoiceError::InternalError, errorMessage)};
        }
        soundFile.close();
        debug("done! sound file written to {}", soundFilePath.string());

        CreatureSpeechResponse response;
        response.sound_file_name = soundFilePath.string();
        response.transcript_file_name = transcriptPath.string();
        response.sound_file_size = httpResponse.size();
        response.success = true;

        debug("woo! all done!");
        return VoiceResult<CreatureSpeechResponse>{response};
    }


    std::string CreatureVoices::makeFileName(const CreatureSpeechRequest &speechRequest) {
        std::string fileName;

        // Start with the creature's name, if it exists, in all lower case
        if (!speechRequest.creature_name.empty()) {
            fileName = toLowerAndReplaceSpaces(speechRequest.creature_name);
        } else {
            // If there's no creature name, use the voice ID
            fileName = speechRequest.voice_id;
        }

        // Add a file-system save timestamp that'll sort nicely
        auto now = std::chrono::system_clock::now();
        auto nowTime = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&nowTime), "%Y-%m-%d_%H-%M-%S");
        fileName += fmt::format("_{}", oss.str());

        // Add the title, if it exists, in all lower case, and spaces turned into dashes
        if (!speechRequest.title.empty()) {
            fileName += fmt::format("_{}", toLowerAndReplaceSpaces(speechRequest.title));
        } else {
            // If there's no title, use the model ID
            fileName += fmt::format("_{}", speechRequest.model_id);
        }

        // Ensure characters are valid for a file name
        std::replace(fileName.begin(), fileName.end(), ' ', '_');
        std::replace(fileName.begin(), fileName.end(), '/', '_');
        std::replace(fileName.begin(), fileName.end(), '\\', '_');

        spdlog::debug("Generated file name: {}", fileName);

        return fileName;
    }


    // Utility function to convert a string to lowercase and replace spaces
    std::string CreatureVoices::toLowerAndReplaceSpaces(std::string str) {
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        std::replace(str.begin(), str.end(), ' ', '-');
        return str;
    }
}
