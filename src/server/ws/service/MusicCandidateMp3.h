#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "server/voice/MusicGenerationCache.h"
#include "server/ws/service/SoundRenditionService.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/Sha256.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::ws {

/// A candidate's immutable MP3 audition rendition, served by both the dialog
/// music route and the library alias (#202). Encodes once on a miss and
/// stores the bytes beside the WAV; `renditionMutex` serialises misses so
/// concurrent first fetches don't multiply encoder work.
struct MusicCandidateMp3 {
    std::vector<uint8_t> bytes;
    std::string mimeType;
    voice::CachedMusicGeneration generation;
};

template <typename SpanT>
Result<MusicCandidateMp3> renderMusicCandidateMp3(const std::string &generationId,
                                                  SoundRenditionService &renditionService, std::mutex &renditionMutex,
                                                  const SpanT &span) {
    using RenderResult = Result<MusicCandidateMp3>;
    auto generation = voice::loadMusicGeneration(generationId);
    if (!generation.isSuccess())
        return RenderResult{generation.getError().value()};
    const auto cached = generation.getValue().value();
    auto renditionSpan =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("SoundRenditionService.renderWav", span)
            : nullptr;
    if (renditionSpan) {
        renditionSpan->setAttribute("music.generation_id", generationId);
        renditionSpan->setAttribute("rendition.format", "mp3");
        renditionSpan->setAttribute("sound.source", "music_generation_cache");
        renditionSpan->setAttribute("sound.file_hash", util::sha256Hex(cached.wavPath.string()));
    }
    MusicCandidateMp3 rendered;
    rendered.generation = cached;
    rendered.mimeType = "audio/mpeg";
    {
        const std::scoped_lock renditionLock(renditionMutex);
        auto stored = voice::loadMusicMp3(generationId);
        if (!stored.isSuccess()) {
            recordSpanError(renditionSpan, stored.getError().value().getMessage(), "RenditionCacheError",
                            stored.getError().value().getCode());
            return RenderResult{stored.getError().value()};
        }
        if (auto bytes = stored.getValue().value()) {
            rendered.bytes = std::move(*bytes);
            if (renditionSpan) {
                renditionSpan->setAttribute("cache.hit", true);
                renditionSpan->setAttribute("cache.outcome", "hit");
                renditionSpan->setAttribute("encoding.performed", false);
            }
        } else {
            if (renditionSpan) {
                std::error_code fileSizeError;
                const auto inputBytes = std::filesystem::file_size(cached.wavPath, fileSizeError);
                if (!fileSizeError)
                    renditionSpan->setAttribute("encoding.input_bytes", static_cast<int64_t>(inputBytes));
            }
            auto rendition = renditionService.renderWav(cached.wavPath, SoundRenditionFormat::Mp3);
            if (!rendition.isSuccess()) {
                recordSpanError(renditionSpan, rendition.getError().value().getMessage(), "RenditionError",
                                rendition.getError().value().getCode());
                return RenderResult{rendition.getError().value()};
            }
            rendered.bytes = rendition.getValue().value().bytes;
            rendered.mimeType = rendition.getValue().value().mimeType;
            auto saved = voice::saveMusicMp3(generationId, rendered.bytes);
            if (!saved.isSuccess()) {
                recordSpanError(renditionSpan, saved.getError().value().getMessage(), "RenditionCacheError",
                                saved.getError().value().getCode());
                return RenderResult{saved.getError().value()};
            }
            if (renditionSpan) {
                renditionSpan->setAttribute("cache.hit", false);
                renditionSpan->setAttribute("cache.outcome", "miss");
                renditionSpan->setAttribute("encoding.performed", true);
            }
        }
    }
    if (renditionSpan) {
        renditionSpan->setAttribute("rendition.output_bytes", static_cast<int64_t>(rendered.bytes.size()));
        renditionSpan->setSuccess();
    }
    return RenderResult{std::move(rendered)};
}

} // namespace creatures::ws
