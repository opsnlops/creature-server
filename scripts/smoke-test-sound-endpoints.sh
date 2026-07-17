#!/usr/bin/env bash
# Smoke test for the sound + dialog-preview rendition/metadata endpoints (#56 metadata split + #57 renditions
# + review fixes). Read-only (all GETs) — safe against a live server.
#
# Usage:  BASE=https://server.dev.chirpchirp.dev ./scripts/smoke-test-3.31.0.sh
#         (optionally TOKEN=... for an Authorization: Bearer header)
set -uo pipefail

BASE="${BASE:-http://localhost:8000}"
AUTH=()
[ -n "${TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer ${TOKEN}")

pass=0 fail=0
ok()   { echo "  ✅ $1"; pass=$((pass+1)); }
bad()  { echo "  ❌ $1"; fail=$((fail+1)); }
hdr()  { echo; echo "== $1 =="; }

# headers + status + body for a GET
req() { curl -sS -m 30 "${AUTH[@]}" -D "$2" -o "$3" -w "%{http_code}" "$1"; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

hdr "GET /api/v1/sound  (light list)"
code=$(req "$BASE/api/v1/sound" "$TMP/lh" "$TMP/lb")
[ "$code" = 200 ] && ok "list 200" || bad "list HTTP $code"
# The list must NOT carry the heavy per-track ARRAYS (null field keys are fine;
# populated "mouth_cues":[ / "word_timings":[ are not).
if grep -q '"mouth_cues":\[' "$TMP/lb" || grep -q '"word_timings":\[' "$TMP/lb"; then
  bad "list carries populated mouth_cues/word_timings arrays (should be detail-only)"
else
  ok "list is light (mouth_cues/word_timings null, not populated)"
fi

# Pick a .wav to test with — prefer a dialog render (has embedded provenance).
WAV=$(grep -oE '"file_name":"[^"]+\.wav"' "$TMP/lb" | sed -E 's/.*:"(.*)"/\1/' | head -1)
if [ -z "$WAV" ]; then echo "No .wav in the sound list — cannot test renditions."; exit 1; fi
STEM="${WAV%.wav}"
echo "  using source: $WAV  (stem: $STEM)"

hdr "GET /api/v1/sound/{filename}/metadata  (#56 detail)"
code=$(req "$BASE/api/v1/sound/$WAV/metadata" "$TMP/mh" "$TMP/mb")
[ "$code" = 200 ] && ok "metadata 200" || bad "metadata HTTP $code"
grep -q '"script_turns"' "$TMP/mb" && ok "has script_turns" || echo "  (no script_turns — plain sound?)"
grep -q '"mouth_cues"'  "$TMP/mb" && ok "has mouth_cues (dialog render)" || echo "  (no mouth_cues — not a dialog render, ok)"
grep -q '"word_timings"' "$TMP/mb" && ok "has word_timings (re-rendered dialog)" || echo "  (no word_timings — pre-Part-2 render, degrades gracefully)"

hdr "GET /api/v1/sound/mp3/{stem}.mp3  (#57)"
code=$(req "$BASE/api/v1/sound/mp3/$STEM.mp3" "$TMP/ph" "$TMP/pb")
[ "$code" = 200 ] && ok "mp3 200" || bad "mp3 HTTP $code"
grep -qi '^content-type: audio/mpeg' "$TMP/ph" && ok "Content-Type audio/mpeg" || bad "wrong Content-Type"
grep -qi "content-disposition:.*$STEM.mp3" "$TMP/ph" && ok "Content-Disposition {stem}.mp3" || bad "wrong Content-Disposition"
grep -qiE '^cache-control:.*(immutable|no-store)' "$TMP/ph" && ok "Cache-Control present ($(grep -i '^cache-control' "$TMP/ph" | tr -d '\r'))" || bad "no Cache-Control"
# valid MP3? ID3 tag or MPEG frame sync
if head -c3 "$TMP/pb" | grep -q 'ID3' || command -v file >/dev/null && file "$TMP/pb" | grep -qi 'mpeg\|mp3\|audio'; then
  ok "body looks like a real MP3 ($(wc -c < "$TMP/pb") bytes)"
else bad "body doesn't look like an MP3"; fi

hdr "GET /api/v1/sound/shareable/{stem}.ogg  (#57 reshaped)"
code=$(req "$BASE/api/v1/sound/shareable/$STEM.ogg" "$TMP/oh" "$TMP/ob")
[ "$code" = 200 ] && ok "ogg 200" || bad "ogg HTTP $code"
grep -qi '^content-type: audio/ogg' "$TMP/oh" && ok "Content-Type audio/ogg" || bad "wrong Content-Type"
head -c4 "$TMP/ob" | grep -q 'OggS' && ok "body is a real Ogg ($(wc -c < "$TMP/ob") bytes)" || bad "body isn't Ogg"

hdr "Negative cases"
code=$(req "$BASE/api/v1/sound/mp3/$STEM.wav" "$TMP/nh" "$TMP/nb")
[ "$code" = 422 ] && ok "mp3 with .wav ext → 422" || bad "mp3 .wav ext → HTTP $code (want 422)"
code=$(req "$BASE/api/v1/sound/mp3/this-does-not-exist.mp3" "$TMP/nh2" "$TMP/nb2")
[ "$code" = 404 ] && ok "missing source → 404" || bad "missing source → HTTP $code (want 404)"

echo
echo "==================  $pass passed, $fail failed  =================="
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
