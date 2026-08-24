
#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"

#include "server/audio/SoundPathResolver.h"
#include "server/config/Configuration.h"
#include "server/storage/Storage.h"
#include "server/voice/IxmlReader.h"

#include "model/Sound.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/Sha256.h"
#include "util/helpers.h"

#include "SoundService.h"

namespace creatures {
extern std::shared_ptr<creatures::Configuration> config;
extern std::shared_ptr<creatures::audio::SoundStoreIndex> permanentSoundIndex;
extern std::shared_ptr<creatures::audio::SoundStoreIndex> adHocSoundIndex;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<Database> db;
} // namespace creatures

namespace fs = std::filesystem;

namespace creatures ::ws {

namespace {
std::filesystem::path adHocRoot() { return std::filesystem::temp_directory_path() / "creature-adhoc"; }

template <typename T, typename SpanT>
Result<T> soundServiceError(const std::shared_ptr<SpanT> &span, const ServerError &serviceError,
                            const char *errorType) {
    if (span) {
        span->setError(serviceError.getMessage());
        span->setAttribute("error.type", errorType);
        span->setAttribute("error.code", static_cast<int64_t>(serviceError.getCode()));
        span->setAttribute("error.message", serviceError.getMessage());
    }
    return Result<T>{serviceError};
}

// The full validation lives beside the resolver so it is unit-testable
// (issue #94): 255-byte cap, well-formed UTF-8, C0/DEL/C1 rejection, no path
// components.
bool isSafeFilename(const std::string &filename) { return creatures::audio::isSafeSoundFilename(filename); }

/**
 * Look up a basename in an indexed store (issue #94). Ambiguous basenames are
 * a deterministic 409 naming every candidate, instead of the old
 * iteration-order pick. Falls back to the walking resolver only when the
 * index global isn't constructed (early startup, tests).
 */
Result<std::optional<std::string>> lookupSoundInStore(const std::shared_ptr<creatures::audio::SoundStoreIndex> &index,
                                                      const fs::path &root, const std::string &filename,
                                                      const char *storeName) {
    if (!index) {
        return Result<std::optional<std::string>>{creatures::audio::resolveSoundInRoot(root, filename)};
    }
    auto lookup = index->find(filename);
    using IndexStatus = creatures::audio::SoundStoreIndex::Status;
    if (lookup.status == IndexStatus::Ambiguous) {
        std::string candidateList;
        for (const auto &candidate : lookup.candidates) {
            if (!candidateList.empty()) {
                candidateList += ", ";
            }
            candidateList += candidate;
        }
        return Result<std::optional<std::string>>{
            ServerError(ServerError::Conflict, fmt::format("Sound '{}' is ambiguous in the {} store ({} matches: {})",
                                                           creatures::audio::sanitizeForLogging(filename), storeName,
                                                           lookup.candidates.size(), candidateList))};
    }
    if (lookup.status == IndexStatus::Found) {
        return Result<std::optional<std::string>>{std::optional<std::string>{lookup.entry->canonicalPath}};
    }
    return Result<std::optional<std::string>>{std::optional<std::string>{}};
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

    creatures::Sound sound;
    sound.fileName = filename;
    sound.size = size;
    sound.transcript = std::move(transcript);
    sound.lipsync = std::move(lipsync);

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

Result<std::vector<Sound>> SoundService::getAllSounds(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability ? observability->createOperationSpan("SoundService.getAllSounds", parentSpan) : nullptr;
    auto logger = spdlog::default_logger();

    if (!logger) {
        return soundServiceError<std::vector<Sound>>(
            span, ServerError(ServerError::InternalError, "Logger unavailable"), "DependencyUnavailable");
    }

    logger->debug("Request to return a list of the sound files");

    if (!config) {
        return soundServiceError<std::vector<Sound>>(
            span, ServerError(ServerError::InternalError, "Sound configuration unavailable"), "DependencyUnavailable");
    }

    // Copy the path locally
    std::string path = config->getSoundFileLocation();

    // Create the response to return
    std::vector<Sound> soundList;

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
                        soundList.emplace_back(std::move(sound));
                    }
                }
            }

            // Sort the list by file name (case-insensitive)
            std::sort(soundList.begin(), soundList.end(), [](const Sound &a, const Sound &b) {
                std::string aLower = a.fileName;
                std::string bLower = b.fileName;
                std::transform(aLower.begin(), aLower.end(), aLower.begin(), ::tolower);
                std::transform(bLower.begin(), bLower.end(), bLower.begin(), ::tolower);
                return aLower < bLower;
            });

            logger->debug("found {} sound files", soundList.size());

        } else {
            logger->warn("Sound file location not found: {}", path);

            return soundServiceError<std::vector<Sound>>(
                span, ServerError(ServerError::NotFound, fmt::format("No files found in {}", path)),
                "SoundDirectoryNotFound");
        }
    } catch (const fs::filesystem_error &e) {
        logger->error("Error reading sound file location: {}", e.what());

        return soundServiceError<std::vector<Sound>>(span, ServerError(ServerError::InternalError, e.what()),
                                                     "FilesystemError");
    }
    logger->debug("Returning {} sound files", soundList.size());
    if (span) {
        span->setAttribute("sound.count", static_cast<int64_t>(soundList.size()));
        span->setSuccess();
    }
    return Result<std::vector<Sound>>{std::move(soundList)};
}

Result<std::vector<api::AdHocSoundEntry>> SoundService::getAdHocSounds(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("SoundService.getAdHocSounds", parentSpan)
                    : nullptr;

    if (!db) {
        return soundServiceError<std::vector<api::AdHocSoundEntry>>(
            span, ServerError(ServerError::InternalError, "Database unavailable"), "DependencyUnavailable");
    }

    std::vector<api::AdHocSoundEntry> items;

    auto result = db->listAdHocAnimations(span);
    if (!result.isSuccess()) {
        auto error = result.getError().value();
        return soundServiceError<std::vector<api::AdHocSoundEntry>>(span, error, "AdHocSoundLookupFailed");
    }

    const auto records = result.getValue().value();
    for (const auto &record : records) {
        if (record.animation.metadata.sound_file.empty()) {
            continue;
        }
        const fs::path path(record.animation.metadata.sound_file);
        Sound sound;
        sound.fileName = path.filename().string();
        std::error_code errorCode;
        const auto size = fs::file_size(path, errorCode);
        if (!errorCode && size <= UINT32_MAX)
            sound.size = static_cast<uint32_t>(size);
        auto transcript = path;
        transcript.replace_extension(".txt");
        if (fs::exists(transcript))
            sound.transcript = transcript.filename().string();
        auto lipsync = path;
        lipsync.replace_extension(".json");
        if (fs::exists(lipsync))
            sound.lipsync = lipsync.filename().string();
        items.push_back({record.animation.id, formatTimeISO8601(record.createdAt), path.string(), std::move(sound)});
    }

    if (span) {
        span->setAttribute("adhoc_sound.count", static_cast<int64_t>(items.size()));
        span->setSuccess();
    }

    debug("Returning {} ad-hoc sound files", items.size());
    return Result<std::vector<api::AdHocSoundEntry>>{std::move(items)};
}

template <typename SpanT>
Result<std::string> resolveAdHoc(const std::string &filename, const std::shared_ptr<SpanT> &span) {
    if (!isSafeFilename(filename))
        return soundServiceError<std::string>(span, ServerError(ServerError::InvalidData, "Invalid filename"),
                                              "InvalidFilename");
    const auto root = adHocRoot();
    if (!fs::exists(root))
        return soundServiceError<std::string>(span, ServerError(ServerError::NotFound, "No ad-hoc sounds available"),
                                              "SoundStoreNotFound");
    auto lookup = lookupSoundInStore(adHocSoundIndex, root, filename, "ad-hoc");
    if (!lookup.isSuccess())
        return soundServiceError<std::string>(span, lookup.getError().value(), "SoundLookupFailed");
    if (!lookup.getValue().value())
        return soundServiceError<std::string>(
            span,
            ServerError(ServerError::NotFound,
                        fmt::format("Ad-hoc sound '{}' not found", audio::sanitizeForLogging(filename))),
            "SoundNotFound");
    if (span) {
        span->setAttribute("audio.file.hash", util::sha256Hex(filename));
        span->setAttribute("audio.file.extension", fs::path(filename).extension().string());
        span->setAttribute("audio.store", "ad_hoc");
        span->setSuccess();
    }
    return *lookup.getValue().value();
}

template <typename SpanT>
Result<std::string> resolvePermanent(const std::string &filename, const std::shared_ptr<SpanT> &span) {
    if (!isSafeFilename(filename))
        return soundServiceError<std::string>(span, ServerError(ServerError::InvalidData, "Invalid filename"),
                                              "InvalidFilename");
    if (!config)
        return soundServiceError<std::string>(
            span, ServerError(ServerError::InternalError, "Sound configuration unavailable"), "DependencyUnavailable");
    auto lookup = lookupSoundInStore(permanentSoundIndex, config->getSoundFileLocation(), filename, "permanent");
    if (!lookup.isSuccess())
        return soundServiceError<std::string>(span, lookup.getError().value(), "SoundLookupFailed");
    if (!lookup.getValue().value())
        return soundServiceError<std::string>(
            span,
            ServerError(ServerError::NotFound,
                        fmt::format("Sound '{}' not found", audio::sanitizeForLogging(filename))),
            "SoundNotFound");
    if (span) {
        span->setAttribute("audio.file.hash", util::sha256Hex(filename));
        span->setAttribute("audio.file.extension", fs::path(filename).extension().string());
        span->setAttribute("audio.store", "permanent");
        span->setSuccess();
    }
    return *lookup.getValue().value();
}

Result<std::string> SoundService::resolveAdHocSoundPath(const std::string &filename,
                                                        std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("SoundService.resolveAdHocSoundPath", parentSpan) : nullptr;
    return resolveAdHoc(filename, span);
}

Result<std::string> SoundService::resolveAdHocSoundPath(const std::string &filename,
                                                        std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability
                    ? observability->createChildOperationSpan("SoundService.resolveAdHocSoundPath", parentSpan)
                    : nullptr;
    return resolveAdHoc(filename, span);
}

Result<std::string> SoundService::resolvePermanentSoundPath(const std::string &filename,
                                                            std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability ? observability->createOperationSpan("SoundService.resolvePermanentSoundPath", parentSpan)
                              : nullptr;
    return resolvePermanent(filename, span);
}

Result<std::string> SoundService::resolvePermanentSoundPath(const std::string &filename,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability
                    ? observability->createChildOperationSpan("SoundService.resolvePermanentSoundPath", parentSpan)
                    : nullptr;
    return resolvePermanent(filename, span);
}

template <typename SpanT>
Result<std::optional<SoundService::ResolvedSound>> resolveAny(SoundService &service, const std::string &filename,
                                                              const std::shared_ptr<SpanT> &parentSpan) {
    auto permanent = service.resolvePermanentSoundPath(filename, parentSpan);
    if (permanent.isSuccess())
        return std::optional<SoundService::ResolvedSound>{{permanent.getValue().value(), true}};
    if (permanent.getError()->getCode() != ServerError::NotFound)
        return permanent.getError().value();
    auto adHoc = service.resolveAdHocSoundPath(filename, parentSpan);
    if (adHoc.isSuccess())
        return std::optional<SoundService::ResolvedSound>{{adHoc.getValue().value(), false}};
    if (adHoc.getError()->getCode() != ServerError::NotFound)
        return adHoc.getError().value();
    return std::optional<SoundService::ResolvedSound>{};
}

Result<std::optional<SoundService::ResolvedSound>>
SoundService::resolveSoundPath(const std::string &filename, std::shared_ptr<RequestSpan> parentSpan) {
    return resolveAny(*this, filename, parentSpan);
}

Result<std::optional<SoundService::ResolvedSound>>
SoundService::resolveSoundPath(const std::string &filename, std::shared_ptr<OperationSpan> parentSpan) {
    return resolveAny(*this, filename, parentSpan);
}

Result<Sound> SoundService::buildSoundMetadata(const std::string &absolutePath, const std::string &filename,
                                               std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("SoundService.buildSoundMetadata", parentSpan) : nullptr;
    uint32_t size = 0;
    std::error_code ec;
    const auto bytes = fs::file_size(absolutePath, ec);
    if (!ec) {
        size = static_cast<uint32_t>(bytes);
    }
    const Sound sound = buildSound(absolutePath, filename, size, /*heavy=*/true);
    if (span) {
        span->setAttribute("sound.file.hash", util::sha256Hex(filename));
        span->setAttribute("sound.file.size", static_cast<int64_t>(size));
        span->setAttribute("sound.track.count", static_cast<int64_t>(sound.tracks.size()));
        span->setSuccess();
    }
    return sound;
}

/**
 * Admit an ad-hoc sound for playback.
 *
 * RTP keeps its single reserved MusicEvent because frame dispatch belongs on
 * the event loop. Local/travel playback goes straight to the one-slot audio
 * coordinator so HTTP floods cannot accumulate on the sacred 1 ms queue.
 */
Result<api::StatusResponse> SoundService::playSound(const std::string &soundFile,
                                                    std::shared_ptr<RequestSpan> parentSpan) {
    auto logger = spdlog::default_logger();
    auto serviceSpan = creatures::observability
                           ? creatures::observability->createOperationSpan("SoundService.playSound", parentSpan)
                           : nullptr;
    const auto fail = [&](ServerError::Code code, const std::string &message) -> Result<api::StatusResponse> {
        if (serviceSpan) {
            serviceSpan->setAttribute("error.code", static_cast<int64_t>(code));
            serviceSpan->setAttribute("error.message", message);
            serviceSpan->setAttribute("audio.failure_stage", "admission");
            serviceSpan->setError(message);
        }
        return ServerError(code, message);
    };
    if (!logger)
        return fail(ServerError::InternalError, "Logger unavailable");
    if (soundFile.empty())
        return fail(ServerError::InvalidData, "Sound filename is required");
    if (!isSafeFilename(soundFile))
        return fail(ServerError::InvalidData, "Invalid filename");
    if (!config)
        return fail(ServerError::InternalError, "Sound configuration unavailable");

    // isSafeFilename passed, but keep log/span rendering bounded and
    // printable anyway — defense in depth for anything that logs earlier
    // on a rejection path (issue #94).
    const std::string loggedName = creatures::audio::sanitizeForLogging(soundFile);
    logger->debug("Request to play sound file: {}", loggedName);
    if (serviceSpan) {
        serviceSpan->setAttribute("audio.file.name", loggedName);
    }

    // Resolve only through the configured permanent/ad-hoc stores. The
    // operation-span overload keeps both lookup attempts beneath this
    // service operation.
    const auto resolvedResult = resolveSoundPath(soundFile, serviceSpan);
    if (!resolvedResult.isSuccess())
        return fail(resolvedResult.getError()->getCode(), resolvedResult.getError()->getMessage());
    const auto resolvedSound = resolvedResult.getValue().value();
    if (!resolvedSound)
        return fail(ServerError::NotFound, fmt::format("Sound file not found: {}", loggedName));
    const std::string &fullFilePath = resolvedSound->path;
    if (!fileIsReadable(fullFilePath))
        return fail(ServerError::NotFound, fmt::format("Sound file not found: {}", loggedName));

    constexpr uintmax_t MAX_PLAYBACK_FILE_SIZE = 1024ULL * 1024ULL * 1024ULL;
    std::error_code fileError;
    const auto fileSize = fs::file_size(fullFilePath, fileError);
    if (fileError || fileSize > MAX_PLAYBACK_FILE_SIZE)
        return fail(ServerError::InvalidData, "Sound file is too large or cannot be inspected");

    const bool rtpMode = config->getAudioMode() == Configuration::AudioMode::RTP;
    const bool travelMode = config->getTravelMode();

    // Only accept extensions the SELECTED playback mode can actually play
    // (issue #94): RTP streams through the 17-channel WAV loader only,
    // while the local decoder also handles .wave/.mp3/.flac. The old
    // mode-independent check let an .mp3 into RTP mode, where it failed
    // deep inside a worker thread instead of here as a 400.
    std::string extension = fs::path(soundFile).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    // The stores' convention is .wav (never .wave), matching the historical
    // validation. RTP streams only the 17-channel WAV contract; the local
    // decoder additionally handles mp3/flac.
    const bool supportedFormat =
        rtpMode ? (extension == ".wav") : (extension == ".wav" || extension == ".mp3" || extension == ".flac");
    if (!supportedFormat)
        return fail(ServerError::InvalidData,
                    fmt::format("Unsupported sound file format for {} playback: {}",
                                rtpMode ? "RTP" : (travelMode ? "travel" : "local"), extension));
    if (serviceSpan) {
        serviceSpan->setAttribute("audio.store", resolvedSound->fromPermanentStore ? "permanent" : "ad_hoc");
        serviceSpan->setAttribute("audio.file.size", static_cast<int64_t>(fileSize));
        serviceSpan->setAttribute("audio.mode", rtpMode ? "rtp" : (travelMode ? "travel" : "main"));
    }

    std::string message;
    if (rtpMode) {
        if (!eventLoop)
            return fail(ServerError::InternalError, "Sound event loop unavailable");
        auto rtpReservation = rtp::standaloneRtpAdmission().tryAcquire();
        if (!rtpReservation)
            return fail(ServerError::Conflict, "Standalone RTP audio loader is busy");

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
            return fail(error.getCode(), error.getMessage());
        }
        const uint64_t generation = admission.getValue().value();
        if (serviceSpan) {
            serviceSpan->setAttribute("audio.admission.result", "accepted");
            serviceSpan->setAttribute("audio.local.generation", static_cast<int64_t>(generation));
        }
        message = fmt::format("Queued {} as local audio generation {}", soundFile, generation);
    }

    if (serviceSpan) {
        serviceSpan->setSuccess();
    }
    return api::makeStatusResponse(200, message, api::STATUS_OK);
}

} // namespace creatures::ws
