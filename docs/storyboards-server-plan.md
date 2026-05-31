# Storyboards — server-side CRUD

## Context

The client team is building a "Storyboards" feature in Creature Console in parallel with this work. A storyboard is a card of visual tiles; each tile carries an opaque `action` object that the client interprets (play animation, fire fixture pattern, run ad-hoc speech, etc.). The server is a **dumb persistence layer** — it stores the storyboard JSON, broadcasts a cache-invalidation, and never interprets tile actions. The client-side contract is pinned in `~/code/creature-console/docs/storyboard-server-contract.md`.

The **load-bearing correctness property** is opaque round-tripping of `tiles[].action`: the server must preserve unknown action types and unknown keys verbatim so old/new clients interoperate.

## Approach

Mirror DialogScript exactly — that's the canonical CRUD-of-JSON template in this codebase. Differences from DialogScript:
- 5 endpoints, no `/validate` (contract doesn't ask for one).
- `tiles` may be empty (vs DialogScript turns which are required non-empty).
- `tiles[].action` is opaque; the C++ struct carries it as `nlohmann::json`, and HTTP responses are emitted as raw JSON via `ResponseFactory::createResponse(...)` rather than `createDtoResponse(convertToDto(...))`. Going through an oatpp DTO would silently strip unknown keys inside `action`, which would break forward compat. The Swagger spec gets `addResponse<oatpp::String>(...)` with a textual reference to the contract — a small spec-quality hit in exchange for the lossless round-trip the contract requires.
- Use the post-#16 `HttpResponseHelpers<Self>` mixin (`bailHttp` / `bailFromServerError` / `okStatus`).

## Files to create

### `src/model/Storyboard.h` + `src/model/Storyboard.cpp`
- Validation caps from the contract:
  ```
  MAX_STORYBOARD_TITLE = 256
  MAX_STORYBOARD_NOTES = 16384
  MAX_STORYBOARD_TILES = 200
  MAX_STORYBOARD_TILE_LABEL = 256
  ```
- `struct Storyboard { std::string id; std::string title; std::string notes; nlohmann::json tiles /* always an array */; int64_t created_at; int64_t updated_at; };` — `tiles` stays as `nlohmann::json` to preserve opaque action shapes through the C++ layer.
- `storyboardToJson(const Storyboard&) → nlohmann::json` — symmetric to `dialogScriptToJson` (`src/model/DialogScript.cpp:62`).
- A lightweight `StoryboardDto` for Swagger docs only (id/title/notes/created_at/updated_at as typed fields, plus `oatpp::Any tiles` for the spec entry). Not used for actual serialization — responses bypass it.

### `src/server/storyboard/helpers.cpp`
- `Database::parseStoryboardJson(nlohmann::json, opSpan) → Result<Storyboard>` — mirrors `Database::parseDialogScriptJson` (`src/server/script/helpers.cpp:152`). Validates:
  - `id` required string non-empty
  - `title` required string non-empty, ≤ `MAX_STORYBOARD_TITLE`
  - `notes` optional string, ≤ `MAX_STORYBOARD_NOTES`
  - `tiles` required array (may be empty), ≤ `MAX_STORYBOARD_TILES`
  - Each tile: `id` required string non-empty, `label` ≤ `MAX_STORYBOARD_TILE_LABEL` if present. **Do NOT introspect `action`** beyond confirming it's an object — this is the forward-compat seam.
  - `created_at` / `updated_at` int64, server-stamped (carried through for round-trip).

### `src/server/storyboard/get.cpp`
- `Database::getStoryboard(id, opSpan) → Result<Storyboard>` — mirrors `Database::getDialogScript` (`src/server/script/get.cpp:147`). Path: BSON → `bsoncxx::to_json` → `parseStoryboardJson`.
- `Database::listStoryboards(opSpan) → Result<std::vector<Storyboard>>` — mirrors `Database::listDialogScripts` (`src/server/script/getall.cpp:35`), sorted `updated_at: -1`.

### `src/server/storyboard/upsert.cpp`
- `Database::upsertStoryboard(const std::string &storyboardJson, opSpan) → Result<Storyboard>` — mirrors `Database::upsertDialogScript` (`src/server/script/upsert.cpp:33`):
  1. `JsonParser::parseJsonString` → nlohmann::json
  2. `parseStoryboardJson` (validates known fields, doesn't touch `action`)
  3. `JsonParser::jsonStringToBson(storyboardJson, …)` — **this is the opaque-preservation step**; whatever was in the original JSON string lands in BSON, including unknown tile fields
  4. `update_one(..., upsert: true)` keyed on `{id}`
- `Database::deleteStoryboard(id, opSpan) → Result<void>` — mirrors `Database::deleteDialogScript` (`src/server/script/upsert.cpp:166`). Returns `NotFound` when `deleted_count == 0`.

All four follow the canonical span structure from `docs/database-observability.md` (issue #17).

### `src/server/ws/controller/StoryboardController.h`
The 5 endpoints, all under `/api/v1/storyboard`:

| Method | Path | Body | Success | Helper for error |
|---|---|---|---|---|
| GET | `/api/v1/storyboard` | — | `200` `{count, items: [...]}` | `bailFromServerError` |
| GET | `/api/v1/storyboard/{id}` | — | `200` storyboard JSON | `bailHttp` for bad UUID, `bailFromServerError` for NotFound |
| POST | `/api/v1/storyboard` | `BODY_STRING` | `201` storyboard JSON | `bailHttp(400)` on JSON/parse error, `bailFromServerError` on DB |
| PUT | `/api/v1/storyboard/{id}` | `BODY_STRING` | `200` storyboard JSON | `bailHttp(400)` for bad UUID, `bailFromServerError` for NotFound on the pre-existence check |
| DELETE | `/api/v1/storyboard/{id}` | — | `200` `okStatus` envelope | `bailHttp(400)` for bad UUID, `bailFromServerError` for NotFound |

Controller uses `runEndpoint(...)` wrapper for tracing (matches DialogScriptController pattern). Mirrors `DialogScriptController`'s `buildScriptJsonForUpsert` helper as `buildStoryboardJsonForUpsert(body, id, created_at, updated_at)` that stamps the server-managed fields onto the parsed nlohmann::json **and removes any client-provided id/created_at/updated_at** before validation (the contract calls for the "lenient" 3.15.1 approach — accept and ignore, don't 400 on unknowns).

Responses are emitted via `ResponseFactory::createResponse(Status::CODE_xxx, jsonStr)` with `Content-Type: application/json; charset=utf-8` — same pattern `DialogPreviewController` uses for its audio/lookup responses. The `okStatus(...)` helper is fine for DELETE since the response is the standard StatusDto envelope, not a storyboard.

After POST/PUT/DELETE success: `scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::StoryboardList)`.

## Files to modify

- **`src/model/CacheInvalidation.h`** — add `StoryboardList` to the enum (after `DialogScriptList`).
- **`src/model/CacheInvalidation.cpp`** — add `STORYBOARD_LIST_CACHE_TYPE = "storyboard-list"` constant, plus the switch arm in `toString` and the if-branch in `cacheTypeFromString`.
- **`src/server/config.h`** — add `#define STORYBOARDS_COLLECTION "storyboards"` next to `DIALOG_SCRIPTS_COLLECTION` (line 35).
- **`src/server/database.h`** — declare the 4 new `Database::*Storyboard*` methods alongside the DialogScript declarations (around line 190), plus `parseStoryboardJson`.
- **`src/server/ws/App.cpp`** — include `StoryboardController.h`, add the swagger-docs registration line at L78, add the runtime router line at L92 (mirrors DialogScriptController exactly two lines above each).
- **`CMakeLists.txt`** — add `src/server/storyboard/*` to the `file(GLOB serverFiles CONFIGURE_DEPENDS …)` block (around line 386, next to `src/server/script/*`). **This is a new directory → busts Phase 1 Docker cache once**; harmless but worth knowing for the next build's CI time. Also add the new test file(s) to the `creature-server-test` target (see Tests below).
- **`VERSION.txt`** — bump minor (`3.16.1` → `3.17.0`) since this is a new feature, not a patch.

## Tests

Add `tests/server/storyboard/StoryboardParse_test.cpp` (the load-bearing properties of this feature live in `parseStoryboardJson` + opaque round-trip):

1. **Validation**: rejects missing/empty title, oversized title, oversized notes, too-many tiles, missing tile id, oversized tile label.
2. **Empty tiles allowed**: parse succeeds with `"tiles": []`.
3. **Opaque action round-trip**: build a storyboard JSON where one tile's `action` contains an unknown `type` (e.g. `"type": "future_action_xyz"`) plus arbitrary nested keys; parse → `storyboardToJson` → expect the exact action JSON back (deep equality). This is the regression test for the forward-compat guarantee.
4. **Server-managed fields ignored**: `buildStoryboardJsonForUpsert` strips client-provided `id`/`created_at`/`updated_at` and replaces them with server values.

Add the test file to `CMakeLists.txt`'s `creature-server-test` source list (the explicit one at line ~644, next to the other `tests/server/ws/...` entries).

## Verification

1. `cd build && ninja` — clean build under `-Wshadow -Wall -Wextra -Wpedantic`.
2. `clang-format -i` on every touched file.
3. `cd build && ./creature-server-test --gtest_filter='Storyboard*'` — new tests pass.
4. `./creature-server-test` — full suite still 112+ passing.
5. Local smoke test against the running server:
   - `curl -X POST -d @example.json https://server.prod.chirpchirp.dev/api/v1/storyboard` (using the contract's full example body) → 201 with id + timestamps stamped.
   - `curl https://server.prod.chirpchirp.dev/api/v1/storyboard` → 200 with the new storyboard in `items`.
   - `curl https://server.prod.chirpchirp.dev/api/v1/storyboard/{id}` → 200 with the **exact action JSON the client sent** (verify deep equality byte-for-byte on a tile with a custom action `type` not in the contract's table).
   - `curl -X DELETE https://server.prod.chirpchirp.dev/api/v1/storyboard/{id}` → 200 okStatus.
   - WebSocket client should see `{"command":"cache-invalidate","payload":{"cache_type":"storyboard-list"}}` after each mutate.
6. Bump VERSION.txt, build amd64 + arm64 debs (`out/debs/`), deploy, re-run the curl checks against the deployed build.

## Out of scope (intentionally)

- No `/validate` endpoint — contract doesn't ask for one.
- No tile-action type validation — that's the client's job; we'd break forward compat by enforcing it.
- No backfill from old data — feature is brand new.
- No swagger DTO for the full nested `action` shape — `oatpp::Any` for the spec entry is sufficient.
