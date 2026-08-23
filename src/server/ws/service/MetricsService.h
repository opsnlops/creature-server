
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

#include "server/metrics/counters.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/dto/SystemCountersDto.h"

namespace creatures ::ws {

class MetricsService {

  private:
    typedef oatpp::web::protocol::http::Status Status;

  public:
    oatpp::Object<creatures::SystemCountersDto> getCounters();
};

} // namespace creatures::ws
