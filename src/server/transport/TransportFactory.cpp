#include "TransportFactory.h"

#include <memory>

#include "server/config/Configuration.h"
#include "server/transport/TransportServer.h"
#include "server/transport/UWebSocketsServer.h"

namespace creatures::transport {

std::shared_ptr<TransportServer> createTransportServer(const Configuration &configuration) {
    return std::make_shared<UWebSocketsServer>(configuration.getHttpMaxConnections(),
                                               configuration.getHttpMaxConnectionsPerPeer());
}

} // namespace creatures::transport
