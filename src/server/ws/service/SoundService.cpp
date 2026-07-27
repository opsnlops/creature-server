
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <typeinfo>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "exception/exception.h"

#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"

#include "server/audio/SoundPathResolver.h"
#include "server/config/Configuration.h"
#include "server/storage/Storage.h"
#include "server/voice/IxmlReader.h"

#include "model/Sound.h"
#include "server/database.h"
#include "server/ws/dto/AdHocDtoUtils.h"
#include "server/ws/dto/AdHocSoundEntryDto.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

#include "util/ChildProcess.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

#include "SoundService.h"

namespace creatures {
extern std::shared_ptr<creatures::Configuration> config;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<Database> db;
} // namespace creatures

namespace fs = std::filesystem;

namespace creatures ::ws {

using oatpp::web::protocol::http::Status;

namespace {
std::filesystem::path adHocRoot() { return std::filesystem::temp_directory_path() / "creature-adhoc"; }

bool isSafeFilename(const std::string &filename) {
    constexpr std::size_t MAX_FILENAME_LENGTH = 255;
    if (filename.empty() || filename.size() > MAX_FILENAME_LENGTH) {
        return false;
    }
    if (std::any_of(filename.begin(), filename.end(),
                    [](unsigned char character) { return character < 0x20 || character == 0x7f; })) {
        return false;
    }

    fs::path candidate(filename);

    // Reject absolute paths, root references, or anything with parent components
    if (candidate.is_absolute() || candidate.has_root_path() || candidate != candidate.filename()) {
        return false;
    }

    for (const auto &part : candidate) {
        if (part == ".." || part == "." || part.native().find('\0') != std::string::npos) {
            return false;
        }
    }

    return true;
}

// Populate a Sound's embedded-metadata fields from a WAV's iXML document. Always
// fills the lightweight fields (title/ids/flags/script + structured turns + track
// list); `heavy` additionally parses the large per-track mouth-cue and word-timing
// arrays. The sound LIST calls this with heavy=false to stay light; the per-sound
// metadata endpoint uses heavy=true (issue #56).
void populateEmbeddedMetadata(creatures::Sound &sound, const std::string &ixml, bool heavy) {
    sound.title = creatures::voice::extractIxmlField(ixml, "TITLE").value_or("");
    sound.sourceScriptId = creatures::voice::extractIxmlField(ixml, "SOURCE_SCRIPT_ID").value_or("");
    sound.script = creatures::voice::extractIxmlField(ixml, "DIALOG_SCRIPT").value_or("");
    sound.generationIds = creatures::voice::extractIxmlField(ixml, "GENERATION_IDS").value_or("");
    sound.hasEmbeddedScript = !sound.script.empty();
    sound.hasEmbeddedLipsync = ixml.find("<LIPSYNC>") != std::string::npos;
    sound.scriptTurns = creatures::voice::parseDialogScriptTurns(sound.script);
    sound.tracks = creatures::voice::parseIxmlTrackList(ixml);
    if (heavy) {
        sound.lipsyncTracks = creatures::voice::parseIxmlLipsync(ixml);
        sound.wordTracks = creatures::voice::parseIxmlWordAlignment(ixml);
    }
}

// Build a Sound from a file on disk: size, transcript/lipsync sidecars, and (for
// .wav) the embedded iXML metadata. Single builder shared by the list (heavy=false)
// and the per-sound metadata endpoint (heavy=true).
creatures::Sound buildSound(const fs::path &filepath, const std::string &filename, uint32_t size, bool heavy) {
    std::string transcript;
    auto transcriptPath = filepath;
    transcriptPath.replace_extension(".txt");
    if (fs::exists(transcriptPath)) {
        transcript = transcriptPath.filename().string();
    }

    std::string lipsync;
    auto lipsyncPath = filepath;
    lipsyncPath.replace_extension(".json");
    if (fs::exists(lipsyncPath)) {
        lipsync = lipsyncPath.filename().string();
    }

    creatures::Sound sound{filename, size, transcript, lipsync};

    // Reading the iXML chunk seeks past the (large) audio data, so this stays cheap
    // even for big files; heavy=false additionally skips the cue/word array parsing.
    if (filepath.extension() == ".wav") {
        if (auto ixml = creatures::voice::readIxmlChunk(filepath)) {
            populateEmbeddedMetadata(sound, *ixml, heavy);
        }
    }
    return sound;
}
} // namespace

oatpp::Object<ListDto<oatpp::Object<creatures::SoundDto>>> SoundService::getAllSounds() {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
    auto logger = appLogger ? appLogger : spdlog::default_logger();

    if (!logger) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Logger unavailable");
    }

    logger->debug("Request to return a list of the sound files");

    if (!config) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Sound configuration unavailable");
    }

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
            // Recursive so sounds in subdirectories — notably permanent dialog
            // renders under dialog/ — are listed too (issue #46). The emitted
            // file_name stays the basename, which the resolver looks up by walking
            // the tree, so the client contract is unchanged.
            for (const auto &entry : fs::recursive_directory_iterator(path)) {
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
                            logger->warn("Failed to get file size for {}: {}", filename, e.what());
                            continue; // Skip this file
                        }

                        // Validate file size is reasonable (prevent display of huge files)
                        constexpr uintmax_t MAX_SOUND_FILE_SIZE = 1024 * 1024 * 1024; // 1GB max
                        if (size > MAX_SOUND_FILE_SIZE) {
                            logger->warn("Skipping oversized sound file: {} ({} bytes)", filename, size);
                            continue;
                        }

                        // Light list: title/flags + structured script turns + track list
                        // (comparable in size to the script blob already returned here). The
                        // heavy per-track mouth-cue and word-timing arrays are deliberately
                        // omitted (heavy=false) so the list stays small even for a store full
                        // of multi-track dialog renders — the console fetches those per-sound
                        // via GET /api/v1/sound/{filename}/metadata (issue #56).
                        Sound sound = buildSound(filepath, filename, static_cast<uint32_t>(size), /*heavy=*/false);

                        logger->debug("Adding sound file: {} ({})", sound.fileName, sound.size);
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

            logger->debug("found {} sound files", soundList->size());

        } else {
            logger->warn("Sound file location not found: {}", path);

            status = Status::CODE_404;
            message = fmt::format("No files found in {}", path);
            error = true;
        }
    } catch (const fs::filesystem_error &e) {
        logger->error("Error reading sound file location: {}", e.what());

        status = Status::CODE_500;
        message = fmt::format("Error reading sound file location: {}", e.what());
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, message);

    // All done!
    auto list = ListDto<oatpp::Object<creatures::SoundDto>>::createShared();
    list->count = soundList->size();
    list->items = soundList;

    logger->debug("Returning {} sound files", static_cast<uint32_t>(list->count));
    return list;
}

oatpp::Object<AdHocSoundListDto> SoundService::getAdHocSounds(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("SoundService.getAdHocSounds", parentSpan)
                    : nullptr;

    if (!db) {
        if (span) {
            span->setError("Database unavailable");
        }
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database unavailable");
    }

    auto list = AdHocSoundListDto::createShared();
    auto items = oatpp::List<oatpp::Object<AdHocSoundEntryDto>>::createShared();

    auto result = db->listAdHocAnimations(span);
    if (!result.isSuccess()) {
        auto error = result.getError().value();
        Status status = Status::CODE_500;
        switch (error.getCode()) {
        case ServerError::NotFound:
            status = Status::CODE_404;
            break;
        case ServerError::InvalidData:
            status = Status::CODE_400;
            break;
        default:
            status = Status::CODE_500;
            break;
        }
        if (span) {
            span->setError(error.getMessage());
        }
        OATPP_ASSERT_HTTP(false, status, error.getMessage().c_str());
    }

    const auto records = result.getValue().value();
    for (const auto &record : records) {
        auto dto = buildAdHocSoundEntryDto(record.animation, record.createdAt);
        if (!dto) {
            continue;
        }
        items->push_back(dto);
    }

    list->count = static_cast<v_uint32>(items->size());
    list->items = items;

    if (span) {
        span->setAttribute("adhoc_sound.count", static_cast<int64_t>(items->size()));
        span->setSuccess();
    }

    debug("Returning {} ad-hoc sound files", static_cast<uint32_t>(list->count));
    return list;
}

std::string SoundService::resolveAdHocSoundPath(const std::string &filename, std::shared_ptr<RequestSpan> parentSpan) {
    if (!isSafeFilename(filename)) {
        OATPP_ASSERT_HTTP(false, Status::CODE_400, "Invalid filename");
    }

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("SoundService.resolveAdHocSoundPath", parentSpan)
                    : nullptr;
    auto root = adHocRoot();
    if (!fs::exists(root)) {
        OATPP_ASSERT_HTTP(false, Status::CODE_404, "No ad-hoc sounds available");
    }

    if (auto found = creatures::audio::resolveSoundInRoot(root, filename)) {
        if (span) {
            span->setAttribute("adhoc_sound.path", *found);
        }
        return *found;
    }

    OATPP_ASSERT_HTTP(false, Status::CODE_404,
                      fmt::format("Ad-hoc sound '{}' not found in {}", filename, root.string()).c_str());
}

std::string SoundService::resolvePermanentSoundPath(const std::string &filename,
                                                    std::shared_ptr<RequestSpan> parentSpan) {
    if (!isSafeFilename(filename)) {
        OATPP_ASSERT_HTTP(false, Status::CODE_400, "Invalid filename");
    }
    if (!config) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Sound configuration unavailable");
    }

    auto span =
        creatures::observability
            ? creatures::observability->createOperationSpan("SoundService.resolvePermanentSoundPath", parentSpan)
            : nullptr;

    const fs::path root = config->getSoundFileLocation();

    // Top-level first, then a recursive basename search so dialog/ renders and any
    // other subdir'd sound resolve too (issue #46).
    if (auto found = creatures::audio::resolveSoundInRoot(root, filename)) {
        if (span) {
            span->setAttribute("sound.path", *found);
        }
        return *found;
    }

    OATPP_ASSERT_HTTP(false, Status::CODE_404,
                      fmt::format("Sound '{}' not found in {}", filename, root.string()).c_str());
}

std::optional<SoundService::ResolvedSound> SoundService::resolveSoundPath(const std::string &filename,
                                                                          std::shared_ptr<RequestSpan> parentSpan) {
    // Permanent store first (content-addressed, immutable), then the ad-hoc bucket.
    // The single-store resolvers throw HttpError when they miss; catch that here so
    // callers get a plain optional and the store-precedence policy lives in one place.
    try {
        return ResolvedSound{resolvePermanentSoundPath(filename, parentSpan), true};
    } catch (oatpp::web::protocol::http::HttpError &) {
        // fall through to ad-hoc
    }
    try {
        return ResolvedSound{resolveAdHocSoundPath(filename, parentSpan), false};
    } catch (oatpp::web::protocol::http::HttpError &) {
        return std::nullopt;
    }
}

std::string SoundService::resolveAdHocSoundPath(const std::string &filename,
                                                std::shared_ptr<OperationSpan> parentSpan) {
    auto span =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("SoundService.resolveAdHocSoundPath", parentSpan)
            : nullptr;
    try {
        OATPP_ASSERT_HTTP(isSafeFilename(filename), Status::CODE_400, "Invalid filename");
        const auto root = adHocRoot();
        OATPP_ASSERT_HTTP(fs::exists(root), Status::CODE_404, "No ad-hoc sounds available");

        if (auto found = creatures::audio::resolveSoundInRoot(root, filename)) {
            if (span) {
                span->setAttribute("audio.file.name", filename);
                span->setAttribute("audio.store", "ad_hoc");
                span->setSuccess();
            }
            return *found;
        }
        OATPP_ASSERT_HTTP(false, Status::CODE_404, fmt::format("Ad-hoc sound '{}' not found", filename).c_str());
    } catch (const std::exception &exception) {
        if (span) {
            span->setAttribute("error.type", typeid(exception).name());
            span->setAttribute("error.code", "sound.resolve_ad_hoc_failed");
            span->setAttribute("error.message", exception.what());
            span->recordException(exception);
            span->setError(exception.what());
        }
        throw;
    }
}

std::string SoundService::resolvePermanentSoundPath(const std::string &filename,
                                                    std::shared_ptr<OperationSpan> parentSpan) {
    auto span =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("SoundService.resolvePermanentSoundPath", parentSpan)
            : nullptr;
    try {
        OATPP_ASSERT_HTTP(isSafeFilename(filename), Status::CODE_400, "Invalid filename");
        OATPP_ASSERT_HTTP(config, Status::CODE_500, "Sound configuration unavailable");

        const fs::path root = config->getSoundFileLocation();
        if (auto found = creatures::audio::resolveSoundInRoot(root, filename)) {
            if (span) {
                span->setAttribute("audio.file.name", filename);
                span->setAttribute("audio.store", "permanent");
                span->setSuccess();
            }
            return *found;
        }
        OATPP_ASSERT_HTTP(false, Status::CODE_404, fmt::format("Sound '{}' not found", filename).c_str());
    } catch (const std::exception &exception) {
        if (span) {
            span->setAttribute("error.type", typeid(exception).name());
            span->setAttribute("error.code", "sound.resolve_permanent_failed");
            span->setAttribute("error.message", exception.what());
            span->recordException(exception);
            span->setError(exception.what());
        }
        throw;
    }
}

std::optional<SoundService::ResolvedSound> SoundService::resolveSoundPath(const std::string &filename,
                                                                          std::shared_ptr<OperationSpan> parentSpan) {
    try {
        return ResolvedSound{resolvePermanentSoundPath(filename, parentSpan), true};
    } catch (oatpp::web::protocol::http::HttpError &) {
        // Fall through to the ad-hoc store. Each attempted lookup retains its
        // own child span so store precedence remains visible.
    }
    try {
        return ResolvedSound{resolveAdHocSoundPath(filename, parentSpan), false};
    } catch (oatpp::web::protocol::http::HttpError &) {
        return std::nullopt;
    }
}

oatpp::Object<creatures::SoundDto> SoundService::buildSoundMetadata(const std::string &absolutePath,
                                                                    const std::string &filename) {
    uint32_t size = 0;
    std::error_code ec;
    const auto bytes = fs::file_size(absolutePath, ec);
    if (!ec) {
        size = static_cast<uint32_t>(bytes);
    }
    const Sound sound = buildSound(absolutePath, filename, size, /*heavy=*/true);
    return creatures::convertSoundToDto(sound);
}

/**
 * Admit an ad-hoc sound for playback.
 *
 * RTP keeps its single reserved MusicEvent because frame dispatch belongs on
 * the event loop. Local/travel playback goes straight to the one-slot audio
 * coordinator so HTTP floods cannot accumulate on the sacred 1 ms queue.
 */
oatpp::Object<creatures::ws::StatusDto> SoundService::playSound(const oatpp::String &inSoundFile,
                                                                std::shared_ptr<RequestSpan> parentSpan) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
    auto logger = appLogger ? appLogger : spdlog::default_logger();
    auto serviceSpan = creatures::observability
                           ? creatures::observability->createOperationSpan("SoundService.playSound", parentSpan)
                           : nullptr;
    try {
        OATPP_ASSERT_HTTP(logger, Status::CODE_500, "Logger unavailable");
        OATPP_ASSERT_HTTP(inSoundFile && !inSoundFile->empty(), Status::CODE_400, "Sound filename is required");
        const std::string soundFile = std::string(inSoundFile);
        OATPP_ASSERT_HTTP(isSafeFilename(soundFile), Status::CODE_400, "Invalid filename");
        OATPP_ASSERT_HTTP(config, Status::CODE_500, "Sound configuration unavailable");

        logger->debug("Request to play sound file: {}", soundFile);
        if (serviceSpan) {
            serviceSpan->setAttribute("audio.file.name", soundFile);
        }

        // Resolve only through the configured permanent/ad-hoc stores. The
        // operation-span overload keeps both lookup attempts beneath this
        // service operation.
        const auto resolvedSound = resolveSoundPath(soundFile, serviceSpan);
        OATPP_ASSERT_HTTP(resolvedSound.has_value(), Status::CODE_404,
                          fmt::format("Sound file not found: {}", soundFile).c_str());
        const std::string &fullFilePath = resolvedSound->path;
        OATPP_ASSERT_HTTP(fileIsReadable(fullFilePath), Status::CODE_404,
                          fmt::format("Sound file not found: {}", soundFile).c_str());

        constexpr uintmax_t MAX_PLAYBACK_FILE_SIZE = 1024ULL * 1024ULL * 1024ULL;
        std::error_code fileError;
        const auto fileSize = fs::file_size(fullFilePath, fileError);
        OATPP_ASSERT_HTTP(!fileError && fileSize <= MAX_PLAYBACK_FILE_SIZE, Status::CODE_400,
                          "Sound file is too large or cannot be inspected");

        std::string extension = fs::path(soundFile).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        const bool supportedFormat = extension == ".wav" || extension == ".mp3" || extension == ".flac";
        OATPP_ASSERT_HTTP(supportedFormat, Status::CODE_400, "Unsupported sound file format");

        const bool rtpMode = config->getAudioMode() == Configuration::AudioMode::RTP;
        const bool travelMode = config->getTravelMode();
        OATPP_ASSERT_HTTP(!travelMode || extension == ".wav", Status::CODE_400,
                          "Travel mode local playback requires a WAV file");
        if (serviceSpan) {
            serviceSpan->setAttribute("audio.store", resolvedSound->fromPermanentStore ? "permanent" : "ad_hoc");
            serviceSpan->setAttribute("audio.file.size", static_cast<int64_t>(fileSize));
            serviceSpan->setAttribute("audio.mode", rtpMode ? "rtp" : (travelMode ? "travel" : "main"));
        }

        oatpp::String message;
        if (rtpMode) {
            OATPP_ASSERT_HTTP(eventLoop, Status::CODE_500, "Sound event loop unavailable");
            auto rtpReservation = rtp::standaloneRtpAdmission().tryAcquire();
            OATPP_ASSERT_HTTP(rtpReservation, Status::CODE_409, "Standalone RTP audio loader is busy");

            const framenum_t frameNumber = eventLoop->getNextFrameNumber();
            const std::string traceId = serviceSpan ? serviceSpan->getTraceIdHex() : std::string{};
            const std::string spanId = serviceSpan ? serviceSpan->getSpanIdHex() : std::string{};
            auto playEvent =
                std::make_shared<MusicEvent>(frameNumber, fullFilePath, std::move(rtpReservation), traceId, spanId);
            eventLoop->scheduleEvent(playEvent);
            if (serviceSpan) {
                serviceSpan->setAttribute("audio.admission.result", "scheduled");
                serviceSpan->setAttribute("event.frame_number", static_cast<int64_t>(frameNumber));
            }
            message = fmt::format("Scheduled {} for frame {}", soundFile, frameNumber);
        } else {
            const std::string traceId = serviceSpan ? serviceSpan->getTraceIdHex() : std::string{};
            const std::string spanId = serviceSpan ? serviceSpan->getSpanIdHex() : std::string{};
            const auto admission = MusicEvent::submitLocalAudio(fullFilePath, travelMode, serviceSpan, traceId, spanId);
            if (!admission.isSuccess()) {
                const auto error = admission.getError().value();
                const auto status = error.getCode() == ServerError::Conflict ? Status::CODE_409 : Status::CODE_500;
                OATPP_ASSERT_HTTP(false, status, error.getMessage().c_str());
            }
            const uint64_t generation = admission.getValue().value();
            if (serviceSpan) {
                serviceSpan->setAttribute("audio.admission.result", "accepted");
                serviceSpan->setAttribute("audio.local.generation", static_cast<int64_t>(generation));
            }
            message = fmt::format("Queued {} as local audio generation {}", soundFile, generation);
        }

        auto response = StatusDto::createShared();
        response->code = 200;
        response->message = message;
        response->status = "OK";
        if (serviceSpan) {
            serviceSpan->setSuccess();
        }
        return response;
    } catch (const std::exception &exception) {
        if (serviceSpan) {
            serviceSpan->setAttribute("error.type", typeid(exception).name());
            serviceSpan->setAttribute("error.code", "sound.play_failed");
            serviceSpan->setAttribute("error.message", exception.what());
            serviceSpan->setAttribute("audio.failure_stage", "admission");
            serviceSpan->recordException(exception);
            serviceSpan->setError(exception.what());
        }
        throw;
    } catch (...) {
        if (serviceSpan) {
            serviceSpan->setAttribute("error.type", "UnknownSoundPlaybackException");
            serviceSpan->setAttribute("error.code", "sound.play_unknown_exception");
            serviceSpan->setAttribute("error.message", "Unknown sound playback exception");
            serviceSpan->setAttribute("audio.failure_stage", "admission");
            serviceSpan->setError("Unknown sound playback exception");
        }
        throw;
    }
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

    std::string soundFile = std::string(inSoundFile);
    debug("generateLipSync() called for file: {}, allowOverwrite: {}", soundFile, allowOverwrite);

    if (!parentSpan) {
        warn("no parent span provided for SoundService.generateLipSync, creating a root span");
    }

    // Create operation span - connects to parent RequestSpan if provided
    auto span = creatures::observability ? creatures::observability->createOperationSpan("SoundService.generateLipSync",
                                                                                         std::move(parentSpan))
                                         : nullptr;

    if (span) {
        span->setAttribute("sound.file", soundFile);
        span->setAttribute("allow_overwrite", allowOverwrite);
        debug("Created observability span for processSound");
    }

    auto response = StatusDto::createShared();

    if (!config) {
        std::string errorMsg = "Sound configuration unavailable";
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        response->code = 500;
        response->status = "Internal Server Error";
        response->message = errorMsg;
        return response;
    }

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

    // Build the Rhubarb argv. Each filename is its own argv entry, so no
    // shell is involved and metacharacters in the names can't escape.
    std::vector<std::string> args = {"-f", "json", "-o", jsonOutputPath.string()};
    if (hasTranscript) {
        args.push_back("--dialogFile");
        args.push_back(transcriptPath.string());
        debug("Added transcript file to command");
    }
    args.push_back(soundFilePath.string());

    info("Executing Rhubarb: {} with {} args (input={})", rhubarbBinary, args.size(),
         soundFilePath.filename().string());

    // 6. Execute Rhubarb
    std::string commandOutput;
    int exitCode = 0;

    try {
        auto spawnResult = util::runChildProcess(rhubarbBinary, args, /*mergeStderrToStdout=*/true);
        if (!spawnResult.isSuccess()) {
            auto err = spawnResult.getError().value();
            std::string errorMsg =
                fmt::format("Failed to execute Rhubarb binary at '{}'. "
                            "Is it installed and accessible? "
                            "Check the server configuration (--rhubarb-binary-path or RHUBARB_BINARY_PATH environment "
                            "variable). Detail: {}",
                            rhubarbBinary, err.getMessage());
            error(errorMsg);
            if (span) {
                span->setError(errorMsg);
            }
            response->code = 500;
            response->status = "Internal Server Error";
            response->message = errorMsg;
            return response;
        }
        auto child = spawnResult.getValue().value();
        commandOutput = child.output;
        exitCode = child.exitCode;
        debug("Rhubarb exited with code {} ({} bytes of output)", exitCode, commandOutput.size());

        if (span) {
            span->setAttribute("rhubarb.exit_code", static_cast<int64_t>(exitCode));
            span->setAttribute("rhubarb.output_length", static_cast<int64_t>(commandOutput.length()));
        }

        if (exitCode != 0) {
            std::string errorMsg =
                fmt::format("Rhubarb processing failed with exit code {}.\n\nOutput:\n{}", exitCode, commandOutput);
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
        std::string errorMsg =
            fmt::format("Exception during Rhubarb execution: {}\n\nOutput:\n{}", e.what(), commandOutput);
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
        std::string errorMsg =
            fmt::format("Rhubarb completed but did not create the expected JSON file '{}'.\n\nOutput:\n{}",
                        jsonOutputPath.filename().string(), commandOutput);
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
