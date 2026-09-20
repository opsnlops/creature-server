#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"
#include "server/ws/service/SoundRenditionService.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

/**
 * File and rendition routes. Large source WAVs come back as a streamed
 * `FilePayload` so neither the application worker nor the loop ever holds a
 * whole file in memory; encoded renditions (MP3/Ogg, a few MB) are produced
 * in memory exactly as the oat++ controllers did.
 */

/// GET/HEAD /api/v1/sound/{filename} — permanent store, streamed.
PreparedResponse getSoundFile(const std::string &filename, const std::shared_ptr<OperationSpan> &span);

/// GET /api/v1/sound/ad-hoc/{filename} — ad-hoc store, streamed as an attachment.
PreparedResponse getAdHocSoundFile(const std::string &filename, const std::shared_ptr<OperationSpan> &span);

/// GET /api/v1/sound/mp3/{stem}.mp3 and /api/v1/sound/shareable/{stem}.ogg.
PreparedResponse renderSoundRendition(const std::string &filename, ws::SoundRenditionFormat format,
                                      const std::shared_ptr<OperationSpan> &span);

/// GET /api/v1/sound/provenance/{filename} — raw embedded iXML.
PreparedResponse getSoundProvenance(const std::string &filename, const std::shared_ptr<OperationSpan> &span);

/// GET /api/v1/sound/{filename}/metadata — heavy structured metadata as JSON.
PreparedResponse getSoundMetadata(const std::string &filename, const std::shared_ptr<OperationSpan> &span);

enum class ExchangeAudioFormat { Wav, Mp3, OggOpus };

/// GET /api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.{wav,mp3,ogg}.
PreparedResponse getExchangeAudio(const std::string &sessionId, ExchangeAudioFormat format,
                                  const std::shared_ptr<OperationSpan> &span);

/// GET /api/v1/animation/dialog/preview/audio/{cache_key}/{filename} — mono WAV of a cached take.
PreparedResponse getDialogPreviewAudio(const std::string &cacheKey, const std::string &filename,
                                       const std::shared_ptr<OperationSpan> &span);

/// GET /api/v1/animation/dialog/preview/share/{cache_key}/{filename} — MP3 or Ogg of a cached take.
PreparedResponse getDialogPreviewShareable(const std::string &cacheKey, const std::string &filename,
                                           const std::shared_ptr<OperationSpan> &span);

/// GET /api/v1/animation/dialog/music/generated/{generationId}.mp3 and
/// GET /api/v1/music/generated/{generationId}.mp3. `dialogRoute` selects the
/// download-name shape each alias has always used.
PreparedResponse getGeneratedMusicMp3(const std::string &filename, bool dialogRoute,
                                      const std::shared_ptr<OperationSpan> &span);

} // namespace creatures::transport
