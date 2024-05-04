


#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"

#include "server/ws/dto/StatusDto.h"

#include "server/metrics/counters.h"

#include "MetricsService.h"


namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}


namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;

    oatpp::Object<creatures::SystemCountersDto> MetricsService::getCounters() {

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->info("MetricsService::getCounters()");

        bool error = false;
        oatpp::String errorMessage;

        // Make sure we have a metrics object
        if(creatures::metrics == nullptr) {
            errorMessage = "Metrics object is null";
            appLogger->error(std::string(errorMessage));
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, Status::CODE_500, errorMessage)

        // Return a copy of the system metrics as a DTO
        return creatures::metrics->convertToDto();

    }



} // creatures :: ws