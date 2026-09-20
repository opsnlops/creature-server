#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

// Music library (MusicController).
PreparedResponse generateMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse planMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse listMusicPieces(const std::shared_ptr<OperationSpan> &span);
PreparedResponse getMusicPiece(const std::string &pieceId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse updateMusicPiece(const std::string &pieceId, const std::string &body,
                                  const std::shared_ptr<OperationSpan> &span);
PreparedResponse deleteMusicPiece(const std::string &pieceId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse refineMusicPiece(const std::string &pieceId, const std::string &body,
                                  const std::shared_ptr<OperationSpan> &span);
PreparedResponse saveMusicCandidate(const std::string &generationId, const std::string &body,
                                    const std::shared_ptr<OperationSpan> &span);

// Shared by both recipe aliases: the same candidate cache, the same shape.
PreparedResponse getMusicCandidateRecipe(const std::string &generationId, const std::shared_ptr<OperationSpan> &span);

// Dialog-bound music (DialogMusicController).
PreparedResponse submitDialogMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse planDialogMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse listMusicFinetunes(const std::shared_ptr<OperationSpan> &span);
PreparedResponse promoteGeneratedMusic(const std::string &generationId, const std::shared_ptr<OperationSpan> &span);

} // namespace creatures::transport
