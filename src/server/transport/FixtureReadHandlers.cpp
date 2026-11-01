#include "server/transport/FixtureReadHandlers.h"

#include <cstdint>
#include <utility>

#include "api/JsonResponse.h"
#include "util/UuidValidation.h"

namespace creatures::transport {
namespace {

const char *serverErrorType(const ServerError::Code code) {
    switch (code) {
    case ServerError::NotFound:
        return "NotFound";
    case ServerError::Unauthorized:
        return "Unauthorized";
    case ServerError::Forbidden:
        return "Forbidden";
    case ServerError::InvalidData:
        return "InvalidData";
    case ServerError::DatabaseError:
        return "DatabaseError";
    case ServerError::Conflict:
        return "Conflict";
    default:
        return "InternalError";
    }
}

PreparedResponse errorResponse(const ServerError &error, const std::shared_ptr<OperationSpan> &requestSpan) {
    const auto statusCode = serverErrorToStatusCode(error.getCode());
    if (requestSpan) {
        requestSpan->setAttribute("error.type", serverErrorType(error.getCode()));
        requestSpan->setAttribute("error.code", static_cast<int64_t>(statusCode));
        requestSpan->setAttribute("error.message", error.getMessage());
        requestSpan->setAttribute("server.error.code", static_cast<int64_t>(error.getCode()));
        requestSpan->setError(error.getMessage());
    }
    return PreparedResponse::json(statusCode, api::jsonToString(api::statusResponseToJson(
                                                  api::makeStatusResponse(statusCode, error.getMessage()))));
}

} // namespace

PreparedResponse listFixtures(const std::shared_ptr<OperationSpan> &requestSpan, const FixtureListLoader &loader) {
    const auto result = loader(requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }

    const auto fixtures = result.getValue().value();
    if (requestSpan) {
        requestSpan->setAttribute("fixtures.count", static_cast<int64_t>(fixtures.size()));
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::listResponseToJson(fixtures, dmxFixtureToJson)));
}

PreparedResponse getFixture(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &requestSpan,
                            const FixtureLoader &loader) {
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", fixtureId);
    }
    if (!isUuidShape(fixtureId)) {
        return errorResponse(ServerError(ServerError::InvalidData, "fixtureId must be a UUID"), requestSpan);
    }

    const auto result = loader(fixtureId, requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    if (requestSpan) {
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(dmxFixtureToJson(result.getValue().value())));
}

} // namespace creatures::transport
