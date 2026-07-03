
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
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/ws/dto/AdHocSoundEntryDto.h"
#include "server/ws/dto/GenerateLipSyncRequestDto.h"
#include "server/ws/dto/GenerateLipSyncUploadResponseDto.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/PlaySoundRequestDTO.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/SoundService.h"

#include "server/audio/MonoWavDownmixer.h"
#include "server/audio/OggOpusWriter.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/metrics/counters.h"
#include "server/voice/LipSyncProcessor.h"
#include "server/voice/RhubarbData.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "util/Result.h"
#include "util/uuidUtils.h"

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

class SoundController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<SoundController> {
  public:
    SoundController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

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
        info->addTag("Sounds");

        info->addResponse<Object<SoundsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/sound", getAllSounds, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/sound", "GET", "api/v1/sound", "getAllSounds", "SoundController", request,
                           [&](const auto &span) {
                               const auto result = m_soundService.getAllSounds();
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(getAdHocSounds) {
        info->summary = "List ad-hoc/generated sound files";
        info->addTag("Sounds");
        info->addResponse<Object<AdHocSoundListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/sound/ad-hoc", getAdHocSounds, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/sound/ad-hoc", "GET", "api/v1/sound/ad-hoc", "getAdHocSounds",
                           "SoundController", request, [&](const auto &span) {
                               const auto result = m_soundService.getAdHocSounds();
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(playSound) {
        info->summary = "Queue up a sound to play on the next frame";
        info->addTag("Sounds");

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/sound/play", playSound, BODY_DTO(Object<creatures::ws::PlaySoundRequestDTO>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/sound/play", "POST", "api/v1/sound/play", "playSound", "SoundController",
                           request, [&](const auto &span) {
                               if (span && requestBody && requestBody->file_name) {
                                   span->setAttribute("sound.file", std::string(requestBody->file_name));
                               }
                               const auto result = m_soundService.playSound(std::string(requestBody->file_name));
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(generateLipSyncFromUpload) {
        info->summary = "Generate lip sync data by uploading a WAV file";
        info->addTag("Sounds");

        info->addResponse<String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/sound/generate-lipsync/upload", generateLipSyncFromUpload, BODY_STRING(String, body),
             QUERY(String, filename, "filename"), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        info("REST call to generateLipSyncFromUpload");
        return runEndpoint(
            "POST /api/v1/sound/generate-lipsync/upload", "POST", "api/v1/sound/generate-lipsync/upload",
            "generateLipSyncFromUpload", "SoundController", request, [&](const auto &span) {
                if (!filename) {
                    return bailHttp(span, Status::CODE_400, "Query parameter 'filename' is required.");
                }

                if (!body) {
                    return bailHttp(span, Status::CODE_400, "Request body must contain WAV data.");
                }

                std::string wavData = body;
                if (wavData.empty()) {
                    return bailHttp(span, Status::CODE_400, "Uploaded WAV data is empty.");
                }

                std::string originalFilename = filename;
                std::string sanitizedFilename;
                try {
                    sanitizedFilename = sanitizeFilename(originalFilename);
                } catch (const std::invalid_argument &ex) {
                    return bailHttp(span, Status::CODE_400, ex.what());
                }

                if (!sanitizedFilename.ends_with(".wav")) {
                    return bailHttp(span, Status::CODE_422, "Only .wav files are supported for lip sync generation.");
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
                    return bailHttp(span, Status::CODE_500,
                                    fmt::format("Failed to create temporary directory: {}", ec.message()));
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
                            warn("Failed to clean up temporary lip sync directory {}: {}", path.string(),
                                 cleanupEc.message());
                        }
                    }
                } cleanupGuard{tempDir};

                auto wavPath = tempDir / sanitizedFilename;
                std::ofstream wavFile(wavPath, std::ios::binary);
                if (!wavFile.is_open()) {
                    return bailHttp(span, Status::CODE_500, "Failed to write uploaded WAV file.");
                }

                wavFile.write(wavData.data(), static_cast<std::streamsize>(wavData.size()));
                if (!wavFile.good()) {
                    return bailHttp(span, Status::CODE_500, "Failed to persist uploaded WAV data.");
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

                auto result = voice::LipSyncProcessor::generateLipSync(sanitizedFilename, tempDir.string(),
                                                                       rhubarbBinaryPath, true, nullptr, nullptr);

                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }

                auto jsonContent = result.getValue().value();

                RhubarbSoundData lipSyncData;
                try {
                    lipSyncData = creatures::RhubarbSoundData::fromJsonString(jsonContent);
                } catch (const std::exception &e) {
                    return bailHttp(span, Status::CODE_500,
                                    fmt::format("Failed to parse Rhubarb JSON output: {}", e.what()));
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

                auto responseDto = GenerateLipSyncUploadResponseDto::createShared();
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
            });
    }

    static std::string sanitizeFilename(const std::string &filename) {
        if (filename.empty() || filename.find('\0') != std::string::npos) {
            throw std::invalid_argument("Invalid filename: Empty or contains null bytes.");
        }

        // Reject absolute paths, root references, or anything with parent components.
        if (const fs::path candidate(filename); candidate.is_absolute() || candidate.has_root_path() ||
                                                candidate.has_parent_path() || candidate != candidate.filename()) {
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
        info->addTag("Sounds");

        info->addResponse<String>(Status::CODE_200, "audio/mpeg");
        info->addResponse<String>(Status::CODE_200, "audio/ogg");
        info->addResponse<String>(Status::CODE_200, "audio/wav");
        info->addResponse<String>(Status::CODE_200, "application/octet-stream");
        info->addResponse<Object<StatusDto>>(Status::CODE_403, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "/api/v1/sound/{filename}", getSound, PATH(String, filename),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("Request to serve sound file: {}", std::string(filename));
        return runEndpoint("GET /api/v1/sound/{filename}", "GET", "/api/v1/sound/" + std::string(filename), "getSound",
                           "SoundController", request, [&](const auto &span) {
                               // Sanitize the filename
                               std::string safeFilename;
                               try {
                                   safeFilename = sanitizeFilename(filename);
                               } catch (const std::invalid_argument &e) {
                                   warn("Attempt to serve {} failed: {}", std::string(filename), e.what());
                                   return bailHttp(span, Status::CODE_403, e.what());
                               }

                               // Resolve via the service: top-level first, then a recursive
                               // basename search so dialog/ renders resolve too (#46). Throws
                               // an HTTP error (404/400) that withSpanStatus stamps.
                               std::string canonicalPath = m_soundService.resolvePermanentSoundPath(safeFilename);

                               std::ifstream file(canonicalPath, std::ios::binary | std::ios::ate);
                               if (!file.is_open()) {
                                   info("Attempt to serve {} failed: {}", std::string(filename), "Not found.");
                                   return bailHttp(span, Status::CODE_404, "File not found.");
                               }

                               std::streamsize fileSize = file.tellg();
                               file.seekg(0, std::ios::beg);
                               auto mimeType = getMimeType(canonicalPath);

                               std::vector<char> buffer(fileSize);
                               if (!file.read(buffer.data(), fileSize)) {
                                   return bailHttp(span, Status::CODE_500, "Error reading file.");
                               }

                               metrics->incrementSoundFilesServed();
                               info("Serving sound file: {} ({}, {} bytes)", std::string(filename), mimeType, fileSize);

                               auto response = ResponseFactory::createResponse(
                                   Status::CODE_200, oatpp::String((const char *)buffer.data(), fileSize));
                               response->putHeader("Content-Type", mimeType.c_str());
                               if (span)
                                   span->setHttpStatus(200);
                               return response;
                           });
    }

    ENDPOINT_INFO(getAdHocSound) {
        info->summary = "Retrieve an ad-hoc generated sound file";
        info->addTag("Sounds");
        info->addResponse<String>(Status::CODE_200, "audio/wav");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/sound/ad-hoc/{filename}", getAdHocSound, PATH(String, filename),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/sound/ad-hoc/{filename}", "GET", "api/v1/sound/ad-hoc/" + std::string(filename),
                           "getAdHocSound", "SoundController", request, [&](const auto &span) {
                               std::string safeFilename;
                               try {
                                   safeFilename = sanitizeFilename(filename);
                               } catch (const std::invalid_argument &e) {
                                   return bailHttp(span, Status::CODE_403, e.what());
                               }

                               std::string filePath;
                               try {
                                   filePath = m_soundService.resolveAdHocSoundPath(safeFilename);
                               } catch (oatpp::web::protocol::http::HttpError &err) {
                                   // withSpanStatus catches HttpError and stamps the right code; rethrow.
                                   throw;
                               }

                               std::ifstream file(filePath, std::ios::binary | std::ios::ate);
                               if (!file.is_open()) {
                                   return bailHttp(span, Status::CODE_404, "File not found.");
                               }

                               std::streamsize fileSize = file.tellg();
                               file.seekg(0, std::ios::beg);

                               std::vector<char> buffer(fileSize);
                               if (!file.read(buffer.data(), fileSize)) {
                                   return bailHttp(span, Status::CODE_500, "Error reading file.");
                               }

                               auto mimeType = getMimeType(filePath);
                               auto response = ResponseFactory::createResponse(
                                   Status::CODE_200, oatpp::String((const char *)buffer.data(), fileSize));
                               response->putHeader("Content-Type", mimeType.c_str());
                               response->putHeader("Content-Disposition",
                                                   "attachment; filename=\"" + safeFilename + "\"");
                               if (span)
                                   span->setHttpStatus(200);
                               return response;
                           });
    }

    ENDPOINT_INFO(getShareableSound) {
        info->summary = "Encode a sound file to Ogg/Opus for sharing (downmixed to mono)";
        info->description = "Looks for the file in the permanent sound store first, then the ad-hoc store. "
                            "Multi-channel WAVs are downmixed to mono before encoding.";
        info->addTag("Sounds");
        info->addResponse<String>(Status::CODE_200, "audio/ogg");
        info->addResponse<Object<StatusDto>>(Status::CODE_403, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_422, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/sound/shareable/{filename}", getShareableSound, PATH(String, filename),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/sound/shareable/{filename}", "GET", "api/v1/sound/shareable/" + std::string(filename),
            "getShareableSound", "SoundController", request, [&](const auto &span) {
                std::string safeFilename;
                try {
                    safeFilename = sanitizeFilename(filename);
                } catch (const std::invalid_argument &e) {
                    warn("Attempt to share {} failed: {}", std::string(filename), e.what());
                    return bailHttp(span, Status::CODE_403, e.what());
                }

                // Permanent store first (top-level, then a recursive basename search
                // so dialog/ renders resolve — #46), then the ad-hoc bucket. Same
                // resolution rules as getSound / getAdHocSound.
                std::string sourcePath;
                try {
                    sourcePath = m_soundService.resolvePermanentSoundPath(safeFilename);
                } catch (oatpp::web::protocol::http::HttpError &) {
                    // Not in the permanent store; fall through to ad-hoc.
                }
                if (sourcePath.empty()) {
                    try {
                        sourcePath = m_soundService.resolveAdHocSoundPath(safeFilename);
                    } catch (oatpp::web::protocol::http::HttpError &) {
                        return bailHttp(span, Status::CODE_404,
                                        fmt::format("Sound '{}' was not found in the sound store or the ad-hoc store",
                                                    safeFilename));
                    }
                }
                if (span) {
                    span->setAttribute("sound.source_path", sourcePath);
                }

                auto monoResult = creatures::audio::loadWavAsMono(sourcePath);
                if (!monoResult.isSuccess()) {
                    const auto error = monoResult.getError().value();
                    const auto status = error.getCode() == ServerError::NotFound ? Status::CODE_404 : Status::CODE_422;
                    return bailHttp(span, status, error.getMessage());
                }
                const auto mono = monoResult.getValue().value();

                auto oggResult = creatures::audio::encodeMonoToOggOpus(mono.samples, mono.sampleRate);
                if (!oggResult.isSuccess()) {
                    const auto error = oggResult.getError().value();
                    const auto status =
                        error.getCode() == ServerError::InvalidData ? Status::CODE_422 : Status::CODE_500;
                    return bailHttp(span, status, error.getMessage());
                }
                const auto oggBytes = oggResult.getValue().value();

                // foo.wav → foo.ogg
                const auto dot = safeFilename.rfind('.');
                const auto shareName = (dot == std::string::npos ? safeFilename : safeFilename.substr(0, dot)) + ".ogg";

                metrics->incrementSoundFilesServed();
                info("Sharing sound file: {} → {} ({} bytes of Ogg/Opus)", safeFilename, shareName, oggBytes.size());

                auto response = ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(oggBytes.data()),
                                                    static_cast<v_buff_size>(oggBytes.size())));
                response->putHeader("Content-Type", "audio/ogg");
                response->putHeader("Content-Disposition", "attachment; filename=\"" + shareName + "\"");
                if (span) {
                    span->setAttribute("share.bytes", static_cast<int64_t>(oggBytes.size()));
                    span->setHttpStatus(200);
                }
                return response;
            });
    }

    ENDPOINT_INFO(generateLipSync) {
        info->summary = "Generate lip sync data for a sound file using Rhubarb Lip Sync (async job)";
        info->addTag("Sounds");

        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/sound/generate-lipsync", generateLipSync,
             BODY_DTO(Object<creatures::ws::GenerateLipSyncRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        info("REST call to generateLipSync (async)");
        return runEndpoint(
            "POST /api/v1/sound/generate-lipsync", "POST", "api/v1/sound/generate-lipsync", "generateLipSync",
            "SoundController", request, [&](const auto &span) {
                if (span) {
                    span->setAttribute("sound.file", std::string(requestBody->sound_file));
                }

                auto soundFile = std::string(requestBody->sound_file);
                bool allowOverwrite =
                    requestBody->allow_overwrite ? static_cast<bool>(requestBody->allow_overwrite) : false;

                // Encode details as JSON to include both filename and allow_overwrite flag
                nlohmann::json jobDetails;
                jobDetails["sound_file"] = soundFile;
                jobDetails["allow_overwrite"] = allowOverwrite;
                const std::string jobDetailsStr = jobDetails.dump();

                debug("Creating lip sync job for sound file: {}, allow_overwrite: {}", soundFile, allowOverwrite);
                std::string jobId =
                    creatures::jobManager->createJob(creatures::jobs::JobType::LipSync, jobDetailsStr, span);
                info("Created lip sync job with ID: {}", jobId);

                if (span) {
                    span->setAttribute("job.id", jobId);
                }

                debug("Queueing job {} for processing", jobId);
                creatures::jobWorker->queueJob(jobId);
                info("Job {} queued successfully", jobId);

                const auto response = JobCreatedDto::createShared();
                response->job_id = jobId;
                response->job_type = "lip-sync";
                response->message = fmt::format("Lip sync job created for '{}'. Listen for job-progress and "
                                                "job-complete WebSocket messages.",
                                                soundFile);

                if (span) {
                    span->setHttpStatus(202);
                }

                debug("Returning 202 Accepted with job ID: {}", jobId);
                return createDtoResponse(Status::CODE_202, response);
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen
