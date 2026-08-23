
#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// Disable shadow warnings for MongoDB C++ driver headers (third-party code)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

#include <bsoncxx/array/element.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>

#pragma GCC diagnostic pop

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "model/AdHocExchange.h"
#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Creature.h"
#include "model/DialogScript.h"
#include "model/DmxFixture.h"
#include "model/Playlist.h"
#include "model/SortBy.h"
#include "model/Stage.h"
#include "model/Storyboard.h"
#include "model/Track.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures {

struct AdHocAnimationRecord {
    creatures::Animation animation;
    std::chrono::system_clock::time_point createdAt;
};

struct AdHocExchangeRecord {
    creatures::AdHocExchange exchange;
    std::chrono::system_clock::time_point createdAt;
};

class Database {

  public:
    explicit Database(const std::string &mongoURI_);

    // Creature stuff
    Result<creatures::Creature> getCreature(const creatureId_t &creatureId,
                                            const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::Creature>>
    getAllCreatures(creatures::SortBy sortBy, bool ascending,
                    const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<json> getCreatureJson(const creatureId_t &creatureId,
                                 const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /**
     * Upsert a creature in the database
     *
     * @param creatureJson The full JSON string of the creature. It's stored in the database (as long as all of the
     *                     needed fields are there) so that the controller and console get get a full view of what
     *                     the creature actually is.
     *
     * @return a `Result<creatures::Creature>` with the encoded creature that we can return to the client
     */
    Result<creatures::Creature> upsertCreature(const std::string &creatureJson,
                                               const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /**
     * Validates that the JSON for a Creature contains the fields we expect.
     *
     * It only validates `inputs` if it exists!
     *
     * @param json the JSON to validate
     * @return true if good, or ServerError if not
     */
    static Result<bool> validateCreatureJson(const nlohmann::json &json);

    /**
     * Validates that the JSON for an Animation contains the fields we expect.
     *
     * @param json the JSON to validate
     * @return true if good, or ServerError if not
     */
    static Result<bool> validateAnimationJson(const nlohmann::json &json);

    /**
     * Validates that the JSON for an Playlist contains the fields we expect.
     *
     * @param json the JSON to validate
     * @return true if good, or ServerError if not
     */
    static Result<bool> validatePlaylistJson(const nlohmann::json &json);

    /**
     * Helper function that checks if a JSON object has all of the required fields. Used
     * heavily by `validate[Thing]Json()`.
     *
     * @param j the JSON object to check
     * @param required_fields an array of fields to validate
     * @return true if all fields are present, or a ServerError if not
     */
    static Result<bool> has_required_fields(const nlohmann::json &j, const std::vector<std::string> &required_fields);

    /*
     * Since the format of the Animations changed completely in Animations 2.0, I'm just removing
     * the old gRPC-based stuff. It's not worth trying to port it over.
     */

    // Animation stuff
    Result<json> getAnimationJson(const animationId_t &animationId,
                                  const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::Animation> getAnimation(const animationId_t &animationId,
                                              const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::AnimationMetadata>>
    listAnimations(creatures::SortBy sortBy, const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::Animation> upsertAnimation(const std::string &animationJson,
                                                 const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<void> deleteAnimation(const animationId_t &animationId,
                                 const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /// How many animations reference this sound file (matched on
    /// metadata.sound_file). Used before deleting a superseded dialog render's
    /// audio, so a file shared by more than one animation is never removed.
    Result<int64_t> countAnimationsBySoundFile(const std::string &soundFile,
                                               const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /// What an animation document can tell a rendition about its sound file:
    /// the display title and who actually performs (resolved from the tracks'
    /// creature ids, in track order). #148 for the title, #153 for the
    /// performers — an iXML TRACK_LIST is a channel map, not a cast list, so
    /// the animation's tracks are the truth about who is in the piece.
    struct AnimationSoundInfo {
        std::string title;                       // may be empty
        std::vector<std::string> performerNames; // may be empty (unresolvable creatures are skipped)
    };

    /// Info for the animation whose metadata.sound_file ends in this basename
    /// (dialog renders store a relative path like "dialog/<uuid>.wav", while the
    /// rendition endpoints are addressed by bare basename). Empty optional when
    /// no animation references the file.
    Result<std::optional<AnimationSoundInfo>>
    findAnimationSoundInfoBySoundFile(const std::string &soundFileBasename,
                                      const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /// One animation rendered against a stage, and whether the stage has been
    /// edited since (#119). Enough for the console to say "5 animations, 3 out
    /// of date" without pulling the (large) track blobs.
    struct StageAnimationRef {
        std::string animation_id;
        std::string title;
        std::string source_script_id;
        int64_t source_stage_updated_at{0};
        bool stale{false};
    };

    /// Every animation rendered against `stageId`, newest stage-stamp first.
    /// `stageUpdatedAt` is the stage's CURRENT updated_at; anything rendered
    /// against an older stamp is flagged stale.
    Result<std::vector<StageAnimationRef>>
    listAnimationsBySourceStageId(const std::string &stageId, int64_t stageUpdatedAt,
                                  const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /// Find an existing permanent Animation rendered from the given DialogScript
    /// (matches on metadata.source_script_id). Returns the matching animation_id
    /// in the optional, or an empty optional if nothing is rendered for this
    /// script yet. Used by the JobWorker's re-render path so a script's
    /// previously-rendered Animation gets overwritten in place rather than
    /// accumulating duplicates (3.15.4+). Only searches the permanent
    /// collection — adhoc renders are TTL'd and don't dedupe.
    Result<std::optional<animationId_t>>
    findAnimationIdBySourceScriptId(const std::string &scriptId, const std::string &stageId,
                                    const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::string> playStoredAnimation(const animationId_t &animationId, universe_t universe,
                                            const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    // Playlist stuff
    Result<json> getPlaylistJson(const playlistId_t &playlistId,
                                 const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::Playlist>>
    getAllPlaylists(const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::Playlist> getPlaylist(const playlistId_t &playlistId,
                                            const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::Playlist> upsertPlaylist(const std::string &playlistJson,
                                               const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    // DMX Fixture stuff
    Result<creatures::DmxFixture> getFixture(const fixtureId_t &fixtureId,
                                             const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<json> getFixtureJson(const fixtureId_t &fixtureId,
                                const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::DmxFixture>>
    getAllFixtures(const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::DmxFixture> upsertFixture(const std::string &fixtureJson,
                                                const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<void> deleteFixture(const fixtureId_t &fixtureId,
                               const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<void> setFixtureUniverse(const fixtureId_t &fixtureId, std::optional<universe_t> universe,
                                    const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    static Result<bool> validateFixtureJson(const nlohmann::json &json);

    /**
     * Public wrapper around the private `fixtureFromJson` for callers that only need to parse + validate
     * a fixture config (e.g. validate-only endpoints).
     */
    static Result<creatures::DmxFixture> parseFixtureJson(json fixtureJson,
                                                          std::shared_ptr<OperationSpan> parentSpan = nullptr);

    // Dialog Script stuff — editable, persisted multi-character dialog scenes
    // (see DialogScriptController). The render endpoint can take a script_id
    // and snapshot the script's turns onto the resulting Animation.
    Result<creatures::DialogScript> getDialogScript(const scriptId_t &scriptId,
                                                    const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<json> getDialogScriptJson(const scriptId_t &scriptId,
                                     const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::DialogScript>>
    listDialogScripts(const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::DialogScript> upsertDialogScript(const std::string &scriptJson,
                                                       const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<void> deleteDialogScript(const scriptId_t &scriptId,
                                    const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /// Parse + validate a DialogScript JSON document without persisting. Server-managed
    /// fields (`id`, `created_at`, `updated_at`) are tolerated if present but never
    /// trusted from the client — the controller stamps them.
    static Result<creatures::DialogScript> parseDialogScriptJson(json scriptJson,
                                                                 std::shared_ptr<OperationSpan> parentSpan = nullptr);

    // Storyboard stuff — client-driven CRUD of opaque storyboard documents
    // (see StoryboardController). The server is a dumb persistence layer:
    // it stores tiles[].action verbatim and never interprets it. See
    // creature-console/docs/storyboard-server-contract.md.
    Result<creatures::Storyboard> getStoryboard(const storyboardId_t &storyboardId,
                                                const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::Storyboard>>
    listStoryboards(const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::Storyboard> upsertStoryboard(const std::string &storyboardJson,
                                                   const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    // Stage stuff — where each creature sits and which way it faces (#119).
    // Geometry the server reads (creature_id/x/y/z/yaw) is validated; the
    // console-owned extras ride along opaque. See model/Stage.h.
    Result<creatures::Stage> getStage(const stageId_t &stageId,
                                      const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::Stage>> listStages(const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::Stage> upsertStage(const std::string &stageJson,
                                         const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<void> deleteStage(const stageId_t &stageId, const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /// Parse + validate a Stage JSON document without persisting.
    static Result<creatures::Stage> parseStageJson(json stageJson, std::shared_ptr<OperationSpan> parentSpan = nullptr);

    Result<void> deleteStoryboard(const storyboardId_t &storyboardId,
                                  const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /// Parse + validate a Storyboard JSON document without persisting. Server-managed
    /// fields (`id`, `created_at`, `updated_at`) are tolerated if present but never
    /// trusted from the client — the controller stamps them. tiles[].action is treated
    /// as opaque (object check only, no key introspection).
    static Result<creatures::Storyboard> parseStoryboardJson(json storyboardJson,
                                                             std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /// Transitional wrapper around the framework-neutral Track codec.
    static Result<creatures::Track> parseTrackJson(json trackJson);

    /// Transitional wrapper around the framework-neutral Animation API codec.
    static Result<creatures::Animation> parseAnimationJson(json animationJson);

    /**
     * Ensure supporting indexes (including TTL) for the ad-hoc animation collection exist.
     */
    Result<void> ensureAdHocAnimationIndexes(uint32_t ttlHours);

    /**
     * Insert a freshly generated ad-hoc animation into the TTL collection.
     */
    Result<void> insertAdHocAnimation(const creatures::Animation &animation,
                                      std::chrono::system_clock::time_point createdAt,
                                      std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<std::vector<AdHocAnimationRecord>> listAdHocAnimations(std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<creatures::Animation> getAdHocAnimation(const animationId_t &animationId,
                                                   std::shared_ptr<OperationSpan> parentSpan = nullptr);

    // Ad-hoc exchange stuff — one document per streaming ad-hoc session, so a
    // whole conversation turn can be exported as a single file (issue #150).
    // Same TTL story as the ad-hoc animations the parts reference.
    Result<void> ensureAdHocExchangeIndexes(uint32_t ttlHours);
    Result<void> insertAdHocExchange(const creatures::AdHocExchange &exchange,
                                     std::chrono::system_clock::time_point createdAt,
                                     std::shared_ptr<OperationSpan> parentSpan = nullptr);
    /// Overwrite the mutable fields of an exchange (matched on session_id) once
    /// the session finishes: status, title, transcript, sound_file, duration,
    /// finished_at, parts. The BSON created_at (TTL clock) is left untouched.
    Result<void> finalizeAdHocExchange(const creatures::AdHocExchange &exchange,
                                       std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<std::vector<AdHocExchangeRecord>> listAdHocExchanges(int limit,
                                                                std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<AdHocExchangeRecord> getAdHocExchange(const std::string &sessionId,
                                                 std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Request that the database perform a health check
     *
     * This is what updates the serverPingable flag
     */
    void performHealthCheck();

    /**
     * Can the server be pinged?
     *
     * @return true if the server is pingable
     */
    bool isServerPingable() const;

    /**
     * Parse and validate a creature config JSON document without persisting it.
     */
    Result<creatures::Creature> parseCreatureJson(json creatureJson,
                                                  std::shared_ptr<OperationSpan> parentSpan = nullptr);

  protected:
    /**
     * Check to see if a field is present in a JSON object
     *
     * @param jsonObj the object to check
     * @param fieldName the field to look for
     * @return true if it's there, or a ServerError if it's not
     */
    static Result<bool> checkJsonField(const nlohmann::json &jsonObj, const std::string &fieldName);

  private:
    std::string mongoURI;
    mongocxx::pool mongoPool;

    Result<mongocxx::collection> getCollection(const std::string &collectionName);

    static Result<creatures::Creature> creatureFromJson(json creatureJson,
                                                        std::shared_ptr<OperationSpan> parentSpan = nullptr);
    /// Adapter for MongoDB documents. `_id` is database-owned metadata, not
    /// controller configuration, so it is removed before strict config parsing.
    static Result<creatures::Creature> creatureFromStoredJson(json creatureJson,
                                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

    static Result<creatures::DmxFixture> fixtureFromJson(json fixtureJson,
                                                         std::shared_ptr<OperationSpan> parentSpan = nullptr);

    static Result<creatures::DialogScript> dialogScriptFromJson(json scriptJson,
                                                                std::shared_ptr<OperationSpan> parentSpan = nullptr);

    static Result<creatures::Stage> stageFromJson(json stageJson, std::shared_ptr<OperationSpan> parentSpan = nullptr);

    static Result<creatures::Storyboard> storyboardFromJson(json storyboardJson,
                                                            std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /*
     * Playlists
     */
    static Result<creatures::Playlist> playlistFromJson(json playlistJson,
                                                        std::shared_ptr<OperationSpan> parentSpan = nullptr);
    static Result<creatures::PlaylistItem> playlistItemFromJson(json playlistItemJson,
                                                                std::shared_ptr<OperationSpan> parentSpan = nullptr);

    // Start out thinking that the server is pingable
    std::atomic<bool> serverPingable{true};
};

} // namespace creatures
