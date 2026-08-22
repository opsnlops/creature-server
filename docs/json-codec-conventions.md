# Neutral JSON codec conventions

Creature Server's API and persistence contracts use `nlohmann::json` at the
model/application boundary. HTTP and WebSocket frameworks are transport
adapters: they move serialized bytes but do not define domain types.

## Input

- Parse untrusted JSON through the checked helpers in `model/JsonCodec.h`.
- Reject missing required fields, explicit `null`, wrong types, integer
  overflow, invalid UUIDs, oversized strings/arrays, and unknown fields.
- Optional means absent. An explicitly `null` optional is invalid unless a
  particular contract is documented as nullable.
- Error messages include the full field path so callers can fix the request.
- Keep API parsing separate from persistence normalization. Trusted historical
  records may need narrowly scoped compatibility that public requests must not
  inherit.
- Validate once, at the boundary that constructs the neutral model. Downstream
  services and database code consume the validated value.

## Output

- Serializers emit canonical field names and JSON types directly from neutral
  models or response structs.
- Omit absent optionals. Do not emit `null` as an oat++ compatibility artifact.
- Lists use `api::listResponseToJson`: `{ "count": N, "items": [...] }`.
- Status responses use `api::StatusResponse` and `api::statusResponseToJson`:
  `{ "status", "code", "message", "session_id"? }`.
- Serialize through `api::jsonToString`; malformed legacy UTF-8 is replaced so
  response rendering cannot turn completed work into a misleading retryable
  failure. The HTTP adapter records the repair as `response.utf8.replaced`.
- Preserve opaque client-owned JSON only where the contract explicitly allows
  it, such as stage-placement extras.

## Tests

- Pin exact keys, values, and JSON types for representative responses.
- Test absent optionals separately from explicit `null` inputs.
- Cover missing, wrong-type, overflow, size-limit, UUID, and unknown-field
  failures for every public parser.
- Add round-trip tests only when GET output is intentionally valid POST input.
  Persistence normalization is not automatically a public round-trip contract.
