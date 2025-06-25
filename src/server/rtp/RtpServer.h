//
// Created by April White on 6/24/25.
//

#pragma once

#include <uvgrtp/lib.hh>

namespace creatures :: rtp{

    class RtpServer {

    public:
        RtpServer() = default;
        ~RtpServer() = default;

        void start();
        void stop();

    private:
        uvgrtp::context         ctx;
        uvgrtp::session*        session;
        uvgrtp::media_stream*   stream;
    };

} // creatures :: rtp

