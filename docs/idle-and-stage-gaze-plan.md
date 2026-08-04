# Natural idle motion + stage-aware head-look

Tracking issue: [#119](https://github.com/opsnlops/creature-server/issues/119)

## Context

Two problems with how dialog scenes currently render:

1. **Listening creatures are frozen.** `SpeechTrackBuilder.cpp:181` holds a single frame —
   `baseFrames[0]`, the first frame of the randomly-chosen speech loop — for the entire
   time a creature isn't talking. In a four-bird scene with one speaker, three birds are
   statues for the whole turn. The runtime idle scheduler can't help: a dialog scene is
   one `Animation` in one `PlaybackSession` covering all N creatures, so
   `hasActiveSessionForCreature` is true for every listener and `startIdleIfNeeded` never
   fires mid-scene.

2. **Nobody looks at anybody.** The birds have no notion of where they are relative to
   each other, or which servo turns their head. A show reads far better when characters
   orient toward whoever is speaking.

Intended outcome: during a rendered dialog, silent creatures cycle a real idle animation
instead of freezing, and every creature's neck points toward the current speaker — with
per-creature timing variation so four heads never move in unison.

Because the physical arrangement changes between the mainstage and the road, stage
placement has to be an editable asset with a cheap re-render path: move a bird, or swap
mainstage for travel, and rebuild the affected animations **without regenerating any
audio**. That constraint shapes Parts 5 and 6.

---

## The worked four-character model

This section is the design rationale. The geometry produced a result that shapes the
whole feature, so it's recorded here rather than re-derived later.

### Coordinate system — listener-centric

Keep AVAudioEngine's axis convention from the console
(`creature-console/Sources/Creature Console/SpatialAudio/SpatialStageTypes.swift`):
**metres, Y-up, right-handed, −Z away from the listener.** But move the origin.

The console currently stores an absolute frame with the listener parked at the edge
(`listener = (0, 1.6, 2)`, creatures at `z ≈ −2.7`, all of them crammed into negative Z).
**Put the listener at the origin instead**, so the stage spans symmetrically around it:

```
origin (0,0,0)  = the listener's head, facing −Z
x ∈ [−5, +5]    = left / right of the listener
y ∈ [−5, +5]    = height relative to the listener's ears
                  (birds' heads at y = −0.2 → 20 cm below your eye line)
z ∈ [−5, +5]    = negative in front of the listener, positive behind
```

Why this is better than the console's current frame:
- **Symmetric, readable numbers.** A bird at `x = −2.4` is 2.4 m to your left. No mental
  subtraction of a listener offset to know where anything is.
- **Behind the listener is expressible.** Positive Z is free. That matters for a road gig
  where the birds flank the room, or for surrounding the listener, and it costs nothing.
- **It's what the geometry math wants anyway.** Every calculation here is relative to
  the listener or between two creatures; an absolute frame with an arbitrary origin only
  adds an offset everyone has to subtract back out.

The listener becomes implicit — no `listener_x/y/z` fields, no `stage_width`/`stage_depth`
(the ±5 bounds are the stage). An optional `listener` override can be added later if you
ever want to audition the mix from off-centre.

**Console migration:** this is a translation, not just an added field. The console's
stored layout subtracts its listener position from every placement
(`x' = x − listenerX`, `y' = y − listenerY`, `z' = z − listenerZ`) and folds `listenerYaw`
into a rotation of the whole frame. That's one `migrateToCurrentVersion` bump — the
mechanism is already there at `SpatialStageTypes.swift:90`.

New field: **`yaw` per placement.** Convention to pin in code and tests:

```
bearing(from P to T) = atan2(Tx - Px, Tz - Pz)     // degrees
  0°   = facing the listener's side of the room (+Z)
 +90°  = facing +X (the listener's right)
 180°  = facing directly away from the listener

yaw uses the same convention.  yaw = 0 → bird faces +Z.
relative_angle = normalize180(bearing_to_target - yaw)
```

### Layout A — four birds in a row (the naive layout)

```
                     −Z  (away from listener)

   Beaky           Mango        C            D
 (−2.4,−3.0)   (−0.8,−3.4) (+0.8,−3.4)  (+2.4,−3.0)
   yaw +12        yaw +4      yaw −4       yaw −12
      \             |           |            /
       \            |           |           /
        ────────────────────────────────────
                       ↓
                 ★ LISTENER (0,0,0)

                     +Z  (behind the listener)
```

Relative angles each bird would need to turn to face each target:

| looker | → Beaky | → Mango | → C | → D | → listener |
|---|---|---|---|---|---|
| **Beaky** (yaw +12) | — | **+92.0°** | **+85.1°** | **+78.0°** | +26.7° |
| **Mango** (yaw +4) | **−80.0°** | — | **+86.0°** | **+78.9°** | +9.2° |
| **C** (yaw −4) | **−78.9°** | **−86.0°** | — | **+80.0°** | −9.2° |
| **D** (yaw −12) | **−78.0°** | **−85.1°** | **−92.0°** | — | −26.7° |

Every creature-to-creature angle is 78–92°. **That is the whole problem: birds standing
in a row must turn nearly 90° to look at each other**, which no animatronic neck can do.
With a ±55° neck, all three of Beaky's targets clamp to the same rail — she looks
identical whether she's addressing Mango, C, or D.

### Layout B — the conversation V (recommended)

Angle the resting facings inward so neck range is spent on *differentiating* targets
rather than on getting to 90° in the first place. This is what a director does anyway.

Perches at genuinely different heights — which is both realistic and, as the tilt section
below shows, the thing that rescues target identity:

```
                     −Z  (away from listener)

    Beaky           Mango        C            D
  x −2.4          x −0.8      x +0.8       x +2.4
  y +0.10         y −0.30     y −0.50      y +0.25     ← perch height
  z −3.0          z −3.4      z −3.4       z −3.0
    yaw +35        yaw +15     yaw −15      yaw −35
       ↘             ↘           ↙            ↙
        ╲            ╲          ╱            ╱
         ────────────────────────────────────
                        ↓
                  ★ LISTENER (0,0,0)

        side view:   D ▔▔        ▁▁▁ Beaky
                          Mango ▁▁
                     C ▁▁▁▁
```

A nice property falls out of putting the listener at the origin: with the birds spread
across X, **angling them inward toward each other and angling them toward the listener
are the same move.** The conversation V is free — it costs nothing in how much the birds
face the house. Their resting yaw already points within a few degrees of the listener.

Angles now, after a soft-clip through the neck range
(`out = range * tanh(relative / range)` — near-linear in the middle, never rails hard):

| looker | neck range | → Beaky | → Mango | → C | → D | → listener |
|---|---|---|---|---|---|---|
| **Beaky** | ±55° | — | +46.7° | +44.6° | +41.9° | +3.7° |
| **Mango** | ±60° (inverted) | −54.5° | — | +49.3° | +48.7° | −1.8° |
| **C** | ±50° | −43.8° | −45.3° | — | +47.4° | +1.8° |
| **D** | ±45° | −37.8° | −39.7° | −41.0° | — | −3.7° |

Converted to servo bytes via the per-creature calibration
(`byte = 255 * (angle − degrees_at_0) / (degrees_at_255 − degrees_at_0)`, clamped):

| looker | → Beaky | → Mango | → C | → D | → listener |
|---|---|---|---|---|---|
| **Beaky** (±55, normal) | — | **236** | **231** | **225** | **137** |
| **Mango** (±60, inverted) | **244** | — | **23** | **25** | **132** |
| **C** (±50, normal) | **16** | **13** | — | **249** | **133** |
| **D** (±45, normal) | **21** | **16** | **12** | — | **117** |

### Tilt — where target identity actually lives

Elevation angle is `atan2(dy, horizontal_distance)`. Because the perches are at different
heights, these angles are large and, crucially, **nothing clamps** — a ±30° tilt range
covers almost the whole set:

| looker | tilt range | → Beaky | → Mango | → C | → D | → listener |
|---|---|---|---|---|---|---|
| **Beaky** | ±30° | — | −13.6° | −10.5° | +1.8° | −1.5° |
| **Mango** | ±30° | +13.6° | — | −7.1° | +9.7° | +4.9° |
| **C** | ±30° | +10.5° | +7.1° | — | **+24.5°** | +8.1° |
| **D** | ±30° | −1.8° | −9.7° | **−24.5°** | — | −3.7° |

As bytes (±30°, so 4.25 bytes/degree, centre 128):

| looker | → Beaky | → Mango | → C | → D | → listener |
|---|---|---|---|---|---|
| **Beaky** | — | **70** | **83** | **136** | **122** |
| **Mango** | **186** | — | **98** | **169** | **149** |
| **C** | **173** | **158** | — | **232** | **162** |
| **D** | **120** | **87** | **24** | — | **112** |

### The finding that shapes the design

Read the two byte tables together:

| | pan spread across targets | tilt spread across targets |
|---|---|---|
| Beaky | 225 → 236 (**11 bytes**) | 70 → 136 (**66 bytes**) |
| Mango | 23 → 244 *(sign flip)* | 98 → 186 (**88 bytes**) |
| C | 13 → 249 *(sign flip)* | 158 → 232 (**74 bytes**) |
| D | 12 → 21 (**9 bytes**) | 24 → 120 (**96 bytes**) |

**Pan says which side. Tilt says which bird.**

Pan is nearly degenerate for target identity — every bird-to-bird angle exceeds the neck
range, so C aiming at Beaky (16) and C aiming at Mango (13) differ by 3 bytes, under the
servo's mechanical slop. But pan *is* the big unmistakable gesture: C swinging from 133
(on the listener) to 13 (on Mango) is ~120 bytes of travel that reads from the back row.

Tilt carries the information pan can't. C looking at Beaky vs Mango is 173 vs 158 —
15 bytes, clearly visible — and C looking up at D is 232, dramatically different. Because
the birds sit at different heights, elevation is *not* saturated, so the neck genuinely
resolves which bird is being addressed.

Consequences for the build:

- **Tilt is not a nice-to-have; it's what makes the feature legible.** Ship both axes.
- **Perch heights in the stage document matter more than X/Z.** A stage where every bird
  is at the same `y` throws away the only channel that disambiguates targets. Worth
  saying so in the console UI.
- **Still don't build addressee tracking.** Listeners → current speaker; speaker →
  previous speaker. The geometry already does the expressive work.
- **Timing is still where the naturalism comes from** — per-creature reaction delay,
  travel duration, easing, overshoot, micro-drift.

The feature degrades gracefully throughout: sloppy coordinates still produce correct side
selection, a bird with no `head_tilt` still pans, and a bird with neither is untouched.

### The timing model (where the value is)

Turn boundary at frame 201 (t = 4.02 s, Mango starts speaking). Every listener retargets,
but each on its own schedule, drawn from a per-creature seeded RNG:

| creature | reaction delay | pan travel | tilt travel | overshoot | starts | pan arrives |
|---|---|---|---|---|---|---|
| Beaky | 180 ms (9 f) | 320 ms (16 f) | 260 ms (13 f) | +4.1° | f210 | f226 |
| C | 340 ms (17 f) | 410 ms (20 f) | 350 ms (18 f) | +2.2° | f218 | f238 |
| D | 520 ms (26 f) | 260 ms (13 f) | 300 ms (15 f) | +5.8° | f227 | f240 |
| Mango *(speaker)* | 100 ms (5 f) | 360 ms (18 f) | 290 ms (15 f) | +3.0° | f206 | f224 |

Both axes share one reaction delay — a head starts moving as a head — but get slightly
different travel durations, so the gaze traces a curve rather than a straight diagonal.
Tilt runs a little faster than pan and takes about half the overshoot; a real head settles
its elevation before it finishes swinging.

Heads begin moving 180 / 340 / 520 ms apart and arrive at different times. That stagger
is the entire difference between "a scene" and "four animatronics executing a cue."

Beaky's neck byte across that boundary — smoothstep travel, damped overshoot, then
micro-drift:

```
frame   210  212  214  216  218  220  222  224  226  230  234  238  →
byte    137  141  152  168  187  205  221  232  236  245  239  236  ~236 ±3
        └── hold ──┘└──────── smoothstep travel ────────┘└ settle ┘└ drift ┘
         (on listener)                                    (overshoot)
```

Additional naturalism rules, all cheap:

- **Micro-drift while holding:** ±1.5° (≈ ±3–4 bytes) of slow noise, per-creature phase.
  Nobody is ever perfectly still.
- **Not everyone reacts:** ~15% chance a given listener ignores a turn change and holds
  its previous target. Four birds all snapping on the same cue is the creepy case.
- **Glance-away on long turns:** during a turn longer than ~6 s, one listener may look
  away and back.
- **Speaker gaze:** speaker → previous speaker; the first speaker of a scene →
  the listener. Costs nothing and reads correctly.

### Idle animations with four birds

Each of the four independently picks from its own `idle_animation_ids` — reusing the
shuffle-with-no-immediate-repeat logic already in
`CreatureService::startIdleIfNeeded` (`src/server/ws/service/CreatureService.cpp:971`).

The failure mode to design against: if C and D both draw the same idle animation and both
start at phase 0, they loop in lockstep and it looks broken. **Each creature gets a random
start phase into its idle loop**, so identical animations still read as independent.

```
              t=0        2s         4s         6s         8s
Beaky  preen_slow  (140 f)  ▓▓▓▓▓▓▓▓▓░░░░░░░░░[SPEAKING]░░░░░  phase start 87
Mango  look_around ( 90 f)  ░░░░[SPEAKING]▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓  phase start 12
C      shuffle     (200 f)  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓  phase start 154
D      preen_slow  (140 f)  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓[SPEAKING]░  phase start 33
                            ▓ = idle loop   ░ = speech loop
```

The idle counter **free-runs across the whole scene** rather than restarting at each
silence — restarting causes a visible pop back to the idle loop's frame 0 at every turn
boundary.

### Layer composition

Three layers, applied in order per frame, per creature:

```
  ┌─ Layer 0: body ────────────────────────────────────────────┐
  │   speaking → speech_loop[speakingCounter++ % N]            │
  │   silent   → idle_loop[idleCounter++ % M]                  │
  │   boundary → byte-wise crossfade over ~10 frames (200 ms)  │
  └────────────────────────────────────────────────────────────┘
                            ↓
  ┌─ Layer 1: mouth ───────────────────────────────────────────┐
  │   speaking → frame[mouth_slot] = mouthBytes[f]             │
  │   silent   → frame[mouth_slot] = rest                      │
  │              (an idle animation's own beak motion reads as  │
  │               silent mouthing under someone else's voice)   │
  └────────────────────────────────────────────────────────────┘
                            ↓
  ┌─ Layer 2: head-look ───────────────────────────────────────┐
  │   frame[head_pan.slot]  = gaze pan byte   (overrides L0)   │
  │   frame[head_tilt.slot] = gaze tilt byte  (overrides L0)   │
  │   each axis independent; skipped entirely when the         │
  │   creature lacks that axis or the render has no stage      │
  │   → with neither axis, output is byte-identical to today   │
  └────────────────────────────────────────────────────────────┘
```

Layer 2 overriding layer 0 on the neck slots is deliberate: the idle animation's neck
values are absolute positions, so adding an offset would clip. A blend weight can be
added later if the idle head-bobs turn out to be missed.

---

## Implementation

### Part 1 — idle frames during silence

**`src/server/voice/SpeechTrackBuilder.h`** — extend `SpeechTrackOptions`:

```cpp
// Frames to cycle during silent stretches when dialogIdleMode is on. Empty →
// today's behavior (freeze on baseFrames[0]). Must match baseFrames' width.
std::span<const std::vector<uint8_t>> idleFrames{};
std::size_t idleStartOffset{0};   // random phase, so identical loops desync
std::size_t crossfadeFrames{10};  // byte-wise blend at speech↔idle boundaries
std::size_t mouthRestByte{0};     // forced at mouth_slot on silent frames
```

**`src/server/voice/SpeechTrackBuilder.cpp`** — generalize `resolveSpeechBaseFrames` to
take the animation-id list as a parameter (it currently hardcodes
`creature.speech_loop_animation_ids`) so the same resolve/decode/width-validate path
serves `idle_animation_ids`. Then the silent branch at line 181 becomes an idle-loop
lookup with its own free-running counter, plus the crossfade at run boundaries.

Fallbacks, all silent and non-fatal — this feature must never fail a render:
- empty `idle_animation_ids` → freeze on `baseFrames[0]`
- idle frame width ≠ speech frame width → freeze
- idle animation's `milliseconds_per_frame` ≠ scene's → freeze (same guard as
  `JobWorker.cpp:1675` uses for speech loops)

**`src/server/voice/DialogAnimation.h`** — add `idleFrames` + `idleStartOffset` to
`CreatureTrackInput`; thread through `buildDialogAnimation` into `SpeechTrackOptions`.

**`src/server/jobs/JobWorker.cpp:1639–1711`** — resolve and decode each creature's idle
animation next to where the speech loop is already resolved, and draw the random phase.

Also clean up the stale "neutral pose" comments left over from the deleted
`buildNeutralFrame` (`DialogAnimation.h:38,51,61,81`, `JobWorker.h:103`).

### Part 2 — `head_pan` + `head_tilt` on the creature

**`src/model/Creature.h`** — two new optional fields sharing one calibration shape:

```cpp
struct HeadAxis {
    uint8_t slot;             // index into the motion array
    float degrees_at_0;       // physical angle at servo byte 0
    float degrees_at_255;     // physical angle at servo byte 255
                              // swap the two to express an inverted motor
};
std::optional<HeadAxis> head_pan;    // left / right,  yaw-like
std::optional<HeadAxis> head_tilt;   // up / down,     elevation
```

Independently optional: a bird with only `head_pan` pans and never tilts, one with
neither is rendered exactly as today. In JSON:

```json
"mouth_slot": 6,
"head_pan":  { "slot": 2, "degrees_at_0": -55, "degrees_at_255":  55 },
"head_tilt": { "slot": 4, "degrees_at_0": -30, "degrees_at_255":  30 }
```

Must be **optional** in `Database::creatureFromJson` (`src/server/creature/helpers.cpp`)
— the controller's JSON file is the source of truth and is only re-read at registration,
so a required field breaks every existing controller at boot.
`speech_loop_animation_ids` / `idle_animation_ids` are the pattern to copy: guarded with
`contains() && !is_null()`, and omitted from the DTO when absent (`Creature.cpp:94–108`).

### Part 3 — the `Stage` asset

Follow `docs/storyboards-server-plan.md` exactly — it is a literal file-by-file recipe
for adding an asset type, and Storyboard is the newest, cleanest example.

Schema keeps every `SpatialStageLayout` field the console owns (so its UserDefaults blob
migrates up to the server rather than becoming a second source of truth), adds `yaw`, and
re-origins on the listener:

```json
{
  "id": "uuid",
  "title": "Mainstage",
  "version": 1,

  "_comment": "Origin (0,0,0) IS the listener, facing -Z. No listener_* fields,
               no stage_width/stage_depth — the ±5 m bounds are the stage.",

  "monitoring_delay_ms": 10,
  "common_playout_delay_ms": 20,
  "background_music_gain": 0.7,
  "reverb_blend": 0.08,

  "placements": [
    { "creature_id": "…", "audio_channel": 3,
      "x": -2.4, "y": -0.2, "z": -3.0,
      "yaw": 35.0,
      "gain": 1.0, "muted": false }
  ],
  "created_at": 0, "updated_at": 0
}
```

Validation caps (`inline constexpr` in `src/model/Stage.h`, per the Storyboard pattern):
positions clamped to ±5 m on every axis, `yaw` normalized to (−180, 180], max 16
placements (the audio-lane ceiling), duplicate `creature_id` rejected.

Files, per the storyboard recipe:
- create `src/model/Stage.{h,cpp}` (struct, validation caps, Swagger-only DTO, `stageToJson`)
- create `src/server/stage/{helpers,get,upsert}.cpp` (`parseStageJson`, `getStage`,
  `listStages`, `upsertStage`, `deleteStage`)
- create `src/server/ws/controller/StageController.h` — the 5 CRUD endpoints under
  `/api/v1/stage`, using `runEndpoint` + the `HttpResponseHelpers<Self>` mixin
- modify `src/model/CacheInvalidation.{h,cpp}` (`StageList` / `"stage-list"`),
  `src/server/config.h` (`STAGES_COLLECTION`), `src/server/database.h`,
  `src/server/storage/Storage.{h,cpp}` (`publishStage` / `deleteStage` via
  `runPublisher` — never pair the DB write and cache invalidation by hand, per issue #11),
  `src/server/ws/App.cpp` (two registration lines), `CMakeLists.txt`
- **`CMakeLists.txt`: a new `src/server/stage/*` directory busts the Phase 1 Docker
  dep cache once.** Expect one ~15 min build.

Keep the fields the server doesn't use (gain, muted, reverb_blend, delays) round-tripping
untouched — they're the console's, and preserving them is what makes this one document
instead of two.

**Cross-repo:** the console side is a separate task in `creature-console` — bump
`SpatialStageLayout.currentVersion`, translate stored placements into listener-centric
coordinates in `migrateToCurrentVersion` (`SpatialStageTypes.swift:90`), add the `yaw`
control to `SpatialStageView`, and switch `SpatialStageLayoutStore` from UserDefaults to
the new `/api/v1/stage` endpoints. Open a matching issue there.

### Part 4 — the gaze layer

New `src/server/voice/GazeTrack.{h,cpp}` — pure, no DB, no network, fully unit-testable:

```cpp
struct GazeGeometry {   // resolved from Stage + Creature head axes
    float x, y, z, yaw;                        // y is load-bearing: it drives tilt
    std::optional<Creature::HeadAxis> pan;
    std::optional<Creature::HeadAxis> tilt;
};

struct GazeTrack {                             // empty vector = axis not driven
    std::vector<uint8_t> panBytes;
    std::vector<uint8_t> tiltBytes;
};

// Speaking spans per creature, derived from the same mouthBytes mask
// buildSpeechTrack already computes.
GazeTrack buildGazeTrack(const GazeGeometry &self,
                         const std::map<std::string, GazeGeometry> &others,
                         const std::vector<SpeakerSpan> &timeline,
                         std::size_t totalFrames, uint32_t msPerFrame,
                         std::mt19937 &rng);
```

Pipeline inside: speaker timeline → per-creature target sequence → per-target bearing and
elevation → soft-clip each through its own axis range → calibrated bytes → the timing
model (shared delay, per-axis smoothstep travel, overshoot, settle, micro-drift,
skip-chance). RNG by reference so tests pin it with a fixed seed, matching how
`resolveSpeechBaseFrames` already threads its generator.

Applied as layer 2 in `buildSpeechTrack`, with each axis bounds-checked against frame
width independently — the canonical Beaky-chest guard (3.14.4). Out of range → drop that
axis's writes, log one warning, set a flag on the result; the other axis still renders.

### Part 5 — binding a stage to a render

- optional `stage_id` on `DialogScript` (so re-renders stay consistent)
- optional `stage_id` on the render request, overriding the script's
- neither set → no gaze layer, byte-identical to today's output

**One animation per (script, stage).** Today the re-render path finds the existing
animation by `source_script_id` and overwrites it in place (3.15.4). Extend that key to
`source_script_id AND source_stage_id`, so rendering a script for the travel stage creates
a second animation rather than clobbering the mainstage one — you keep both renditions
live. Backward compatible: existing animations carry an empty `source_stage_id` and match
a no-stage render exactly as before. Titles get the stage appended when a stage is bound
(`"Scene 3 — Travel"`) so the two are distinguishable in the console.

### Part 6 — re-rendering when the stage changes

The requirement: edit a stage — move a bird, or switch mainstage → travel — and re-render
everything built on it.

**The hard constraint: a stage re-render must never regenerate audio.** eleven_v3 is
nondeterministic and exposes no seed (see the multichar-dialog findings), so calling
ElevenLabs again would produce a *different performance*. Re-rendering for a geometry
change has to be motion-only: same WAV, same timing, same voices, new tracks. That also
makes it free, offline, and instant instead of a paid async round-trip.

That turns out to be entirely possible, because everything the motion builder needs is
already persisted:

```
existing Animation ──► metadata.sound_file ──► the rendered 17-ch WAV
                                                     │
                                    IxmlReader::readIxmlChunk
                                                     │
                                    parseIxmlLipsync ─┴─► per-channel Rhubarb cues
                                                            │
                                       SoundDataProcessor ──┴─► per-frame mouth bytes
```

`src/server/voice/IxmlReader.h` already exposes `parseIxmlLipsync` (issue #53), which
returns `DialogLipsyncTrack{channel, name, cues}` per creature — and channel maps to
creature via `Creature.audio_channel`. So the speaking timeline and mouth bytes come back
out of the WAV losslessly, with no ElevenLabs call and no re-slicing.

**Fallback for old renders** that predate the LIPSYNC block: scrape `frames[f][mouth_slot]`
straight out of the existing animation's tracks — that is exactly what `buildSpeechTrack`
wrote. Caveat worth knowing: for animations rendered *before* Part 1, silent frames carry
whatever mouth byte the speech loop's frame 0 had, so the scraped mask can be wrong if
that byte is non-zero. Prefer iXML; fall back to scraping; log which path was used.

**New provenance on `AnimationMetadata`**, mirroring `source_script_id` /
`source_script_turns` exactly:

```cpp
std::string source_stage_id;                          // soft pointer
int64_t source_stage_updated_at{0};                   // staleness comparison
std::vector<StagePlacement> source_stage_placements;  // CoW snapshot
uint64_t render_seed{0};                              // determinism (see below)
std::vector<CreatureLoopChoice> source_loop_choices;  // per-creature speech + idle ids
```

**`render_seed` is what makes re-rendering safe.** Every random choice in this
feature — which speech loop, which idle animation, the idle start phase, each creature's
reaction delay / travel / overshoot / skip-chance — derives from one seed stamped into
metadata. Re-render with the same seed and a new stage and *only the gaze changes*;
everything else is bit-identical. Without it, moving one bird would reshuffle all four
birds' idle animations, and you could never tell what your edit actually did.
`source_loop_choices` belts-and-braces the loop selection so it survives a creature
gaining a new animation in its config.

**Staleness** is `metadata.source_stage_updated_at < stage.updated_at`. Cheap, and it
lets the console say "5 animations, 3 out of date."

**Endpoints:**

| Method | Path | Does |
|---|---|---|
| GET | `/api/v1/stage/{id}/animations` | Animations rendered against this stage, each with a `stale` flag |
| POST | `/api/v1/stage/{id}/rerender` | Enqueue a motion-only re-render for every animation bound to this stage (optionally `{"stale_only": true}`) |
| POST | `/api/v1/animation/{id}/rerender` | One animation; optional `stage_id` in the body re-targets it — this is how you produce a travel rendition from a mainstage one |

All async through the existing `JobManager` / `JobWorker` pattern
(`docs/architecture/job-system.md`), as a new `handleStageRerenderJob` alongside
`handleDialogJob`. Per animation it: loads the animation → recovers mouth bytes → resolves
base + idle frames from `source_loop_choices` → seeds the RNG from `render_seed` → builds
gaze from the *new* stage → `buildSpeechTrack` per creature → writes back **reusing the
same `animation_id`** (the existing overwrite-in-place path) → `storage::publishAnimation`
fires the cache invalidation.

Deleting a stage leaves its animations intact and re-renderable from their
`source_stage_placements` snapshot — same semantics as deleting a DialogScript.

---

## Implementation status

| Part | State |
|---|---|
| 1 — idle animation frames during silence | **done** |
| 2 — gaze config on the creature | **done** (reworked, see below) |
| 3 — the `Stage` asset | **done** |
| 4 — the gaze layer | **done** |
| 5 — binding a stage to a render | **done** |
| 6 — provenance, staleness, discovery | **done** |
| 6 — motion-only re-render execution | **not started** |

416 tests pass. `VERSION.txt` bumped to 3.36.0.

### Changed from the original plan

**The creature config was wrong, and the hardware said so.** The plan invented
`head_pan` / `head_tilt` blocks carrying their own slot numbers. Reading the
controller's `Parrot.cpp` / `Crow.cpp` and `DifferentialHead` showed two errors:

1. The inputs are *already* named — `neck_rotate`, `head_height`, `head_tilt`,
   `body_lean`, `beak`, `chest`, `stand_rotate` — and `Creature::inputs`
   already maps name → slot. A new field with its own slot number would be a
   second source of truth for something that already exists, and the layouts
   differ per creature (Beaky/Crow put `neck_rotate` at 2; Mango/Kenny at 5),
   so a hand-copied number would be a standing invitation to get it wrong.
2. Elevation is **`head_height`**, not `head_tilt`. The neck is differential —
   `DifferentialHead` turns `head_height` + `head_tilt` into
   `neck_left`/`neck_right`. Both servos together raise the head; apart, they
   cock it sideways. `head_tilt` is the one axis that doesn't aim at anything.

So the config names the degree of freedom and the slot is resolved per creature:

```json
"gaze": {
  "pan":       { "input": "neck_rotate", "degrees_at_min": -55, "degrees_at_max": 55 },
  "elevation": { "input": "head_height", "degrees_at_min": -25, "degrees_at_max": 25 },
  "cock":      { "input": "head_tilt",   "degrees_at_min": -20, "degrees_at_max": 20,
                 "listening_amount": 0.4 }
}
```

The only genuinely new information is the degrees — servos are described by
pulse width, and those vary hugely between birds (Beaky's `neck_rotate` is
1400–1900 µs, Kenny's 650–2500 µs), so the angular sweep has to be measured.

**A third axis was added: the head cock.** The dog thing — a listening creature
drops one ear toward its shoulder. It aims at nothing, but it's a strong
"this one is paying attention" signal for almost no cost. Two properties make
it read as curiosity rather than as a twitch: it **persists** (a dog holds the
cock through the conversation instead of re-picking every sentence) and it has
a **favoured side** per creature. It also gets no micro-drift, unlike pan and
elevation — a wandering head tilt reads as a fault, not as life.

**`mouth_slot` turned out to be the same class of bug** — see issue #120. The
invariant is that it points at the beak, and no config satisfies it. The
general principle now has support in code: `inputSlotByName`,
`resolvedMouthSlot` (new optional `mouth_input`), and `mouthSlotMatchesBeak`,
with `POST /api/v1/creature/validate` failing loudly on a mismatch.

### What remains

The motion-only re-render **execution** — `POST /api/v1/stage/{id}/rerender`,
`POST /api/v1/animation/{id}/rerender`, and the `handleStageRerenderJob` that
recovers mouth bytes via `parseIxmlLipsync` (falling back to scraping
`mouth_slot`) and rebuilds tracks against the new stage. Everything it depends
on is in place: `render_seed`, `source_stage_id` / `source_stage_updated_at`,
the `(script, stage)` reuse key, and `GET /api/v1/stage/{id}/animations` to
find the affected set.

The hard constraint stands: that job must never call ElevenLabs.

### Calibration is still outstanding

Nothing aims until a `gaze` block lands in the controller's configs with
measured degrees, and until real perch heights go into a stage document.
Since elevation is what disambiguates *which* creature is being addressed,
the `y` values matter more than x/z.

---

## Decisions taken

- **Neck sweep ≈ ±45–60° pan** (confirmed). The Layout B tables above are the real
  working numbers, not a guess.
- **Both axes ship** — `head_pan` and `head_tilt`. Tilt turned out to be the axis that
  makes gaze target legible at all, so it's not an add-on.
- **Listener-centric coordinates**, origin at the listener's head, ±5 m bounds.
- **Bake at render time**, not a runtime layer.
- **Gaze targets:** listeners → current speaker, speaker → previous speaker, first
  speaker of a scene → the listener. No addressee field on script turns.

Still open, and safe to settle during implementation:

- **Tilt sweep per bird.** Modeled at ±30°. Only affects whether the extremes (C→D at
  +24.5°) clamp; the calibration is per-creature so it can be tuned bird by bird.
- **Real perch heights.** The `y` values above are invented. Since tilt is where target
  identity lives, measuring these properly is the single highest-value input.

---

## Verification

- **Unit tests** (`tests/server/voice/`), added explicitly to the `creature-server-test`
  target in `CMakeLists.txt`:
  - `GazeTrack_test.cpp` — pin the bearing *and* elevation conventions with the exact
    Layout B numbers from the two tables above (they're a ready-made fixture: four birds,
    16 directed pairs, both axes); pin the calibration mapping including the
    inverted-motor case (Mango: `degrees_at_0 = +60`, `degrees_at_255 = −60`, 0° → byte
    128); pin soft-clip monotonicity and that ±180° never wraps to the wrong side; pin
    that a creature with only one axis configured renders that axis and leaves the other
    untouched.
  - `SpeechTrackBuilder_test.cpp` — extend the existing file: idle frames cycle and
    free-run across silence runs; empty/mismatched idle frames fall back to
    `baseFrames[0]`; crossfade is monotonic; mouth is forced to rest on silent frames.
    The existing body-tail assertion at line 177 must still pass unchanged.
  - `tests/model/Stage_test.cpp` — round-trip including unknown/console-owned fields.
  - `StageRerender_test.cpp` — the two properties that make re-rendering trustworthy:
    (a) re-rendering with the *same* stage and same `render_seed` reproduces a
    byte-identical Animation; (b) re-rendering with a *moved* bird changes only the
    `head_pan` / `head_tilt` slots and leaves every other byte untouched. Plus: mouth
    bytes recovered from an iXML LIPSYNC block match the bytes recovered by scraping
    `mouth_slot`, so the fallback path is proven equivalent.
- **Regression:** render a dialog with no stage bound and no head axes on any creature;
  assert the resulting Animation is byte-identical to the pre-change render.
- **No-audio-regeneration check:** assert the stage re-render job makes zero outbound HTTP
  calls and leaves `metadata.sound_file` and the WAV's mtime unchanged. This is the
  correctness property that protects the performance, not just a speed optimization.
- **End to end:** `./local_build.sh`, then `POST /api/v1/stage` with the four-bird Layout B
  document, `POST /api/v1/animation/dialog` with a 4-turn script, and inspect the rendered
  track bytes at both head slots — pan should match the timing trace above (Beaky
  137 → 236 with the stagger), tilt should match its own table.
- **Stage edit loop:** `PUT` the stage with Beaky moved to the far side, `POST
  /api/v1/stage/{id}/rerender`, confirm the job completes without an ElevenLabs call and
  that Beaky's gaze bytes flipped sign while the audio file is untouched. Then create a
  second "Travel" stage, re-render the same script against it, and confirm you end up with
  two coexisting animations rather than one overwritten one.
- **On the birds:** the real check, and the one that decides the tuning. Watch (a) whether
  the stagger reads as natural or sloppy, (b) whether the clamped near-90° pan reads as
  "looking at each other," and (c) **whether tilt actually disambiguates who's being
  addressed** — that's the load-bearing claim of this design and it can only be confirmed
  by eye. Delay / travel / overshoot ranges and the tilt sweep are the knobs.
- `clang-format -i` on every modified file; build is `-Wshadow -Wall -Wextra -Wpedantic`.
- Bump `VERSION.txt` (3.35.1 → 3.36.0 — new feature, minor bump).
- Open a GitHub issue first and reference it from commits (`Refs #N`).
