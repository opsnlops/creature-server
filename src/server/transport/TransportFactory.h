#pragma once

#include <memory>

namespace creatures {
class Configuration;
}

namespace creatures::transport {

class TransportServer;

std::shared_ptr<TransportServer> createTransportServer(const Configuration &configuration);

} // namespace creatures::transport
