# Speech-track builder dedupe — implementation plan (issue #15)

## Context

[Issue #15](https://github.com/opsnlops/creature-server/issues/15) called out duplicated logic between `DialogAnimation.cpp::buildNeutralFrame` and `StreamingAdHocSession.cpp` after the 3.14.4 fix for Beaky's chest-slot crash. The original framing centered on a shared neutral-frame builder.

**That half of the issue is obsolete.** In 3.15.3 we removed `DialogAnimation::buildNeutralFrame` entirely — the dialog path now uses `input.baseFrames.front()` (the speech-loop's first frame) as the authoritative idle pose. There's no neutral-frame code left to dedupe with anything.

What's still duplicated, across **three** paths (the issue only mentioned two):

| Step | `StreamingAdHocSession` | `processAdHocSpeechJob` | `DialogAnimation::buildDialogAnimation` |
|---|---|---|---|
| Pick a random `speech_loop_animation_ids` entry | once at session start (`start()`) | per job | per creature in the loop |
| Load the base animation from the DB | yes | yes | yes (via `JobWorker.cpp` before passing to builder) |
| Find the per-creature track in the base anim | yes | yes | yes |
| Decode base64 base frames → `vector<vector<uint8_t>>` | once at session start | yes | yes (caller via `creatureInputs`) |
| Validate base frame widths consistent | implicit | implicit | yes (explicit width loop) |
| Resolve + bounds-check `mouth_slot` | yes (`creature_.mouth_slot`) | yes (`creature.mouth_slot`) | yes (from `input.creatureJson`) |
| Cycle base frames + insert mouth byte at `mouth_slot` | yes (`+ baseOffset` continuity) | yes (`% size` simple) | yes (`speakingCounter %` with idle separation) |
| Build a `Track { creature_id, animation_id, frames }` | yes | yes | yes |

The Beaky-chest bug (3.14.4) only happened because the dialog path's bounds check was harder than the ad-hoc path's — exactly the duplicated-logic-drifts-apart shape the issue warned about. The risk repeats every time one of these three paths is touched.

## Approach

Two new helpers in a fresh `src/server/voice/SpeechTrackBuilder.{h,cpp}`. Both are pure functions — no extern globals, no DB — so the existing three call sites do their own DB work and pass the resolved inputs in. That keeps the helper trivially testable.

### `resolveSpeechBaseFrames`

Owns "pick a `speech_loop_animation_ids` entry → look up the animation → find the right track → decode base64 → validate widths." Returns the decoded frame vector or a structured error.

```cpp
struct ResolvedSpeechBase {
    std::vector<std::vector<uint8_t>> baseFrames;  // decoded
    std::string baseAnimationId;                   // for logging / metadata
    std::string baseAnimationTitle;                // for error messages
    uint32_t baseMsPerFrame;                       // carried through for callers
};

Result<ResolvedSpeechBase> resolveSpeechBaseFrames(
    const creatures::Creature &creature,           // has speech_loop_animation_ids
    creatures::Database &db,
    std::mt19937 &rng,                             // injected so tests can pin choice
    std::shared_ptr<OperationSpan> parentSpan = nullptr);
```

The RNG comes in by reference so the caller (production or test) owns the seeding. Same pattern the existing code uses inline.

### `buildSpeechTrack`

Owns the per-track frame assembly: cycle base frames + insert mouth byte at `mouth_slot`, with optional dialog-style idle/tail extension. Single function, behavior controlled by an options struct.

```cpp
struct SpeechTrackInput {
    std::span<const std::vector<uint8_t>> baseFrames;  // already decoded + width-validated
    std::span<const uint8_t> mouthBytes;               // per-target-frame mouth byte (0 = rest)
    std::size_t mouthSlot;                             // byte index in each frame; bounds-checked here
    std::size_t totalFrames;
    std::string creatureId;                            // stamped onto the Track + used in log messages
    std::string animationId;                           // stamped onto the Track
};

struct SpeechTrackOptions {
    // Starting offset into baseFrames. StreamingAdHoc uses the prior sentence's
    // end-offset for continuity; ad-hoc speech + dialog start at 0.
    std::size_t startOffset = 0;

    // Dialog-only: when true, frames where the *extended* mouth signal is 0
    // freeze on baseFrames[0] (the speech-loop's idle pose) instead of advancing
    // the body counter. The "extended" mouth signal is the raw mouthBytes mask
    // with each speaking run extended forward by bodyTailFrames so the body
    // doesn't snap to neutral the instant a turn ends.
    bool dialogIdleMode = false;
    std::size_t bodyTailFrames = 0;
};

struct SpeechTrackResult {
    creatures::Track track;
    std::size_t speakingFrameCount;     // for span attributes / debug logs
    bool mouthSlotInRange;              // false → mouth byte was skipped (warned)
};

Result<SpeechTrackResult> buildSpeechTrack(
    const SpeechTrackInput &input,
    const SpeechTrackOptions &options = {},
    std::shared_ptr<OperationSpan> parentSpan = nullptr);
```

**`mouth_slot` bounds check is now in one place** — the Beaky-chest class of bug can't recur. When out of range, the helper warns and skips the mouth write (the ad-hoc behavior), same as the 3.14.4 fix.

### Why options-struct over multiple functions

Single function means the inner loop (cycle base frames + bounds-check mouth_slot + write byte + base64-encode) lives in one place. The dialog-specific knobs (`dialogIdleMode`, `bodyTailFrames`) shape WHICH frame gets emitted, not how the byte gets written. If we split into `buildSimpleSpeechTrack` / `buildDialogSpeechTrack`, the inner loop is duplicated again at a different layer — defeats the point.

### What doesn't fit in the builder

`StreamingAdHocSession` needs the **end offset** for the *next* sentence's continuity. Easy: the result struct carries `endOffset = (startOffset + totalFrames) % baseFrames.size()` so callers that care can read it. AdHoc + dialog ignore it.

The dialog path's `creatureInputs` carries pre-decoded `baseFrames` from earlier in `processDialogJob` (not from `resolveSpeechBaseFrames`). That's fine — `buildSpeechTrack` accepts a `span<const vector<uint8_t>>` so it works either way. The dialog handler can adopt `resolveSpeechBaseFrames` in a follow-up if useful (not required for the dedupe to land).

## Files to create

- `src/server/voice/SpeechTrackBuilder.h` + `SpeechTrackBuilder.cpp` — the two helpers above
- `tests/server/voice/SpeechTrackBuilder_test.cpp` — pinning tests (see below)

## Files to modify

- `src/server/voice/StreamingAdHocSession.cpp` — replace the inline loop at L324-354 with `buildSpeechTrack`; `start()` resolves the base frames once via `resolveSpeechBaseFrames`
- `src/server/jobs/JobWorker.cpp::processAdHocSpeechJob` — replace L796-868 with `resolveSpeechBaseFrames` + `buildSpeechTrack`
- `src/server/voice/DialogAnimation.cpp::buildDialogAnimation` — replace the per-creature inner loop (L147-256) with `buildSpeechTrack(dialogIdleMode=true, bodyTailFrames=kBodyTailFrames)`. The width-consistency check stays — that's per-creature input validation, not per-track frame assembly.
- `CMakeLists.txt` — no changes; both new files are under existing globbed dirs (`src/server/voice/*` + the explicit test target list needs the new test file)
- `VERSION.txt` — bump to **3.17.3** (internal refactor; no wire-format change)

## Tests (`SpeechTrackBuilder_test.cpp`)

Per the Acceptance section of the issue + the third design call (pin both the regression and each call site's specific behavior):

1. **Beaky-chest regression** — synthetic creature with `mouth_slot=7`, base frames of width 6. The helper returns `mouthSlotInRange == false`, doesn't crash, emits the cycled body frames unchanged. Would have caught 3.14.3.
2. **Simple cycling (ad-hoc shape)** — base frames `[A, B, C]`, totalFrames=7, mouthBytes all 0, `dialogIdleMode=false`. Result frames are `[A, B, C, A, B, C, A]` with mouth slot untouched.
3. **Mouth-byte insertion** — base frames `[A]` of width 4, mouthBytes `[5, 0, 9]`, mouth_slot=2. Result frames have byte[2] = 5/0/9 across the three frames respectively.
4. **Offset continuity (streaming shape)** — base frames `[A, B, C]`, startOffset=2, totalFrames=4. Result frames are `[C, A, B, C]`. The result's `endOffset` is `(2 + 4) % 3 = 0`.
5. **Dialog idle mode** — `dialogIdleMode=true`, `bodyTailFrames=0`, baseFrames `[A, B, C]`, mouthBytes `[5, 5, 0, 5, 0]`. Speaking frames advance the body counter (A → B → C → A); silent frames freeze on baseFrames[0] (A). `speakingFrameCount == 3`.
6. **Dialog idle mode with tail** — same as 5 but `bodyTailFrames=1`. The silent run after frame index 2 gets extended forward by 1, so frame 3 is still "speaking" for body-cycle purposes (continues from where index 1 left off).
7. **Empty baseFrames** — returns `InvalidData` with a helpful message.
8. **mouthBytes shorter than totalFrames** — the remaining frames just don't write a mouth byte (treat as 0). No crash.

Existing test `tests/server/voice/DialogAnimation_buildNeutralFrame_test.cpp` is already gone (3.15.3 cleanup). No new pin needed there.

## Verification

1. `ninja creature-server creature-server-test` clean under `-Wshadow -Wall -Wextra -Wpedantic`
2. `./creature-server-test` — full suite passing (158 + 8 new = 166 expected)
3. `clang-format -i` on every touched file
4. **Audit grep**: `grep -rn 'mouth_slot\s*>=' src/server/voice/ src/server/jobs/` → only in `SpeechTrackBuilder.cpp` (the one canonical bounds check) and the JSON-validation layer in `JobWorker.cpp:1187` (which validates the JSON field exists, not the slot-vs-width check)
5. Live smoke on deployed 3.17.3:
   - Ad-hoc speech still renders + plays correctly
   - Streaming session multi-sentence still has continuous body motion across sentences (no snap at sentence boundary)
   - Multichar dialog still has correct idle poses during silent turns
6. Bump VERSION.txt, build amd64 + arm64 debs, deploy.

## Risks

- **Offset continuity for streaming**. The endOffset on `SpeechTrackResult` must equal what the old inline math computed (`(baseOffset + targetFrames) % decodedBaseFrames_.size()`). Test 4 pins this. Production verification: streaming session of 3+ sentences — listen for a body-motion glitch at the boundary.
- **Dialog idle-mode subtle behavior**. The "extend speaking by `kBodyTailFrames` forward" rule is what makes the dialog body not snap to neutral on a turn boundary. Test 6 pins it; if the visual feel changes in prod, that's the regression to look for.
- **JSON vs typed `mouth_slot` lookup**. `StreamingAdHocSession` reads `creature_.mouth_slot` (typed Creature field); `DialogAnimation` reads `input.creatureJson["mouth_slot"]` (raw JSON). The helper takes `mouthSlot` as an already-resolved `size_t`, so the caller does the lookup. Both paths keep their existing resolution; the dedupe is the *use* of mouth_slot, not the lookup.
- **`SoundDataProcessor::processSoundData`** is upstream of `buildSpeechTrack` (produces `mouthBytes`). Not touched by this refactor; same input → same output → same bytes through the new helper.
