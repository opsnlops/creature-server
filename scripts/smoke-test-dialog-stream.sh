#!/usr/bin/env bash
# Live check for the streamed multi-character dialog (issue #186, 3.46.0).
# NOT read-only: it makes the birds talk. Run it against a server whose
# creatures are registered and standing on a stage.
#
# Usage:  BASE=https://server.dev.chirpchirp.dev ./scripts/smoke-test-dialog-stream.sh
#         SPEAKERS="Beaky,Mango"   creature names, in order (default Beaky,Mango)
#         STAGE="Mainstage"        stage title (default: the first stage listed)
#         TOKEN=...                optional Authorization: Bearer
#
# Needs curl and jq.
set -uo pipefail

BASE="${BASE:-http://localhost:8000}"
SPEAKERS="${SPEAKERS:-Beaky,Mango}"
STAGE="${STAGE:-}"
AUTH=(-H 'Content-Type: application/json')
[ -n "${TOKEN:-}" ] && AUTH+=(-H "Authorization: Bearer ${TOKEN}")
# A W3C traceparent, so the whole run is one trace in Honeycomb.
TRACE_ID=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
PARENT_ID=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
AUTH+=(-H "traceparent: 00-${TRACE_ID}-${PARENT_ID}-01")

pass=0 fail=0
ok()  { echo "  ✅ $1"; pass=$((pass+1)); }
bad() { echo "  ❌ $1"; fail=$((fail+1)); }
hdr() { echo; echo "== $1 =="; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# post <path> <json> -> prints status; body in $TMP/body
post() { curl -sS -m 120 "${AUTH[@]}" -o "$TMP/body" -w "%{http_code}" -X POST "$BASE$1" -d "$2"; }
get()  { curl -sS -m 60 "${AUTH[@]}" -o "$TMP/body" -w "%{http_code}" "$BASE$1"; }

hdr "Discover the cast and the stage"
code=$(get /api/v1/creature)
[ "$code" = 200 ] || { bad "GET /creature HTTP $code"; exit 1; }
cp "$TMP/body" "$TMP/creatures"
IDS=()
NAMES=()
IFS=',' read -ra WANTED <<<"$SPEAKERS"
for name in "${WANTED[@]}"; do
  id=$(jq -r --arg n "$name" '.items[] | select(.name == $n) | .id' "$TMP/creatures" | head -1)
  if [ -z "$id" ] || [ "$id" = null ]; then bad "no creature named '$name'"; exit 1; fi
  IDS+=("$id"); NAMES+=("$name")
  ok "$name = $id"
done
code=$(get /api/v1/stage)
[ "$code" = 200 ] || { bad "GET /stage HTTP $code"; exit 1; }
if [ -n "$STAGE" ]; then
  STAGE_ID=$(jq -r --arg t "$STAGE" '.items[] | select(.title == $t) | .id' "$TMP/body" | head -1)
else
  STAGE_ID=$(jq -r '.items[0].id' "$TMP/body")
  STAGE=$(jq -r '.items[0].title' "$TMP/body")
fi
if [ -z "$STAGE_ID" ] || [ "$STAGE_ID" = null ]; then bad "no stage '$STAGE'"; exit 1; fi
ok "stage '$STAGE' = $STAGE_ID"
IDS_JSON=$(printf '%s\n' "${IDS[@]}" | jq -R . | jq -sc .)

hdr "Error paths"
code=$(post /api/v1/animation/dialog-stream/start "{\"creature_ids\":$IDS_JSON}")
[ "$code" = 400 ] && ok "start without stage_id → 400" || bad "start without stage_id → HTTP $code: $(cat "$TMP/body")"
code=$(post /api/v1/animation/dialog-stream/start "{\"creature_ids\":[\"${IDS[0]}\",\"${IDS[0]}\"],\"stage_id\":\"$STAGE_ID\"}")
[ "$code" = 400 ] && ok "duplicate creature → 400" || bad "duplicate creature → HTTP $code"
code=$(post /api/v1/animation/dialog-stream/start "{\"creature_ids\":$IDS_JSON,\"stage_id\":\"00000000-0000-4000-8000-000000000000\"}")
[ "$code" = 404 ] || [ "$code" = 400 ] && ok "unknown stage → $code" || bad "unknown stage → HTTP $code: $(cat "$TMP/body")"
code=$(post /api/v1/animation/dialog-stream/turn "{\"session_id\":\"00000000-0000-4000-8000-000000000000\",\"creature_id\":\"${IDS[0]}\",\"text\":\"hi\"}")
[ "$code" = 404 ] && ok "turn on unknown session → 404" || bad "turn on unknown session → HTTP $code"

hdr "Start a session"
code=$(post /api/v1/animation/dialog-stream/start "{\"creature_ids\":$IDS_JSON,\"stage_id\":\"$STAGE_ID\",\"resume_playlist\":true}")
[ "$code" = 200 ] || { bad "start → HTTP $code: $(cat "$TMP/body")"; exit 1; }
SESSION=$(jq -r .session_id "$TMP/body")
ok "session $SESSION ($(jq -c .creature_ids "$TMP/body") on $(jq -r .stage_id "$TMP/body"))"

code=$(post /api/v1/animation/ad-hoc-stream/text "{\"session_id\":\"$SESSION\",\"text\":\"who is this\"}")
[ "$code" = 409 ] && ok "/ad-hoc-stream/text on a dialog session → 409" || bad "/ad-hoc-stream/text on a dialog session → HTTP $code"
code=$(post /api/v1/animation/dialog-stream/turn "{\"session_id\":\"$SESSION\",\"creature_id\":\"00000000-0000-4000-8000-000000000000\",\"text\":\"hi\"}")
[ "$code" = 400 ] && ok "turn from a non-participant → 400" || bad "turn from a non-participant → HTTP $code"

hdr "Turns (watch the birds: listeners idle with beaks shut and look at the speaker)"
LINES=(
  "April, I think the servos you ordered are here!"
  "Or it's more heat sinks. It's always heat sinks."
  "You don't know that."
  "I know it in my bones. Little tiny aluminium bones."
)
t0=$(date +%s)
for i in "${!LINES[@]}"; do
  who=$(( i % ${#IDS[@]} ))
  code=$(post /api/v1/animation/dialog-stream/turn "{\"session_id\":\"$SESSION\",\"creature_id\":\"${IDS[$who]}\",\"text\":$(jq -Rn --arg t "${LINES[$i]}" '$t')}")
  if [ "$code" = 200 ]; then
    ok "turn $((i+1)) (${NAMES[$who]}) accepted, turns_received=$(jq -r .turns_received "$TMP/body") at +$(( $(date +%s) - t0 ))s"
  else
    bad "turn $((i+1)) → HTTP $code: $(cat "$TMP/body")"
  fi
  sleep 2
done

hdr "Finish"
code=$(post /api/v1/animation/dialog-stream/finish "{\"session_id\":\"$SESSION\"}")
[ "$code" = 200 ] || { bad "finish → HTTP $code: $(cat "$TMP/body")"; exit 1; }
cp "$TMP/body" "$TMP/finish"
ANIM=$(jq -r .animation_id "$TMP/finish")
STATUS=$(jq -r .exchange_status "$TMP/finish")
ok "finish: exchange_status=$STATUS parts=$(jq -r .parts_rendered "$TMP/finish")/$(jq -r .parts_total "$TMP/finish") animation_id=$ANIM last_turn=$(jq -r .last_turn_animation_id "$TMP/finish")"
[ "$STATUS" = ready ] || bad "exchange_status is '$STATUS', expected ready"
[ -n "$ANIM" ] && [ "$ANIM" != null ] || bad "no stitched animation_id"

hdr "The stitched animation"
code=$(get "/api/v1/animation/ad-hoc/$ANIM")
if [ "$code" = 200 ]; then
  tracks=$(jq '.tracks | length' "$TMP/body")
  frames=$(jq -r '.metadata.number_of_frames' "$TMP/body")
  [ "$tracks" = "${#IDS[@]}" ] && ok "animation has $tracks tracks, $frames frames, stage $(jq -r .metadata.source_stage_id "$TMP/body")" || bad "animation has $tracks tracks, expected ${#IDS[@]}"
  jq -r '.tracks[] | "     track \(.creature_id): \(.frames | length) frames"' "$TMP/body"
  jq -r '.metadata.source_render_choices[]? | "     \(.creature_id): speech \(.speech_loop_animation_id) idle \(.idle_animation_id) phase \(.idle_start_offset)"' "$TMP/body"
else
  bad "GET /animation/ad-hoc/$ANIM → HTTP $code"
fi

hdr "The exchange record"
code=$(get "/api/v1/animation/ad-hoc-stream/exchange/$SESSION")
if [ "$code" = 200 ]; then
  ok "participants: $(jq -c '[.participants[].creature_name]' "$TMP/body") stage_id=$(jq -r .stage_id "$TMP/body") duration_ms=$(jq -r .duration_ms "$TMP/body")"
  jq -r '.parts[] | "     \(.index). \(.creature_name): \(.text) (\(.duration_ms) ms)"' "$TMP/body"
  echo "     title: $(jq -r .title "$TMP/body")"
else
  bad "GET exchange → HTTP $code"
fi
code=$(curl -sS -m 120 "${AUTH[@]}" -o "$TMP/exchange.mp3" -w "%{http_code}" "$BASE/api/v1/animation/ad-hoc-stream/exchange/$SESSION/audio.mp3")
if [ "$code" = 200 ]; then
  ok "audio.mp3 $(wc -c <"$TMP/exchange.mp3" | tr -d ' ') bytes"
  if command -v ffprobe >/dev/null; then
    ffprobe -v error -show_entries format_tags=title,artist,TRACK_LIST -of default=noprint_wrappers=1 "$TMP/exchange.mp3" | sed 's/^/     /'
  fi
else
  bad "audio.mp3 → HTTP $code"
fi

echo
echo "trace: $TRACE_ID"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
