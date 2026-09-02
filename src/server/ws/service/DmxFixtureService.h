
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "api/FixtureResponses.h"
#include "model/DmxFixture.h"

namespace creatures {
class RequestSpan;
class OperationSpan;
} // namespace creatures

namespace creatures ::ws {

class DmxFixtureService {

  public:
    static Result<std::vector<DmxFixture>> getAllFixtures(std::shared_ptr<RequestSpan> parentSpan = nullptr);
    static Result<std::vector<DmxFixture>> getAllFixturesFromOperation(std::shared_ptr<OperationSpan> parentSpan);

    static Result<DmxFixture> getFixture(const fixtureId_t &fixtureId,
                                         std::shared_ptr<RequestSpan> parentSpan = nullptr);
    static Result<DmxFixture> getFixtureFromOperation(const fixtureId_t &fixtureId,
                                                      std::shared_ptr<OperationSpan> parentSpan);

    static Result<DmxFixture> upsertFixture(const std::string &jsonFixture,
                                            std::shared_ptr<RequestSpan> parentSpan = nullptr);

    static Result<void> deleteFixture(const fixtureId_t &fixtureId, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Persist a universe assignment for a fixture and update the runtime map.
     * @param universe nullopt clears the assignment.
     */
    static Result<DmxFixture> setFixtureUniverse(const fixtureId_t &fixtureId, std::optional<universe_t> universe,
                                                 std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Validate a fixture config payload without persisting it.
     */
    static api::FixtureConfigValidationResponse
    validateFixtureConfig(const std::string &jsonFixture, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Trigger a pattern on a fixture, bypassing the binding match. Useful for ad-hoc UI control
     * and testing.
     *
     * @param stopAfterMs nullopt = pattern holds until externally stopped; otherwise the pattern
     *                    is told to stop after `*stopAfterMs` milliseconds (fade-out then starts).
     */
    static Result<DmxFixture> triggerPattern(const fixtureId_t &fixtureId, const std::string &patternId,
                                             std::optional<uint32_t> stopAfterMs,
                                             std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Fire a one-shot pattern that is NOT persisted. The pattern is built from the call
     * arguments and handed to the runner directly. Used by the Creature Console editor
     * to preview unsaved edits — "Fire" can play whatever's on screen without an upsert.
     *
     * Same validation as a saved pattern trigger: every value's channel must exist on the
     * fixture, the fixture must have an `assigned_universe`, and a live-control session
     * (if any) preempts.
     *
     * @param values        per-channel target values. Channel names must exist on the fixture.
     * @param fadeInMs      ramp duration to reach targets (0 = snap)
     * @param fadeOutMs     ramp duration back to pre-pattern values (0 = snap)
     * @param holdMs        hold duration after fade-in (0 = hold until externally stopped)
     * @param stopAfterMs   nullopt = hold; otherwise schedule auto-stop. Validated in (0, 600000].
     */
    static Result<DmxFixture> previewPattern(const fixtureId_t &fixtureId,
                                             const std::vector<std::pair<std::string, uint8_t>> &values,
                                             uint32_t fadeInMs, uint32_t fadeOutMs, uint32_t holdMs,
                                             std::optional<uint32_t> stopAfterMs,
                                             std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Drive a fixture's channels directly with raw DMX values. Used by slider UIs; the
     * server holds the values until `timeoutMs` elapses, then blacks out the fixture.
     * Live control hard-cancels any active pattern on the fixture and blocks new
     * patterns from starting until the live session expires.
     *
     * @param channelValues per-channel (name → 0..255) updates. Unknown channel names
     *                     fail the whole call with 400.
     * @param timeoutMs    auto-blackout deadline in ms from now. Validated to (0, 600000].
     */
    static Result<DmxFixture> setFixtureLive(const fixtureId_t &fixtureId,
                                             const std::vector<std::pair<std::string, uint8_t>> &channelValues,
                                             uint32_t timeoutMs, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Load all persisted fixtures into the cache and rebuild `fixtureUniverseMap` from each fixture's
     * `assigned_universe`. Called once at server startup.
     */
    static void hydrateFromDatabase(std::shared_ptr<OperationSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
