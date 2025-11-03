
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>

#include <fmt/format.h>

#include <nlohmann/json.hpp>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/config.h"
#include "server/database.h"

#include "server/ws/dto/AdHocSoundEntryDto.h"
#include "server/ws/dto/GenerateLipSyncRequestDto.h"
#include "server/ws/dto/GenerateLipSyncUploadResponseDto.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/PlaySoundRequestDTO.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/SoundService.h"

#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/metrics/counters.h"
#include "server/voice/LipSyncProcessor.h"
#include "server/voice/RhubarbData.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"

namespace fs = std::filesystem;

namespace creatures {
extern std::shared_ptr<creatures::Configuration> config;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures ::ws {

class SoundController : public oatpp::web::server::api::ApiController {
  public:
    SoundController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    SoundService m_soundService; // Create the sound service
  public:
    static std::shared_ptr<SoundController>
    createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                 objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<SoundController>(objectMapper);
    }

    ENDPOINT_INFO(getAllSounds) {
        info->summary = "Lists all of the sound files";

        info->addResponse<Object<SoundsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/sound", getAllSounds) {
        creatures::metrics->incrementRestRequestsProcessed();
        return createDtoResponse(Status::CODE_200, m_soundService.getAllSounds());
    }

    ENDPOINT_INFO(getAdHocSounds) {
        info->summary = "List ad-hoc/generated sound files";
        info->addResponse<Object<AdHocSoundListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/sound/ad-hoc", getAdHocSounds) {
        creatures::metrics->incrementRestRequestsProcessed();
        return createDtoResponse(Status::CODE_200, m_soundService.getAdHocSounds());
    }

    ENDPOINT_INFO(playSound) {
        info->summary = "Queue up a sound to play on the next frame";

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/sound/play", playSound,
             BODY_DTO(Object<creatures::ws::PlaySoundRequestDTO>, requestBody)) {
        creatures::metrics->incrementRestRequestsProcessed();
        return createDtoResponse(Status::CODE_200, m_soundService.playSound(std::string(requestBody->file_name)));
    }

    ENDPOINT_INFO(generateLipSyncFromUpload) {
        info->summary = "Generate lip sync data by uploading a WAV file";

        info->addTag("Lip Sync");
        info->addResponse<String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/sound/generate-lipsync/upload", generateLipSyncFromUpload, BODY_STRING(String, body),
             QUERY(String, filename, "filename"), REQUEST(std::shared_ptr<IncomingRequest>, request)) {

        auto span = creatures::observability->createRequestSpan("POST /api/v1/sound/generate-lipsync/upload", "POST",
                                                                "api/v1/sound/generate-lipsync/upload");

        creatures::metrics->incrementRestRequestsProcessed();
        info("REST call to generateLipSyncFromUpload");

        if (span) {
            span->setAttribute("endpoint", "generateLipSyncFromUpload");
            span->setAttribute("controller", "SoundController");
            span->setAttribute("http.method", "POST");
            span->setAttribute("http.target", "api/v1/sound/generate-lipsync/upload");
        }

        auto userAgent = request->getHeader("User-Agent");
        if (span && userAgent) {
            span->setAttribute("http.user_agent", std::string(userAgent));
        }

        if (!filename) {
            auto response = StatusDto::createShared();
            response->status = "Bad Request";
            response->message = "Query parameter 'filename' is required.";
            response->code = 400;
            if (span) {
                span->setHttpStatus(400);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_400, response);
        }

        if (!body) {
            auto response = StatusDto::createShared();
            response->status = "Bad Request";
            response->message = "Request body must contain WAV data.";
            response->code = 400;
            if (span) {
                span->setHttpStatus(400);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_400, response);
        }

        std::string wavData = body;
        if (wavData.empty()) {
            auto response = StatusDto::createShared();
            response->status = "Bad Request";
            response->message = "Uploaded WAV data is empty.";
            response->code = 400;
            if (span) {
                span->setHttpStatus(400);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_400, response);
        }

        std::string originalFilename = filename;
        std::string sanitizedFilename;
        try {
            sanitizedFilename = sanitizeFilename(originalFilename);
        } catch (const std::invalid_argument &ex) {
            auto response = StatusDto::createShared();
            response->status = "Bad Request";
            response->message = ex.what();
            response->code = 400;
            if (span) {
                span->setHttpStatus(400);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_400, response);
        }

        if (!sanitizedFilename.ends_with(".wav")) {
            auto response = StatusDto::createShared();
            response->status = "Unprocessable Entity";
            response->message = "Only .wav files are supported for lip sync generation.";
            response->code = 422;
            if (span) {
                span->setHttpStatus(422);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_422, response);
        }

        if (span) {
            span->setAttribute("upload.original_filename", originalFilename);
            span->setAttribute("upload.sanitized_filename", sanitizedFilename);
        }

        auto tempRoot = fs::temp_directory_path() / "creature-server" / "lipsync-uploads";
        auto tempDir = tempRoot / creatures::util::generateUUID();

        std::error_code ec;
        fs::create_directories(tempDir, ec);
        if (ec) {
            auto response = StatusDto::createShared();
            response->status = "Internal Server Error";
            response->message = fmt::format("Failed to create temporary directory: {}", ec.message());
            response->code = 500;
            if (span) {
                span->setHttpStatus(500);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_500, response);
        }

        struct TempDirCleaner {
            fs::path path;
            ~TempDirCleaner() {
                if (path.empty()) {
                    return;
                }
                std::error_code cleanupEc;
                fs::remove_all(path, cleanupEc);
                if (cleanupEc) {
                    warn("Failed to clean up temporary lip sync directory {}: {}", path.string(), cleanupEc.message());
                }
            }
        } cleanupGuard{tempDir};

        auto wavPath = tempDir / sanitizedFilename;
        std::ofstream wavFile(wavPath, std::ios::binary);
        if (!wavFile.is_open()) {
            auto response = StatusDto::createShared();
            response->status = "Internal Server Error";
            response->message = "Failed to write uploaded WAV file.";
            response->code = 500;
            if (span) {
                span->setHttpStatus(500);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_500, response);
        }

        wavFile.write(wavData.data(), static_cast<std::streamsize>(wavData.size()));
        if (!wavFile.good()) {
            auto response = StatusDto::createShared();
            response->status = "Internal Server Error";
            response->message = "Failed to persist uploaded WAV data.";
            response->code = 500;
            if (span) {
                span->setHttpStatus(500);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_500, response);
        }
        wavFile.close();

        if (span) {
            span->setAttribute("upload.filename", sanitizedFilename);
            span->setAttribute("upload.size_bytes", static_cast<int64_t>(wavData.size()));
            span->setAttribute("upload.temp_path", wavPath.string());
        }

        std::string rhubarbBinaryPath = creatures::config->getRhubarbBinaryPath();
        if (span) {
            span->setAttribute("rhubarb.binary", rhubarbBinaryPath);
        }

        auto result = creatures::voice::LipSyncProcessor::generateLipSync(sanitizedFilename, tempDir.string(),
                                                                          rhubarbBinaryPath, true, nullptr, nullptr);

        if (!result.isSuccess()) {
            auto errorResult = result.getError().value();
            auto response = StatusDto::createShared();
            response->status = "Lip Sync Generation Failed";
            response->message = errorResult.getMessage();
            int statusCode = serverErrorToStatusCode(errorResult.getCode());
            response->code = statusCode;
            auto status = Status::CODE_500;
            switch (statusCode) {
            case 400:
                status = Status::CODE_400;
                break;
            case 403:
                status = Status::CODE_403;
                break;
            case 404:
                status = Status::CODE_404;
                break;
            default:
                status = Status::CODE_500;
                break;
            }

            if (span) {
                span->setHttpStatus(status.code);
                span->setAttribute("error.message", errorResult.getMessage());
            }

            return createDtoResponse(status, response);
        }

        auto jsonContent = result.getValue().value();

        creatures::RhubarbSoundData lipSyncData;
        try {
            lipSyncData = creatures::RhubarbSoundData::fromJsonString(jsonContent);
        } catch (const std::exception &e) {
            auto response = StatusDto::createShared();
            response->status = "Internal Server Error";
            response->message = fmt::format("Failed to parse Rhubarb JSON output: {}", e.what());
            response->code = 500;
            if (span) {
                span->setHttpStatus(500);
                span->setAttribute("error.message", response->message);
            }
            return createDtoResponse(Status::CODE_500, response);
        }

        auto metadataDto = creatures::ws::RhubarbMetadataDto::createShared();
        metadataDto->soundFile = lipSyncData.metadata.soundFile;
        metadataDto->duration = lipSyncData.metadata.duration;

        auto mouthCuesDto = oatpp::List<oatpp::Object<creatures::ws::RhubarbMouthCueDto>>::createShared();
        for (const auto &cue : lipSyncData.mouthCues) {
            auto cueDto = creatures::ws::RhubarbMouthCueDto::createShared();
            cueDto->start = cue.start;
            cueDto->end = cue.end;
            cueDto->value = cue.value;
            mouthCuesDto->push_back(cueDto);
        }

        auto responseDto = creatures::ws::GenerateLipSyncUploadResponseDto::createShared();
        responseDto->metadata = metadataDto;
        responseDto->mouthCues = mouthCuesDto;

        auto jsonFilename = fmt::format("{}.json", sanitizedFilename.substr(0, sanitizedFilename.size() - 4));

        if (span) {
            span->setHttpStatus(200);
            span->setAttribute("json.size_bytes", static_cast<int64_t>(jsonContent.size()));
            span->setAttribute("json.filename", jsonFilename);
            span->setAttribute("json.mouth_cues", static_cast<int64_t>(lipSyncData.mouthCues.size()));
        }

        auto response = createDtoResponse(Status::CODE_200, responseDto);
        response->putHeader("Content-Type", "application/json; charset=utf-8");
        response->putHeader("Content-Disposition", fmt::format("attachment; filename=\"{}\"", jsonFilename));
        return response;
    }

    static std::string sanitizeFilename(const std::string &filename) {
        if (filename.empty() || filename.find('\0') != std::string::npos) {
            throw std::invalid_argument("Invalid filename: Empty or contains null bytes.");
        }

        fs::path candidate(filename);

        // Reject absolute paths, root references, or anything with parent components.
        if (candidate.is_absolute() || candidate.has_root_path() || candidate.has_parent_path() ||
            candidate != candidate.filename()) {
            throw std::invalid_argument("Invalid filename: Path traversal detected.");
        }

        // Keep legacy restriction to predictable ASCII names with extensions.
        static const std::regex validFilenameRegex("^[a-zA-Z0-9_-]+\\.[a-zA-Z0-9]+$");
        if (!std::regex_match(filename, validFilenameRegex)) {
            throw std::invalid_argument("Invalid filename: Contains unsafe characters.");
        }

        return filename;
    }

    static std::string getMimeType(const std::string &filename) {
        if (filename.ends_with(".mp3"))
            return "audio/mpeg";
        if (filename.ends_with(".wav"))
            return "audio/wav";
        if (filename.ends_with(".ogg"))
            return "audio/ogg";
        return "application/octet-stream"; // Default for unknown types
    }

    ENDPOINT_INFO(getSound) {
        info->summary = "Retrieve a sound file";

        info->addResponse<String>(Status::CODE_200, "audio/mpeg");
        info->addResponse<String>(Status::CODE_200, "audio/ogg");
        info->addResponse<String>(Status::CODE_200, "audio/wav");
        info->addResponse<String>(Status::CODE_200, "application/octet-stream");
        info->addResponse<Object<StatusDto>>(Status::CODE_403, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "/api/v1/sound/{filename}", getSound, PATH(String, filename)) {

        debug("Request to serve sound file: {}", std::string(filename));
        creatures::metrics->incrementRestRequestsProcessed();

        // Sanitize the filename
        std::string safeFilename;
        try {
            safeFilename = sanitizeFilename(filename);
        } catch (const std::invalid_argument &e) {

            warn("Attempt to serve {} failed: {}", std::string(filename), e.what());

            auto response = StatusDto::createShared();
            response->status = "Forbidden";
            response->message = e.what();
            response->code = 403;
            return createDtoResponse(Status::CODE_403, response);
        }

        // Assemble the path
        auto filePath = config->getSoundFileLocation() + "/" + safeFilename;

        // Resolve the canonical path
        std::filesystem::path canonicalPath;
        std::filesystem::path baseDir;
        try {
            // Try to resolve canonical paths
            canonicalPath = std::filesystem::canonical(filePath);
            baseDir = std::filesystem::canonical(config->getSoundFileLocation());
        } catch (const std::filesystem::filesystem_error &e) {

            warn("Attempt to serve {} failed: {}", std::string(filename), e.what());

            auto response = StatusDto::createShared();
            response->status = "Not Found";
            response->message = e.what();
            response->code = 404;
            return createDtoResponse(Status::CODE_404, response);
        }

        // Ensure the resolved path is within the base directory
        if (canonicalPath.string().find(baseDir.string()) != 0) {

            warn("Attempt to serve {} failed: {}", std::string(filename), "Path traversal attempt.");

            auto response = StatusDto::createShared();
            response->status = "Forbidden";
            response->message = "Forbidden: Path traversal attempt.";
            response->code = 403;
            return createDtoResponse(Status::CODE_403, response);
        }

        // Open the file
        std::ifstream file(canonicalPath.string(), std::ios::binary | std::ios::ate);
        if (!file.is_open()) {

            info("Attempt to serve {} failed: {}", std::string(filename), "Not found.");

            auto response = StatusDto::createShared();
            response->status = "Not Found";
            response->message = "File not found.";
            response->code = 404;
            return createDtoResponse(Status::CODE_404, response);
        }

        // Get file size
        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg); // Go back to the start of the file

        // Get MIME type
        auto mimeType = getMimeType(filePath);

        // Read file content
        std::vector<char> buffer(fileSize);
        if (!file.read(buffer.data(), fileSize)) {

            auto response = StatusDto::createShared();
            response->status = "Internal Server Error";
            response->message = "Error reading file.";
            response->code = 500;
            return createDtoResponse(Status::CODE_500, response);
        }

        // Increment metrics and log
        creatures::metrics->incrementSoundFilesServed();
        info("Serving sound file: {} ({}, {} bytes)", std::string(filename), mimeType, fileSize);

        // Create response
        auto response =
            ResponseFactory::createResponse(Status::CODE_200, oatpp::String((const char *)buffer.data(), fileSize));
        response->putHeader("Content-Type", mimeType.c_str());
        // content-length is automatically added by oatpp
        return response;
    }

    ENDPOINT_INFO(getAdHocSound) {
        info->summary = "Retrieve an ad-hoc generated sound file";
        info->addResponse<String>(Status::CODE_200, "audio/wav");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/sound/ad-hoc/{filename}", getAdHocSound, PATH(String, filename)) {
        creatures::metrics->incrementRestRequestsProcessed();

        std::string safeFilename;
        try {
            safeFilename = sanitizeFilename(filename);
        } catch (const std::invalid_argument &e) {
            auto response = StatusDto::createShared();
            response->status = "Forbidden";
            response->message = e.what();
            response->code = 403;
            return createDtoResponse(Status::CODE_403, response);
        }

        std::string filePath;
        try {
            filePath = m_soundService.resolveAdHocSoundPath(safeFilename);
        } catch (oatpp::web::protocol::http::HttpError &err) {
            auto info = err.getInfo();
            auto response = StatusDto::createShared();
            response->status = std::string(Status(info.status).description);
            response->message = err.getMessage();
            response->code = info.status.code;
            return createDtoResponse(Status(info.status), response);
        }

        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            auto response = StatusDto::createShared();
            response->status = "Not Found";
            response->message = "File not found.";
            response->code = 404;
            return createDtoResponse(Status::CODE_404, response);
        }

        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(fileSize);
        if (!file.read(buffer.data(), fileSize)) {
            auto response = StatusDto::createShared();
            response->status = "Internal Server Error";
            response->message = "Error reading file.";
            response->code = 500;
            return createDtoResponse(Status::CODE_500, response);
        }

        auto mimeType = getMimeType(filePath);
        auto response =
            ResponseFactory::createResponse(Status::CODE_200, oatpp::String((const char *)buffer.data(), fileSize));
        response->putHeader("Content-Type", mimeType.c_str());
        response->putHeader("Content-Disposition", "attachment; filename=\"" + safeFilename + "\"");
        return response;
    }

    ENDPOINT_INFO(generateLipSync) {
        info->summary = "Generate lip sync data for a sound file using Rhubarb Lip Sync (async job)";

        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/sound/generate-lipsync", generateLipSync,
             BODY_DTO(Object<creatures::ws::GenerateLipSyncRequestDto>, requestBody),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {

        // Create a trace span for this request
        auto span = creatures::observability->createRequestSpan("POST /api/v1/sound/generate-lipsync", "POST",
                                                                "api/v1/sound/generate-lipsync");

        info("REST call to generateLipSync (async)");
        creatures::metrics->incrementRestRequestsProcessed();

        if (span) {
            span->setAttribute("endpoint", "generateLipSync");
            span->setAttribute("controller", "SoundController");
            span->setAttribute("http.method", "POST");
            span->setAttribute("http.target", "api/v1/sound/generate-lipsync");

            // Add User-Agent if present
            auto userAgent = request->getHeader("User-Agent");
            if (userAgent) {
                span->setAttribute("http.user_agent", std::string(userAgent));
            }

            // Add request details
            span->setAttribute("sound.file", std::string(requestBody->sound_file));
        }

        std::string soundFile = std::string(requestBody->sound_file);
        bool allowOverwrite = requestBody->allow_overwrite ? static_cast<bool>(requestBody->allow_overwrite) : false;

        // Create a job for the lip sync processing
        // Encode details as JSON to include both filename and allow_overwrite flag
        nlohmann::json jobDetails;
        jobDetails["sound_file"] = soundFile;
        jobDetails["allow_overwrite"] = allowOverwrite;
        std::string jobDetailsStr = jobDetails.dump();

        debug("Creating lip sync job for sound file: {}, allow_overwrite: {}", soundFile, allowOverwrite);
        std::string jobId = creatures::jobManager->createJob(creatures::jobs::JobType::LipSync, jobDetailsStr);
        info("Created lip sync job with ID: {}", jobId);

        if (span) {
            span->setAttribute("job.id", jobId);
        }

        // Queue the job for processing
        debug("Queueing job {} for processing", jobId);
        creatures::jobWorker->queueJob(jobId);
        info("Job {} queued successfully", jobId);

        // Create the response DTO
        auto response = JobCreatedDto::createShared();
        response->job_id = jobId;
        response->job_type = "lip-sync";
        response->message = fmt::format(
            "Lip sync job created for '{}'. Listen for job-progress and job-complete WebSocket messages.", soundFile);

        if (span) {
            span->setHttpStatus(202);
            span->setAttribute("success", true);
        }

        debug("Returning 202 Accepted with job ID: {}", jobId);
        return createDtoResponse(Status::CODE_202, response);
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen
