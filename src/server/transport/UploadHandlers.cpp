#include "server/transport/UploadHandlers.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "api/JsonResponse.h"
#include "api/SoundRequests.h"
#include "api/VoiceContracts.h"
#include "server/audio/SoundPathResolver.h"
#include "server/config/Configuration.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/transport/HandlerSupport.h"
#include "server/voice/LipSyncProcessor.h"
#include "server/voice/RhubarbData.h"
#include "server/voice/WhisperLipSyncProcessor.h"
#include "util/ObservabilityManager.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

namespace creatures::transport {
namespace fs = std::filesystem;

PreparedResponse generateLipSync(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parseBody(body, "sound.lipsync", "lip sync request", api::generateLipSyncRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto soundFile = parsed.getValue()->soundFile;
    const bool allowOverwrite = parsed.getValue()->allowOverwrite;
    if (!audio::isSafeSoundFilename(soundFile))
        return errorStatus(400, "sound_file must be a safe filename without path components", span,
                           "InvalidSoundFilename");
    std::string extension = fs::path(soundFile).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (extension != ".wav")
        return errorStatus(422, "sound_file must name a WAV file", span, "InvalidSoundFilename");
    if (!creatures::jobManager || !creatures::jobWorker)
        return errorStatus(500, "Job processing is unavailable", span, "MissingDependencies");
    if (span) {
        span->setAttribute("sound.file", audio::sanitizeForLogging(soundFile));
        span->setAttribute("allow_overwrite", allowOverwrite);
    }
    nlohmann::json jobDetails;
    jobDetails["sound_file"] = soundFile;
    jobDetails["allow_overwrite"] = allowOverwrite;
    const auto jobId = creatures::jobManager->createJob(jobs::JobType::LipSync, jobDetails.dump(), span);
    creatures::jobWorker->queueJob(jobId);
    spdlog::info("Created and queued lip sync job {} for {}", jobId, audio::sanitizeForLogging(soundFile));
    if (span) {
        span->setAttribute("job.id", jobId);
        span->setSuccess();
    }
    const nlohmann::json response = {{"job_id", jobId},
                                     {"job_type", "lip-sync"},
                                     {"message", fmt::format("Lip sync job created for '{}'. Listen for job-progress "
                                                             "and job-complete WebSocket messages.",
                                                             soundFile)}};
    return PreparedResponse::json(202, api::jsonToString(response));
}

PreparedResponse generateLipSyncFromUpload(const std::string &filename, const std::string &wavData,
                                           const std::shared_ptr<OperationSpan> &span) {
    if (filename.empty())
        return errorStatus(400, "Query parameter 'filename' is required.", span, "MissingFilename");
    if (wavData.empty())
        return errorStatus(400, "Uploaded WAV data is empty.", span, "EmptyUpload");
    const auto sanitized = sanitizeSoundFilename(filename);
    if (!sanitized.isSuccess())
        return errorStatus(400, sanitized.getError()->getMessage(), span, "InvalidFilename");
    const auto sanitizedFilename = sanitized.getValue().value();
    if (!sanitizedFilename.ends_with(".wav"))
        return errorStatus(422, "Only .wav files are supported for lip sync generation.", span, "InvalidFilename");
    if (!creatures::config)
        return errorStatus(500, "Lip sync unavailable: configuration missing", span, "MissingDependencies");
    if (span) {
        span->setAttribute("upload.original_filename", filename);
        span->setAttribute("upload.sanitized_filename", sanitizedFilename);
    }

    const auto tempDir = fs::temp_directory_path() / "creature-server" / "lipsync-uploads" / util::generateUUID();
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    if (ec)
        return errorStatus(500, fmt::format("Failed to create temporary directory: {}", ec.message()), span,
                           "TempDirFailure");
    struct TempDirCleaner {
        fs::path path;
        ~TempDirCleaner() {
            std::error_code cleanupEc;
            fs::remove_all(path, cleanupEc);
            if (cleanupEc)
                spdlog::warn("Failed to clean up temporary lip sync directory {}: {}", path.string(),
                             cleanupEc.message());
        }
    } cleanupGuard{tempDir};

    const auto wavPath = tempDir / sanitizedFilename;
    {
        std::ofstream wavFile(wavPath, std::ios::binary);
        if (!wavFile.is_open())
            return errorStatus(500, "Failed to write uploaded WAV file.", span, "UploadWriteFailure");
        wavFile.write(wavData.data(), static_cast<std::streamsize>(wavData.size()));
        if (!wavFile.good())
            return errorStatus(500, "Failed to persist uploaded WAV data.", span, "UploadWriteFailure");
    }
    if (span) {
        span->setAttribute("upload.filename", sanitizedFilename);
        span->setAttribute("upload.size_bytes", static_cast<int64_t>(wavData.size()));
        span->setAttribute("upload.temp_path", wavPath.string());
    }
    const auto rhubarbBinaryPath = creatures::config->getRhubarbBinaryPath();
    if (span)
        span->setAttribute("rhubarb.binary", rhubarbBinaryPath);
    const auto result = voice::LipSyncProcessor::generateLipSync(sanitizedFilename, tempDir.string(), rhubarbBinaryPath,
                                                                 true, nullptr, span);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    const auto jsonContent = result.getValue().value();
    RhubarbSoundData lipSyncData;
    try {
        lipSyncData = RhubarbSoundData::fromJsonString(jsonContent);
    } catch (const std::exception &error) {
        return errorStatus(500, fmt::format("Failed to parse Rhubarb JSON output: {}", error.what()), span,
                           "RhubarbOutputInvalid");
    }
    auto mouthCues = nlohmann::json::array();
    for (const auto &cue : lipSyncData.mouthCues)
        mouthCues.push_back({{"start", cue.start}, {"end", cue.end}, {"value", cue.value}});
    const nlohmann::json responseBody = {
        {"metadata", {{"soundFile", lipSyncData.metadata.soundFile}, {"duration", lipSyncData.metadata.duration}}},
        {"mouthCues", std::move(mouthCues)}};
    const auto jsonFilename = fmt::format("{}.json", sanitizedFilename.substr(0, sanitizedFilename.size() - 4));
    if (span) {
        span->setAttribute("json.size_bytes", static_cast<int64_t>(jsonContent.size()));
        span->setAttribute("json.filename", jsonFilename);
        span->setAttribute("json.mouth_cues", static_cast<int64_t>(lipSyncData.mouthCues.size()));
        span->setSuccess();
    }
    auto response = PreparedResponse::json(200, api::jsonToString(responseBody));
    response.headers.push_back({"Content-Disposition", fmt::format("attachment; filename=\"{}\"", jsonFilename)});
    return response;
}

PreparedResponse transcribeAudio(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto startTime = std::chrono::steady_clock::now();
    if (body.empty())
        return errorStatus(400, "Request body is empty — send raw 16kHz mono float32 PCM audio", span, "EmptyBody");
    if (body.size() % sizeof(float) != 0)
        return errorStatus(400, "Body size is not a multiple of 4 bytes (expected float32 PCM)", span,
                           "InvalidBodySize");
    const auto sampleCount = body.size() / sizeof(float);
    std::vector<float> audioData(sampleCount);
    std::memcpy(audioData.data(), body.data(), body.size());
    const float durationSec = static_cast<float>(sampleCount) / 16000.0F;
    spdlog::info("STT request: {:.1f}s of audio ({} samples, {} bytes)", durationSec, sampleCount, body.size());
    if (span) {
        span->setAttribute("audio.duration_ms", static_cast<int64_t>(durationSec * 1000.0F));
        span->setAttribute("audio.samples", static_cast<int64_t>(sampleCount));
        span->setAttribute("audio.bytes", static_cast<int64_t>(body.size()));
    }
    auto &processor = voice::WhisperLipSyncProcessor::instance();
    auto operationSpan = childSpan("stt.transcribe", span);
    const auto transcribed = processor.transcribe(audioData, operationSpan);
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
    if (!transcribed.isSuccess()) {
        const auto error = transcribed.getError().value();
        recordSpanError(operationSpan, error.getMessage(), "SpeechToTextFailure", error.getCode());
        return serverErrorStatus(error, span);
    }
    const auto transcript = transcribed.getValue().value();
    const api::SpeechToTextResponse response{"ok", transcript, static_cast<double>(durationSec), elapsedMs};
    if (span) {
        span->setAttribute("transcript.length", static_cast<int64_t>(transcript.size()));
        span->setAttribute("transcription.time_ms", static_cast<int64_t>(elapsedMs));
        span->setSuccess();
    }
    spdlog::info("STT complete in {:.0f}ms: \"{}\"", elapsedMs, transcript);
    return PreparedResponse::json(200, api::jsonToString(api::speechToTextResponseToJson(response)));
}

} // namespace creatures::transport
