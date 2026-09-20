#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

/**
 * Routes whose bodies are not JSON documents of the usual size. The transport
 * still collects each body under its own explicit limit; nothing here goes
 * through the JSON parser unless the route is JSON.
 */

/// POST /api/v1/sound/generate-lipsync — small JSON control body; queues a job (202).
PreparedResponse generateLipSync(const std::string &body, const std::shared_ptr<OperationSpan> &span);

/// POST /api/v1/sound/generate-lipsync/upload?filename= — raw WAV body, synchronous Rhubarb run.
PreparedResponse generateLipSyncFromUpload(const std::string &filename, const std::string &wavData,
                                           const std::shared_ptr<OperationSpan> &span);

/// POST /api/v1/stt/transcribe — raw 16 kHz mono float32 PCM body.
PreparedResponse transcribeAudio(const std::string &body, const std::shared_ptr<OperationSpan> &span);

} // namespace creatures::transport
