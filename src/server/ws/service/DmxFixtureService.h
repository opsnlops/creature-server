
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <oatpp/web/protocol/http/Http.hpp>

#include "model/DmxFixture.h"
#include "server/ws/dto/FixtureConfigValidationDto.h"
#include "server/ws/dto/ListDto.h"

namespace creatures {
class RequestSpan;
class OperationSpan;
} // namespace creatures

namespace creatures ::ws {

class DmxFixtureService {

  private:
    typedef oatpp::web::protocol::http::Status Status;

  public:
    static oatpp::Object<ListDto<oatpp::Object<creatures::DmxFixtureDto>>>
    getAllFixtures(std::shared_ptr<RequestSpan> parentSpan = nullptr);

    static oatpp::Object<creatures::DmxFixtureDto> getFixture(const oatpp::String &inFixtureId,
                                                              std::shared_ptr<RequestSpan> parentSpan = nullptr);

    static oatpp::Object<creatures::DmxFixtureDto> upsertFixture(const std::string &jsonFixture,
                                                                 std::shared_ptr<RequestSpan> parentSpan = nullptr);

    static void deleteFixture(const oatpp::String &inFixtureId, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Persist a universe assignment for a fixture and update the runtime map.
     * @param universe nullopt clears the assignment.
     * The id parameter stays `oatpp::String` because that's what the controller hands us; we convert
     * to `fixtureId_t` internally before talking to the database.
     */
    static oatpp::Object<creatures::DmxFixtureDto>
    setFixtureUniverse(const oatpp::String &inFixtureId, std::optional<universe_t> universe,
                       std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Validate a fixture config payload without persisting it.
     */
    static oatpp::Object<FixtureConfigValidationDto>
    validateFixtureConfig(const std::string &jsonFixture, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Trigger a pattern on a fixture, bypassing the binding match. Useful for ad-hoc UI control
     * and testing.
     *
     * @param stopAfterMs nullopt = pattern holds until externally stopped; otherwise the pattern
     *                    is told to stop after `*stopAfterMs` milliseconds (fade-out then starts).
     */
    static oatpp::Object<creatures::DmxFixtureDto>
    triggerPattern(const oatpp::String &inFixtureId, const oatpp::String &inPatternId,
                   std::optional<uint32_t> stopAfterMs, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Load all persisted fixtures into the cache and rebuild `fixtureUniverseMap` from each fixture's
     * `assigned_universe`. Called once at server startup.
     */
    static void hydrateFromDatabase(std::shared_ptr<OperationSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
