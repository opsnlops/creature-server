#include "TransportFactory.h"

#include <memory>
#include <stdexcept>

#include "server/config/Configuration.h"
#include "server/transport/TransportServer.h"
#include "server/ws/App.h"

#ifdef CREATURE_HAS_UWEBSOCKETS_TRANSPORT
#include "server/transport/UWebSocketsServer.h"
#endif

namespace creatures::transport {

std::shared_ptr<TransportServer> createTransportServer(const Configuration &configuration) {
    switch (configuration.getHttpTransport()) {
    case Configuration::HttpTransport::Oatpp:
        return std::make_shared<creatures::ws::App>();
    case Configuration::HttpTransport::UWebSockets:
#ifdef CREATURE_HAS_UWEBSOCKETS_TRANSPORT
        return std::make_shared<UWebSocketsServer>(configuration.getHttpMaxConnections(),
                                                   configuration.getHttpMaxConnectionsPerPeer());
#else
        throw std::runtime_error(
            "uWebSockets transport was selected, but this binary was built without CREATURE_ENABLE_UWS_TRANSPORT");
#endif
    }
    throw std::runtime_error("Unknown HTTP transport");
}

} // namespace creatures::transport
