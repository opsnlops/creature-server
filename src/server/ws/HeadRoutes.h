#pragma once

#include <memory>
#include <set>
#include <string>

#include <oatpp/web/server/HttpRequestHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>
#include <oatpp/web/server/api/Endpoint.hpp>

#include "server/namespace-stuffs.h"
#include "server/ws/controller/ControllerUtils.h"

namespace creatures::ws {

/// Give every GET route a HEAD counterpart (#139).
///
/// RFC 9110: "The HEAD method is identical to GET except that the server MUST
/// NOT send content in the response." oatpp routes on the exact method, so
/// without this an unmapped HEAD falls through to the router's 404 — and
/// reports *that* response's Content-Length, which is how a 28 MiB take looked
/// like a 181-byte stub.
///
/// Doing it here rather than by hand per endpoint is the same reasoning as
/// draining unread request bodies in `runEndpoint`: a rule that holds for
/// every route including ones not written yet, instead of a convention someone
/// has to remember. The wrapper runs the real GET handler and drops the body,
/// so status and headers are identical by construction rather than by a second
/// implementation that can drift.
///
/// Routes that already declare their own HEAD keep it — an explicit handler
/// can trace itself accurately as HEAD, which the generic wrapper can't, since
/// the span is created inside the GET handler.
///
/// WebSocket upgrade routes are skipped. Their GET handler hijacks the
/// connection for the upgrade, which is not a thing to do on behalf of a
/// client that only asked for headers.
inline void registerHeadRoutes(const std::shared_ptr<oatpp::web::server::HttpRouter> &router,
                               const oatpp::web::server::api::Endpoints &endpoints) {

    class HeadRequestHandler : public oatpp::web::server::HttpRequestHandler {
      public:
        explicit HeadRequestHandler(std::shared_ptr<HttpRequestHandler> getHandler)
            : getHandler_(std::move(getHandler)) {}

        std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &request) override {
            return asHeadResponse(getHandler_->handle(request));
        }

      private:
        std::shared_ptr<HttpRequestHandler> getHandler_;
    };

    std::set<std::string> alreadyMapped;
    for (const auto &endpoint : endpoints.list) {
        if (endpoint && endpoint->info() && endpoint->info()->method == "HEAD" && endpoint->info()->path) {
            alreadyMapped.insert(endpoint->info()->path);
        }
    }

    std::size_t added = 0;
    for (const auto &endpoint : endpoints.list) {
        if (!endpoint || !endpoint->info() || !endpoint->handler) {
            continue;
        }
        const auto &info = endpoint->info();
        if (info->method != "GET" || !info->path) {
            continue;
        }
        const std::string path = info->path;
        if (alreadyMapped.count(path) > 0) {
            continue; // hand-written HEAD wins
        }
        if (path.find("websocket") != std::string::npos) {
            continue;
        }
        router->route("HEAD", info->path, std::make_shared<HeadRequestHandler>(endpoint->handler));
        alreadyMapped.insert(path);
        ++added;
    }

    debug("mapped HEAD onto {} GET route(s)", added);
}

} // namespace creatures::ws
