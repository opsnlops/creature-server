#include "server/transport/MediaFileHandlers.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "api/DialogContracts.h"
#include "api/JsonResponse.h"
#include "model/AdHocExchange.h"
#include "model/Sound.h"
#include "server/database.h"
#include "server/metrics/counters.h"
#include "server/transport/HandlerSupport.h"
#include "server/voice/DialogCache.h"
#include "server/voice/IxmlReader.h"
#include "server/voice/PcmWavWriter.h"
#include "server/ws/service/MusicCandidateMp3.h"
#include "server/ws/service/SoundRenditionService.h"
#include "server/ws/service/SoundService.h"
#include "util/ObservabilityManager.h"
#include "util/Sha256.h"
#include "util/Slugify.h"
#include "util/UuidValidation.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SystemCounters> metrics;
} // namespace creatures

namespace creatures::transport {
namespace {

constexpr const char *IMMUTABLE_CACHE_CONTROL = "public, max-age=31536000, immutable";
constexpr const char *NO_STORE_CACHE_CONTROL = "no-store";

std::string mimeTypeFor(const std::string &filename) {
    if (filename.ends_with(".mp3"))
        return "audio/mpeg";
    if (filename.ends_with(".wav"))
        return "audio/wav";
    if (filename.ends_with(".ogg"))
        return "audio/ogg";
    return "application/octet-stream";
}

std::string attachment(const std::string &name) { return "attachment; filename=\"" + name + "\""; }

std::string bytesToString(const std::vector<uint8_t> &bytes) {
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/// Size a file for streaming. A missing or unreadable file is a 404, matching
/// the oat++ FileBody path; opening it here proves the loop will be able to.
Result<std::uint64_t> sizeForStreaming(const std::string &path) {
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        return Result<std::uint64_t>{ServerError(ServerError::NotFound, "File not found.")};
    }
    return Result<std::uint64_t>{static_cast<std::uint64_t>(size)};
}

} // namespace

PreparedResponse getSoundFile(const std::string &filename, const std::shared_ptr<OperationSpan> &span) {
    const auto safe = sanitizeSoundFilename(filename);
    if (!safe.isSuccess()) {
        spdlog::warn("Attempt to serve {} failed: {}", filename, safe.getError()->getMessage());
        return errorStatus(403, safe.getError()->getMessage(), span, "InvalidFilename");
    }
    const auto safeFilename = safe.getValue().value();
    ws::SoundService service;
    const auto resolved = service.resolvePermanentSoundPath(safeFilename, span);
    if (!resolved.isSuccess())
        return serverErrorStatus(resolved.getError().value(), span);
    const auto path = resolved.getValue().value();
    const auto size = sizeForStreaming(path);
    if (!size.isSuccess()) {
        spdlog::info("Attempt to serve {} failed: Not found.", filename);
        return errorStatus(404, "File not found.", span, "NotFound");
    }
    const auto mimeType = mimeTypeFor(path);
    if (creatures::metrics)
        creatures::metrics->incrementSoundFilesServed();
    spdlog::info("Serving sound file: {} ({}, {} bytes)", filename, mimeType, size.getValue().value());
    if (span) {
        span->setAttribute("sound.filename_hash", util::sha256Hex(safeFilename));
        span->setAttribute("sound.bytes", static_cast<int64_t>(size.getValue().value()));
        span->setSuccess();
    }
    return PreparedResponse::fileStream(path, size.getValue().value(), mimeType);
}

PreparedResponse getAdHocSoundFile(const std::string &filename, const std::shared_ptr<OperationSpan> &span) {
    const auto safe = sanitizeSoundFilename(filename);
    if (!safe.isSuccess())
        return errorStatus(403, safe.getError()->getMessage(), span, "InvalidFilename");
    const auto safeFilename = safe.getValue().value();
    ws::SoundService service;
    const auto resolved = service.resolveAdHocSoundPath(safeFilename, span);
    if (!resolved.isSuccess())
        return serverErrorStatus(resolved.getError().value(), span);
    const auto path = resolved.getValue().value();
    const auto size = sizeForStreaming(path);
    if (!size.isSuccess())
        return errorStatus(404, "File not found.", span, "NotFound");
    if (span) {
        span->setAttribute("sound.filename_hash", util::sha256Hex(safeFilename));
        span->setAttribute("sound.bytes", static_cast<int64_t>(size.getValue().value()));
        span->setSuccess();
    }
    return PreparedResponse::fileStream(path, size.getValue().value(), mimeTypeFor(path),
                                        {{"Content-Disposition", attachment(safeFilename)}});
}

PreparedResponse renderSoundRendition(const std::string &filename, const ws::SoundRenditionFormat format,
                                      const std::shared_ptr<OperationSpan> &span) {
    const bool mp3 = format == ws::SoundRenditionFormat::Mp3;
    const std::string formatName = mp3 ? "mp3" : "ogg";
    const std::string extension = mp3 ? ".mp3" : ".ogg";

    const auto safe = sanitizeSoundFilename(filename);
    if (!safe.isSuccess()) {
        spdlog::warn("Attempt to render {} for {} failed: {}", formatName, filename, safe.getError()->getMessage());
        return errorStatus(403, safe.getError()->getMessage(), span, "InvalidFilename");
    }
    const auto safeFilename = safe.getValue().value();
    if (!safeFilename.ends_with(extension)) {
        return errorStatus(422,
                           fmt::format("{} renditions are addressed as '{{name}}{}', not '{}'. Request the source's "
                                       "basename with a {} extension.",
                                       formatName, extension, safeFilename, extension),
                           span, "InvalidRenditionName");
    }
    const std::string stem = safeFilename.substr(0, safeFilename.size() - extension.size());
    if (span) {
        // Dialog titles and music-prompt fragments can appear in descriptive
        // filenames. Retain correlation without persisting that creative text.
        span->setAttribute("sound.filename_hash", util::sha256Hex(safeFilename));
    }

    // The source can only be a WAV (loadWavAsMono), so it's exactly '{stem}.wav'.
    const std::string sourceWav = stem + ".wav";
    ws::SoundService service;
    const auto resolvedResult = service.resolveSoundPath(sourceWav, span);
    if (!resolvedResult.isSuccess())
        return serverErrorStatus(resolvedResult.getError().value(), span);
    const auto resolved = resolvedResult.getValue().value();
    if (!resolved) {
        return errorStatus(404,
                           fmt::format("Sound '{}' was not found in the sound store or the ad-hoc store", sourceWav),
                           span, "NotFound");
    }
    if (span) {
        span->setAttribute("sound.store", resolved->fromPermanentStore ? "permanent" : "ad_hoc");
        span->setAttribute("sound.source_hash", util::sha256Hex(resolved->path));
    }

    auto renditionSpan = childSpan("SoundRenditionService.renderWav", span);
    if (renditionSpan) {
        renditionSpan->setAttribute("sound.store", resolved->fromPermanentStore ? "permanent" : "ad_hoc");
        renditionSpan->setAttribute("sound.source_hash", util::sha256Hex(resolved->path));
        renditionSpan->setAttribute("rendition.format", formatName);
        renditionSpan->setAttribute("cache.outcome", "bypass");
        renditionSpan->setAttribute("encoding.performed", true);
        std::error_code fileSizeError;
        const auto inputBytes = std::filesystem::file_size(resolved->path, fileSizeError);
        if (!fileSizeError)
            renditionSpan->setAttribute("encoding.input_bytes", static_cast<int64_t>(inputBytes));
    }
    // If the WAV's iXML is missing a title or a script, borrow both the title
    // (#148) and the actual cast (#153) from the animation that references it.
    // Best-effort: a lookup failure means missing tags, never a failed rendition.
    const auto animationFallback = [&sourceWav, &renditionSpan]() -> ws::SoundRenditionService::FallbackMetadata {
        ws::SoundRenditionService::FallbackMetadata metadata;
        if (!creatures::db)
            return metadata;
        auto lookup = creatures::db->findAnimationSoundInfoBySoundFile(sourceWav, renditionSpan);
        if (!lookup.isSuccess()) {
            spdlog::warn("Animation lookup for {} failed: {}", sourceWav, lookup.getError().value().getMessage());
            return metadata;
        }
        const auto info = lookup.getValue().value();
        if (!info)
            return metadata;
        metadata.title = info->title;
        for (const auto &name : info->performerNames) {
            if (!metadata.artist.empty())
                metadata.artist += ", ";
            metadata.artist += name;
        }
        return metadata;
    };
    const ws::SoundRenditionService renditionService;
    auto encoded = renditionService.renderWav(resolved->path, format, animationFallback);
    if (!encoded.isSuccess()) {
        const auto error = encoded.getError().value();
        recordSpanError(renditionSpan, error.getMessage(), "RenditionError", error.getCode());
        return errorStatus(error.getCode() == ServerError::InvalidData ? 422 : 500, error.getMessage(), span,
                           "RenditionError");
    }
    const auto rendition = encoded.getValue().value();
    if (renditionSpan) {
        renditionSpan->setAttribute("rendition.output_bytes", static_cast<int64_t>(rendition.bytes.size()));
        renditionSpan->setSuccess();
    }
    if (creatures::metrics)
        creatures::metrics->incrementSoundFilesServed();
    spdlog::info("Rendering sound to {}: {} → {} ({} bytes)", formatName, sourceWav, safeFilename,
                 rendition.bytes.size());

    // Permanent-store sounds are immutable and the encoders are deterministic,
    // so those renditions can be cached forever; ad-hoc basenames are reused.
    const char *cacheControl = resolved->fromPermanentStore ? IMMUTABLE_CACHE_CONTROL : NO_STORE_CACHE_CONTROL;
    if (span) {
        span->setAttribute("rendition.format", formatName);
        span->setAttribute("rendition.bytes", static_cast<int64_t>(rendition.bytes.size()));
        span->setAttribute("http.response.cache_control", cacheControl);
        span->setSuccess();
    }
    return PreparedResponse::bytes(
        200, rendition.mimeType, bytesToString(rendition.bytes),
        {{"Content-Disposition", attachment(safeFilename)}, {"Cache-Control", cacheControl}});
}

PreparedResponse getSoundProvenance(const std::string &filename, const std::shared_ptr<OperationSpan> &span) {
    const auto safe = sanitizeSoundFilename(filename);
    if (!safe.isSuccess())
        return errorStatus(403, safe.getError()->getMessage(), span, "InvalidFilename");
    const auto safeFilename = safe.getValue().value();
    ws::SoundService service;
    // Permanent store first (dialog/ renders are where provenance lives), then ad-hoc.
    const auto resolvedResult = service.resolveSoundPath(safeFilename, span);
    if (!resolvedResult.isSuccess())
        return serverErrorStatus(resolvedResult.getError().value(), span);
    const auto resolved = resolvedResult.getValue().value();
    if (!resolved) {
        return errorStatus(404,
                           fmt::format("Sound '{}' was not found in the sound store or the ad-hoc store", safeFilename),
                           span, "NotFound");
    }
    const auto ixml = creatures::voice::readIxmlChunk(resolved->path);
    if (!ixml) {
        return errorStatus(404, fmt::format("Sound '{}' has no embedded provenance", safeFilename), span, "NotFound");
    }
    if (span) {
        span->setAttribute("sound.source_path", resolved->path);
        span->setAttribute("provenance.bytes", static_cast<int64_t>(ixml->size()));
        span->setSuccess();
    }
    return PreparedResponse::bytes(200, "application/xml; charset=utf-8", *ixml);
}

PreparedResponse getSoundMetadata(const std::string &filename, const std::shared_ptr<OperationSpan> &span) {
    const auto safe = sanitizeSoundFilename(filename);
    if (!safe.isSuccess())
        return errorStatus(403, safe.getError()->getMessage(), span, "InvalidFilename");
    const auto safeFilename = safe.getValue().value();
    ws::SoundService service;
    const auto resolvedResult = service.resolveSoundPath(safeFilename, span);
    if (!resolvedResult.isSuccess())
        return serverErrorStatus(resolvedResult.getError().value(), span);
    const auto resolved = resolvedResult.getValue().value();
    if (!resolved) {
        return errorStatus(404,
                           fmt::format("Sound '{}' was not found in the sound store or the ad-hoc store", safeFilename),
                           span, "NotFound");
    }
    if (span)
        span->setAttribute("sound.source_path", resolved->path);
    const auto metadata = service.buildSoundMetadata(resolved->path, safeFilename, span);
    if (!metadata.isSuccess())
        return serverErrorStatus(metadata.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(soundToJson(metadata.getValue().value())));
}

PreparedResponse getExchangeAudio(const std::string &sessionId, const ExchangeAudioFormat format,
                                  const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::db) {
        return errorStatus(500, "Ad-hoc exchange audio unavailable: database missing", span, "MissingDependencies");
    }
    if (!isUuidShape(sessionId))
        return errorStatus(400, "sessionId must be a UUID", span, "InvalidSessionId");
    const auto canonicalSessionId = canonicalUuid(sessionId);
    if (span)
        span->setAttribute("session.id", canonicalSessionId);
    auto opSpan = childSpan("StreamingAdHocController.serveExchangeAudio", span);
    if (opSpan)
        opSpan->setAttribute("session.id", canonicalSessionId);
    auto lookup = creatures::db->getAdHocExchange(canonicalSessionId, opSpan);
    if (!lookup.isSuccess()) {
        if (opSpan)
            opSpan->setError(lookup.getError()->getMessage());
        return serverErrorStatus(lookup.getError().value(), span);
    }
    const auto record = lookup.getValue().value();
    const auto &exchange = record.exchange;
    if (opSpan) {
        opSpan->setAttribute("exchange.status", exchange.status);
        opSpan->setSuccess();
    }
    if (span)
        span->setAttribute("exchange.status", exchange.status);

    if (exchange.status == EXCHANGE_STATUS_STREAMING) {
        auto response = errorStatus(409, "Exchange is still streaming; try again shortly", span, "ExchangeStreaming");
        response.headers.push_back({"Retry-After", "5"});
        return response;
    }
    if (exchange.status == EXCHANGE_STATUS_FAILED || exchange.sound_file.empty()) {
        return errorStatus(410, "Exchange rendered no audio", span, "ExchangeEmpty");
    }
    const std::filesystem::path wavPath(exchange.sound_file);
    std::error_code existsError;
    if (!std::filesystem::exists(wavPath, existsError) || existsError) {
        return errorStatus(404, "Exchange audio has expired", span, "NotFound");
    }

    /// Download filename in the shared export shape (#126, #152): slugified
    /// title plus a short session-id tail, so identically-worded exchanges
    /// don't collide. e.g. "beaky-somebody-is-at-the-door-e3af1c4d.mp3"
    const auto basename = util::exportBasename(exchange.title.empty() ? exchange.creature_name : exchange.title,
                                               exchange.session_id, 48, "exchange");
    // A UUID-addressed exchange can never change once finalized, and the
    // encoders are deterministic, so immutability is honest here.
    std::vector<HttpHeader> headers{{"Cache-Control", IMMUTABLE_CACHE_CONTROL}};

    if (format == ExchangeAudioFormat::Wav) {
        // Streamed, not buffered (#140): a long exchange's 17-channel WAV runs
        // to hundreds of MB and must never be slurped into memory.
        const auto size = sizeForStreaming(wavPath.string());
        if (!size.isSuccess())
            return errorStatus(404, "Exchange audio has expired", span, "NotFound");
        headers.insert(headers.begin(), {"Content-Disposition", attachment(basename + ".wav")});
        if (span) {
            span->setAttribute("rendition.bytes", static_cast<int64_t>(size.getValue().value()));
            span->setSuccess();
        }
        return PreparedResponse::fileStream(wavPath.string(), size.getValue().value(), "audio/wav", std::move(headers));
    }

    const auto renditionFormat =
        format == ExchangeAudioFormat::Mp3 ? ws::SoundRenditionFormat::Mp3 : ws::SoundRenditionFormat::OggOpus;
    const ws::SoundRenditionService renditionService;
    auto encoded = renditionService.renderWav(wavPath, renditionFormat);
    if (!encoded.isSuccess())
        return serverErrorStatus(encoded.getError().value(), span);
    const auto rendition = encoded.getValue().value();
    headers.insert(headers.begin(), {"Content-Disposition", attachment(basename + rendition.extension)});
    if (span) {
        span->setAttribute("rendition.bytes", static_cast<int64_t>(rendition.bytes.size()));
        span->setSuccess();
    }
    return PreparedResponse::bytes(200, rendition.mimeType, bytesToString(rendition.bytes), std::move(headers));
}

PreparedResponse getDialogPreviewAudio(const std::string &cacheKey, const std::string &filename,
                                       const std::shared_ptr<OperationSpan> &span) {
    // Accept either {id} or {id}.wav (preferred for browser save-as / sniffing).
    std::string gid = filename;
    if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".wav") == 0)
        gid.resize(gid.size() - 4);
    if (!api::isLowercaseSha256(cacheKey))
        return errorStatus(400, "cache_key must be a 64-character lowercase hex sha256", span, "InvalidCacheKey");
    if (!isUuidShape(gid))
        return errorStatus(400, "generation_id must be a UUID", span, "InvalidGenerationId");
    auto loadResult = creatures::voice::loadGeneration(cacheKey, gid);
    if (!loadResult.isSuccess()) {
        return errorStatus(404, fmt::format("generation '{}/{}' not found", cacheKey, gid), span, "NotFound");
    }
    const auto gen = loadResult.getValue().value();
    // Embed provenance (#50). Drop the track list: this is a mono file, so a
    // 17-track layout would misdescribe it; the script text and ids still travel.
    auto monoProvenance = gen.provenance;
    monoProvenance.tracks.clear();
    const auto wavBytes =
        creatures::voice::wrapMonoPcmAsWav(gen.audioPcm, 48000, monoProvenance.empty() ? nullptr : &monoProvenance);
    if (span) {
        span->setAttribute("dialog.cache_key", cacheKey);
        span->setAttribute("dialog.generation_id", gid);
        span->setSuccess();
    }
    return PreparedResponse::bytes(200, "audio/wav", bytesToString(wavBytes),
                                   {{"X-Dialog-Cache-Key", cacheKey}, {"X-Dialog-Generation-Id", gid}});
}

PreparedResponse getDialogPreviewShareable(const std::string &cacheKey, const std::string &filename,
                                           const std::shared_ptr<OperationSpan> &span) {
    // The trailing extension picks the rendition: '.mp3' → MP3 (#58), '.ogg'
    // or no extension → Ogg/Opus. Strip it to recover the bare generation_id.
    std::string gid = filename;
    bool wantMp3 = false;
    if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".mp3") == 0) {
        wantMp3 = true;
        gid.resize(gid.size() - 4);
    } else if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".ogg") == 0) {
        gid.resize(gid.size() - 4);
    }
    if (!api::isLowercaseSha256(cacheKey))
        return errorStatus(400, "cache_key must be a 64-character lowercase hex sha256", span, "InvalidCacheKey");
    if (!isUuidShape(gid))
        return errorStatus(400, "generation_id must be a UUID", span, "InvalidGenerationId");
    auto loadResult = creatures::voice::loadGeneration(cacheKey, gid);
    if (!loadResult.isSuccess()) {
        return errorStatus(404, fmt::format("generation '{}/{}' not found", cacheKey, gid), span, "NotFound");
    }
    const auto gen = loadResult.getValue().value();

    // The cache stores raw S16LE bytes; the encoder wants samples.
    std::vector<int16_t> samples(gen.audioPcm.size() / sizeof(int16_t));
    std::memcpy(samples.data(), gen.audioPcm.data(), samples.size() * sizeof(int16_t));

    auto renditionSpan = childSpan("SoundRenditionService.renderMonoPcm", span);
    if (renditionSpan) {
        renditionSpan->setAttribute("dialog.cache_key", cacheKey);
        renditionSpan->setAttribute("dialog.generation_id", gid);
        renditionSpan->setAttribute("rendition.format", wantMp3 ? "mp3" : "ogg_opus");
        renditionSpan->setAttribute("rendition.input_samples", static_cast<int64_t>(samples.size()));
        renditionSpan->setAttribute("cache.outcome", "bypass");
        renditionSpan->setAttribute("encoding.performed", true);
        renditionSpan->setAttribute("encoding.input_bytes", static_cast<int64_t>(samples.size() * sizeof(int16_t)));
    }
    const ws::SoundRenditionService renditionService;
    auto rendition = renditionService.renderMonoPcm(
        samples, 48000, gen.provenance, wantMp3 ? ws::SoundRenditionFormat::Mp3 : ws::SoundRenditionFormat::OggOpus);
    if (!rendition.isSuccess()) {
        recordSpanError(renditionSpan, rendition.getError().value().getMessage(), "RenditionError",
                        rendition.getError().value().getCode());
        return serverErrorStatus(rendition.getError().value(), span);
    }
    const auto rendered = rendition.getValue().value();
    if (renditionSpan) {
        renditionSpan->setAttribute("rendition.output_bytes", static_cast<int64_t>(rendered.bytes.size()));
        renditionSpan->setSuccess();
    }
    const auto shareName = fmt::format("dialog-preview-{}{}", gid.substr(0, 8), rendered.extension);
    if (span) {
        span->setAttribute("dialog.cache_key", cacheKey);
        span->setAttribute("dialog.generation_id", gid);
        span->setAttribute("rendition.format", wantMp3 ? "mp3" : "ogg");
        span->setAttribute("share.bytes", static_cast<int64_t>(rendered.bytes.size()));
        span->setAttribute("http.response.cache_control", IMMUTABLE_CACHE_CONTROL);
        span->setSuccess();
    }
    return PreparedResponse::bytes(200, rendered.mimeType, bytesToString(rendered.bytes),
                                   {{"Content-Disposition", attachment(shareName)},
                                    {"Cache-Control", IMMUTABLE_CACHE_CONTROL},
                                    {"X-Dialog-Cache-Key", cacheKey},
                                    {"X-Dialog-Generation-Id", gid}});
}

PreparedResponse getGeneratedMusicMp3(const std::string &filename, const bool dialogRoute,
                                      const std::shared_ptr<OperationSpan> &span) {
    std::string generationId = filename;
    if (!generationId.ends_with(".mp3"))
        return errorStatus(422, "generated music URLs must end in .mp3", span, "InvalidRenditionName");
    generationId.resize(generationId.size() - 4);
    if (!isUuidShape(generationId))
        return errorStatus(400, "music generation id must be a UUID", span, "InvalidGenerationId");
    if (span)
        span->setAttribute("music.generation_id", generationId);

    // One mutex for both aliases: they serve the same cached candidate, so a
    // concurrent first fetch through either URL must not encode twice (#202).
    static std::mutex renditionMutex;
    ws::SoundRenditionService renditionService;
    auto rendition = ws::renderMusicCandidateMp3(generationId, renditionService, renditionMutex, span);
    if (!rendition.isSuccess())
        return serverErrorStatus(rendition.getError().value(), span);
    const auto rendered = rendition.getValue().value();
    const auto downloadName =
        (dialogRoute ? util::bgmExportBasename(rendered.generation.title, rendered.generation.prompt, generationId)
                     : util::musicExportBasename(rendered.generation.title, generationId)) +
        ".mp3";
    if (span) {
        span->setAttribute("rendition.bytes", static_cast<int64_t>(rendered.bytes.size()));
        span->setAttribute("http.response.cache_control", IMMUTABLE_CACHE_CONTROL);
        span->setSuccess();
    }
    return PreparedResponse::bytes(200, rendered.mimeType, bytesToString(rendered.bytes),
                                   {{"Content-Disposition", attachment(downloadName)},
                                    {"Cache-Control", IMMUTABLE_CACHE_CONTROL},
                                    {"X-Music-Generation-Id", generationId}});
}

} // namespace creatures::transport
