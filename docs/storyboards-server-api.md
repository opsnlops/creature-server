# Storyboards — Server API Reference

This is the **server-side counterpart** to `creature-console/docs/storyboard-server-contract.md` — what the server actually implements. Use this to cross-check the client's expectations against shipped behavior.

**Shipped in:** 3.17.0
**Implementation plan:** [storyboards-server-plan.md](storyboards-server-plan.md)

## Quick reference

| Method | Path | Body | Success | Notable errors |
|---|---|---|---|---|
| GET | `/api/v1/storyboard` | — | `200` `{count, items: [...]}` newest-first by `updated_at` | 500 |
| GET | `/api/v1/storyboard/{id}` | — | `200` full Storyboard | 400 (id not UUID-shaped), 404 |
| POST | `/api/v1/storyboard` | `UpsertStoryboardRequest` | `201` full Storyboard (server-stamped id + timestamps) | 400 (JSON shape / validation) |
| PUT | `/api/v1/storyboard/{id}` | `UpsertStoryboardRequest` | `200` full Storyboard (preserves `created_at`, bumps `updated_at`) | 400, 404 (never creates-by-id) |
| DELETE | `/api/v1/storyboard/{id}` | — | `200` `{status: "ok", code: 200, message: "Storyboard deleted"}` | 400, 404 |

All endpoints return `Content-Type: application/json; charset=utf-8`.

## Data shapes

### `Storyboard` (response body for GET / POST / PUT)

```json
{
  "id": "b4f1c0de-1111-2222-3333-444455556666",
  "title": "Halloween Front Porch",
  "notes": "Beaky greets trick-or-treaters; Mango heckles.",
  "tiles": [ /* StoryboardTile[] */ ],
  "created_at": 1748579999000,
  "updated_at": 1748580015000
}
```

| Field | Type | Notes |
|---|---|---|
| `id` | string | UUID, lowercase. Server-stamped on POST. URL is authoritative on PUT. |
| `title` | string | Required, non-empty. **Max 256 chars.** |
| `notes` | string | Optional. **Max 16384 chars.** Stored as empty string when absent. |
| `tiles` | array | **Max 200 tiles.** May be empty. Stored verbatim (see opaque-action rules below). |
| `created_at` | int64 | Epoch milliseconds. Stamped on first insert; never changes after. Server-managed. |
| `updated_at` | int64 | Epoch milliseconds. Bumped on every successful POST/PUT. Server-managed. |

### `StoryboardTile`

```json
{
  "id": "9a7c6b54-aaaa-bbbb-cccc-ddddeeeeffff",
  "x": 0.08, "y": 0.10, "width": 0.22, "height": 0.18,
  "label": "Greet",
  "sf_symbol": "hand.wave",
  "tint_color_hex": "#34C759",
  "action": { "type": "ad_hoc_speech", "creature_id": "e93b9a7a-1704-11ef-84b9-3b37dddeb225", "resume_playlist": true }
}
```

| Field | Type | Server validation |
|---|---|---|
| `id` | string | Required. **Must be UUID-shaped** (8-4-4-4-12 hex, case-insensitive). |
| `x`,`y`,`width`,`height` | number | Stored verbatim. **Not clamped server-side** — that's the client's job. |
| `label` | string | Optional. **Max 256 chars when present.** |
| `sf_symbol` | string | Stored verbatim. Not validated. |
| `tint_color_hex` | string | Stored verbatim. Not validated. |
| `action` | object | Optional. **If present, must be a JSON object.** Contents are **opaque** — no `type` enum, no key whitelisting. |

### `UpsertStoryboardRequest` (POST / PUT body)

```json
{
  "title": "Halloween Front Porch",
  "notes": "...",
  "tiles": [ /* StoryboardTile[] */ ]
}
```

Only the editable fields. **The server overwrites any client-supplied `id`, `created_at`, or `updated_at`** before validation — sending them is silently ignored, not a 400. This matches the lenient style DialogScript adopted in 3.15.1.

### `StatusDto` (error envelope, used for all 4xx/5xx + DELETE success)

```json
{ "status": "error", "code": 400, "message": "...", "session_id": null }
```

`status` is one of: `"ok"` (2xx success on DELETE), `"error"` (4xx other than 404, or 5xx), `"not_found"` (404). `session_id` is null for storyboards; it's a vestigial field of the shared envelope.

## Opaque action handling — the forward-compat seam

The server stores `tiles[].action` **verbatim** and serves it back unchanged. This is the load-bearing correctness property for the feature.

What this means in practice:
- Any `action.type` the client invents will round-trip. The server doesn't enforce a vocabulary.
- Any nested keys inside `action` round-trip — `action.options.tags[]`, `action.nested.thing.foo`, anything.
- Mongo storage uses `JsonParser::jsonStringToBson` directly on the original request body, so unknown keys land in BSON untouched.
- HTTP responses bypass oatpp's DTO serializer (which would strip unknown keys) and emit raw JSON via `ResponseFactory::createResponse`.

The only thing the server checks about `action`: when present, it must be a JSON object (not a string, array, or null). A 400 results otherwise — the `type` discriminator can't live anywhere else.

A regression test in `tests/server/storyboard/StoryboardParse_test.cpp::OpaqueActionRoundTrip` pins this — it sends a tile with `"type": "future_action_xyz"` plus arbitrary nested fields and asserts byte-equal deep equality after parse + serialize.

## Server-managed fields

- **`id`** — server generates a UUID on POST. On PUT, the URL `{id}` is authoritative; body `id` is overwritten.
- **`created_at`** — set to `now()` on first POST; preserved on every PUT by looking up the existing record before write.
- **`updated_at`** — bumped to `now()` on every successful POST and PUT.

All three are stamped **after** the request body is parsed, then re-validated. Sending them in the request body is harmless — they get overwritten.

## Cache invalidation

After any successful POST / PUT / DELETE, the server broadcasts:

```json
{ "command": "cache-invalidate", "payload": { "cache_type": "storyboard-list" } }
```

Over the existing WebSocket. The broadcast is scheduled with a 50-frame delay (≈ standard `CACHE_INVALIDATION_DELAY_TIME`) to avoid races where the client receives the invalidate before the mutation lands. `cache_type` is the lowercase-kebab `storyboard-list` (added to the `CacheType` enum + string mapping).

GET requests do **not** broadcast invalidation.

## Validation summary

The DB-layer parser (`Database::parseStoryboardJson`) enforces:

| Rule | Failure |
|---|---|
| body parses as JSON object | 400 — `Invalid JSON: ...` |
| `id` present, non-empty string, UUID-shaped | 400 |
| `title` present, non-empty string, ≤ 256 chars | 400 |
| `notes` if present, string, ≤ 16384 chars | 400 |
| `tiles` present, JSON array, ≤ 200 entries | 400 |
| each tile is an object with `id` present, non-empty string, UUID-shaped | 400 |
| each tile's `label` if present, ≤ 256 chars | 400 |
| each tile's `action` if present, JSON object | 400 |
| path param `{id}` on GET / PUT / DELETE is UUID-shaped | 400 — caught at controller before parser |

That's the full list. **Nothing else is validated** — no `x`/`y` range, no `tint_color_hex` regex, no `action.type` enum, no per-tile field count, no sf_symbol shape. The client is the source of truth for what's a valid storyboard inside the caps the server enforces.

## Live verification

```bash
BASE=https://server.prod.chirpchirp.dev/api/v1/storyboard

# Create
curl -X POST -H 'Content-Type: application/json' -d @example.json $BASE
# → 201, body is the full Storyboard with server-stamped id + timestamps

# List
curl $BASE
# → 200, {"count": N, "items": [...]}

# Get
curl $BASE/<id>
# → 200, full Storyboard

# Update (server preserves created_at, bumps updated_at)
curl -X PUT -H 'Content-Type: application/json' -d @example.json $BASE/<id>
# → 200, full Storyboard

# Delete
curl -X DELETE $BASE/<id>
# → 200, {"status":"ok","code":200,"message":"Storyboard deleted","session_id":null}

# Bad UUID shape
curl $BASE/not-a-uuid
# → 400, {"status":"error","code":400,"message":"storyboardId must be a UUID",...}

# Missing storyboard
curl $BASE/00000000-0000-0000-0000-000000000000
# → 404, {"status":"not_found","code":404,"message":"Storyboard not found: ...",...}
```

## Out-of-scope (intentional)

These appear in the client contract but the server **does not** implement them, by design:

- **No `/validate` endpoint.** The contract doesn't ask for one. POST returns a 400 with the same validator output if a body is invalid.
- **No tile-action `type` enum.** Forward-compat requires opaqueness; the table of v1 action types in the client contract is informational.
- **No clamping of tile `x`/`y`/`width`/`height` to [0,1].** Stored verbatim. Contract says client clamps; server may but doesn't have to.
- **No tile re-ordering or de-duplication.** Tiles stored in submitted order.

## Cross-references

- Client contract (source of truth for shape): `creature-console/docs/storyboard-server-contract.md`
- Implementation plan: [storyboards-server-plan.md](storyboards-server-plan.md)
- Shared error envelope and helpers: `src/server/ws/controller/HttpResponseHelpers.h` (issue #16)
- DB observability spans: [database-observability.md](database-observability.md) (issue #17)
