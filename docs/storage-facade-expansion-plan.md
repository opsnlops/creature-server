# Storage facade — expansion plan (issue #11 follow-up, PR #21)

## Context

PR #20 ([3.17.1](https://github.com/opsnlops/creature-server/releases/tag/3.17.1)) shipped the file-storage half of issue #11: a `creatures::storage` facade that couples file writes + animation DB writes with their cache invalidations so the JobWorker handlers can't forget the pairing.

April's comment on the issue: *"There's other places in the codebase where we need a facade. This includes creatures, fixtures, etc. Anywhere that touches the cache invalidators should be locked behind a facade to make it hard to do things wrong."*

The bug class is identical to what PR #20 fixed for file writes, just at the DB-only mutation layer. Every controller currently does:
```cpp
auto result = creatures::db->upsertX(json, span);
if (!result.isSuccess()) { ... }
scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::X);
```
Two steps. Easy to forget. Same footgun.

This PR (3.17.2) covers the remaining DB-only mutations so **every** cache invalidation either lives behind the facade (DB-write paired) or is an explicit, named manual broadcast (debug-button shaped).

## Approach

Extend `creatures::storage` (don't introduce a second facade). The existing `publishAnimation` / `publishAdHocAnimation` / `republishAnimation` already model the right shape — add the same pattern for every other entity, plus a named function for manual broadcasts.

### API additions

```cpp
namespace creatures::storage {

// Each one: db->upsertX(...) + scheduleCacheInvalidationEvent(CacheType::Y).
// Caller can't forget the second step.

Result<creatures::Creature> publishCreature(const std::string &json, std::shared_ptr<OperationSpan> = nullptr);
// upsertCreature + CacheType::Creature

Result<creatures::DmxFixture> publishFixture(const std::string &json, std::shared_ptr<OperationSpan> = nullptr);
// upsertFixture + CacheType::Fixture
Result<void> deleteFixture(const fixtureId_t &id, std::shared_ptr<OperationSpan> = nullptr);
// deleteFixture + CacheType::Fixture
Result<creatures::DmxFixture> setFixtureUniverse(const fixtureId_t &id, std::optional<universe_t>,
                                                  std::shared_ptr<OperationSpan> = nullptr);
// setFixtureUniverse + CacheType::Fixture

Result<creatures::Playlist> publishPlaylist(const std::string &json, std::shared_ptr<OperationSpan> = nullptr);
// upsertPlaylist + CacheType::Playlist

Result<creatures::DialogScript> publishDialogScript(const std::string &json,
                                                    std::shared_ptr<OperationSpan> = nullptr);
// upsertDialogScript + CacheType::DialogScriptList
Result<void> deleteDialogScript(const scriptId_t &id, std::shared_ptr<OperationSpan> = nullptr);
// deleteDialogScript + CacheType::DialogScriptList

Result<creatures::Storyboard> publishStoryboard(const std::string &json,
                                                std::shared_ptr<OperationSpan> = nullptr);
// upsertStoryboard + CacheType::StoryboardList
Result<void> deleteStoryboard(const storyboardId_t &id, std::shared_ptr<OperationSpan> = nullptr);
// deleteStoryboard + CacheType::StoryboardList

Result<void> deleteAnimation(const animationId_t &id, std::shared_ptr<OperationSpan> = nullptr);
// deleteAnimation + CacheType::Animation
// (publishAnimation / republishAnimation already exist from PR #20)

// Explicit broadcast for the few manual cases (DebugController refresh
// buttons, places where the underlying mutation happened outside our process,
// etc.). Same wire effect as the paired publishX but named so it's obvious
// this is a deliberate manual case rather than a forgotten pairing.
void broadcastCacheInvalidation(CacheType type, std::shared_ptr<OperationSpan> = nullptr);

} // namespace creatures::storage
```

### Where the function calls live (service vs controller)

Today: the controllers call `db->upsertX` AND `scheduleCacheInvalidationEvent`, OR the controller calls a service (which calls `db->upsertX`) and then the controller separately fires the invalidation. The split makes the bug easier to introduce — a future service-only caller might do the write without the controller-side invalidation.

After: the publish/delete calls live in **whichever layer already owns the validation + DB call**. For controllers that go directly to `db->upsertX`, the call site swaps to `storage::publishX`. For controllers that delegate to a service, the *service* swaps its `db->upsertX` for `storage::publishX` (so the invalidation is coupled with the DB call, regardless of which layer triggers it). The controller's separate `scheduleCacheInvalidationEvent` line is then deleted.

### Migration sites (~15 call sites across 8 files)

| File | Sites | Replace |
|---|---|---|
| `DialogScriptController.h` | 3 | upsert/upsert/delete + DialogScriptList ×3 → `publishDialogScript` / `publishDialogScript` / `deleteDialogScript` |
| `StoryboardController.h` | 3 | upsert/upsert/delete + StoryboardList ×3 → `publishStoryboard` / `publishStoryboard` / `deleteStoryboard` |
| `DmxFixtureController.h` | 4 | upsert/delete/setUniverse/clearUniverse + Fixture ×4 → `publishFixture` / `deleteFixture` / `setFixtureUniverse` ×2 |
| `CreatureController.h` | 2 | upsertCreature + Creature ×2 → `publishCreature` ×2 (one via service, one via register flow) |
| `AnimationController.h` | 2 | upsertAnimation/deleteAnimation + Animation ×2 → already `publishAnimation` from #20 / new `deleteAnimation` |
| `PlaylistController.h` | 1 | upsertPlaylist + Playlist → `publishPlaylist` |
| `VoiceController.h` | 1 | SoundList after voice generation → fold into the underlying write path (likely `writeSoundFile(Permanent, ...)` already covers it; verify) |
| `DebugController.h` | 3 | manual refresh buttons → `broadcastCacheInvalidation(CacheType::X)` |

Services involved (`AnimationService`, `DmxFixtureService`, `CreatureService`, `PlaylistService`) get their `db->upsertX` / `db->deleteX` lines swapped for `storage::publishX` / `storage::deleteX`. The controllers that consumed these services then delete their now-redundant `scheduleCacheInvalidationEvent` calls.

### Out of scope (deliberately)

- **Mutations that don't fire cache invalidations today.** `setIdleEnabled`, `registerCreature` (if it doesn't invalidate), etc. — these aren't a footgun until they grow an invalidation requirement; revisit then.
- **The legacy LipSync handler's standalone `SoundList` invalidation** (`JobWorker.cpp:524`). The file write is done by Rhubarb shelling out — the manual invalidation is appropriate. If/when the file write moves through `writeSoundFile`, that invalidation gets absorbed.
- **WebSocket message types.** This refactor is about *how clients are told*, not *what messages exist*.

## Files to create

None. All facade additions go into the existing `src/server/storage/Storage.{h,cpp}`. Migrations modify existing files.

## Files to modify

- `src/server/storage/Storage.h` + `Storage.cpp` — ~10 new functions
- `src/server/ws/controller/DialogScriptController.h`
- `src/server/ws/controller/StoryboardController.h`
- `src/server/ws/controller/DmxFixtureController.h`
- `src/server/ws/controller/CreatureController.h`
- `src/server/ws/controller/AnimationController.h`
- `src/server/ws/controller/PlaylistController.h`
- `src/server/ws/controller/VoiceController.h`
- `src/server/ws/controller/DebugController.h`
- `src/server/ws/service/AnimationService.cpp`
- `src/server/ws/service/DmxFixtureService.cpp`
- `src/server/ws/service/CreatureService.cpp`
- `src/server/ws/service/PlaylistService.cpp`
- `CMakeLists.txt` — no new files; the existing storage glob already covers it
- `VERSION.txt` — bump to 3.17.2

## Tests

Extend `tests/server/storage/Storage_test.cpp` (or split into `Storage_publish_test.cpp` if it gets crowded):

For each new publish/delete function, pin the pairing: invoking the function must produce the expected `scheduleCacheInvalidationEvent` call(s). The `FakeWebsocketUtils.cpp` thread-local invalidation log from PR #20 makes this cheap — wrap each test in `clear → call → assert log contents`.

Specifically, these are the regression checks for the footgun:
- `publishCreature` → Creature
- `publishFixture` → Fixture
- `deleteFixture` → Fixture
- `setFixtureUniverse` → Fixture
- `publishPlaylist` → Playlist
- `publishDialogScript` → DialogScriptList
- `deleteDialogScript` → DialogScriptList
- `publishStoryboard` → StoryboardList
- `deleteStoryboard` → StoryboardList
- `deleteAnimation` → Animation
- `broadcastCacheInvalidation(X)` → X
- **Negative cases:** failure-path tests (FakeDatabase stub returns InvalidData) verify NO invalidation fires on DB failure (the "atomic" part of the pairing).

The FakeDatabase additions: stubs for `upsertCreature` (exists), `upsertFixture` (need), `deleteFixture` (need), `setFixtureUniverse` (need), `upsertPlaylist` (need), `upsertDialogScript` (need), `deleteDialogScript` (need), `upsertStoryboard` (need), `deleteStoryboard` (need), `deleteAnimation` (need). Each returns a failure stub by default; tests that need success-paths set a global override (same pattern as the publishAnimation tests once they land).

## Verification

1. `ninja creature-server creature-server-test` clean under `-Wshadow -Wall -Wextra -Wpedantic`
2. `./creature-server-test` — full suite passing (139 + N new storage tests)
3. `clang-format -i` on every touched file
4. **Audit greps:**
   - `grep -rn 'scheduleCacheInvalidationEvent' src/server/ws/controller/ src/server/ws/service/` → empty (everything goes through `storage::publish*` / `storage::deleteX` / `storage::broadcastCacheInvalidation`)
   - `grep -rn 'db->upsert\|db->delete\|db->insert' src/server/ws/controller/ src/server/ws/service/` → only within `creatures::storage` (Storage.cpp) and in DB-read paths that don't need invalidation
5. Live smoke test against deployed 3.17.2:
   - Create a creature → Console refreshes creature list
   - Update a fixture's universe → Console refreshes fixtures
   - Create a playlist → Console refreshes playlists
   - Create + delete a dialog script → Console refreshes script list both times
   - Create + delete a storyboard → Console refreshes storyboard list both times
   - Hit a Debug refresh button → Console refreshes the appropriate cache
6. Bump VERSION, build amd64 + arm64 debs, deploy.

## Risks

- **Service-layer migrations subtly change call ordering.** Today: service does DB write → returns to controller → controller invalidates → returns to client. After: service does DB write + invalidates (atomic). The wire-visible effect is the same; the timing of the invalidation-event schedule moves forward by a few microseconds. Not user-visible but worth flagging.
- **The bigger surface = more places to break.** ~15 call sites across 8 files. Per-site tests + the audit greps catch regressions; the test suite's invalidation-log assertions catch the "forgot to fire X" case directly.
- **Stacked on #20.** PR #21 starts from main *after* #20 lands. If #20 needs revisions, #21 rebases on top. Standard stack workflow.
