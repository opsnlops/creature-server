//
// Created by April White on 6/24/25.
//

#include <uvgrtp/lib.hh>

#include "server/config.h"
#include "server/namespace-stuffs.h"

#include "RtpServer.h"

namespace creatures :: rtp {

    void RtpServer::start() {
        info("starting the RTP server");

        session = ctx.create_session(RTP_MULTICAST_GROUP);
        stream = session->create_stream(
            RTP_PORT, RTP_PORT,
            RTP_FORMAT_GENERIC,  // dynamic multi-channel PCM
            RCE_SEND_ONLY
        );

        if (!stream) {
            error("Failed to create RTP stream");
            return;
        }
        info("RTP stream created successfully");

        stream->configure_ctx(RCC_MULTICAST_TTL, 16);

    }

    void RtpServer::stop() {
        info("stopping the RTP server");

        if (stream) {
            session->destroy_stream(stream);
            stream = nullptr;
        }

        if (session) {
            ctx.destroy_session(session);
            session = nullptr;
        }

        info("RTP server stopped");
    }

} // creatures :: rtp