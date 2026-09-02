#pragma once
#include "server/transport/HttpTypes.h"
#include <memory>
#include <string>
namespace creatures {
class OperationSpan;
}
namespace creatures::transport {
PreparedResponse listSounds(bool adHoc, const std::shared_ptr<OperationSpan> &span);
PreparedResponse playSound(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse listVoices(const std::shared_ptr<OperationSpan> &span);
PreparedResponse voiceSubscription(const std::shared_ptr<OperationSpan> &span);
PreparedResponse createVoiceFile(const std::string &body, const std::shared_ptr<OperationSpan> &span);
} // namespace creatures::transport
