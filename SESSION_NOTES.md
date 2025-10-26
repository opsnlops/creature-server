# Session Notes - Animation Interrupt System

## Testing & Bug Fixes Session

**Date:** 2025-10-18 (Initial Testing) + Follow-up Session
**Status:** ✅ Fully Tested & Working
**Context Continuation:** This session continued from a previous conversation after running out of context

### Summary

The cooperative animation scheduler with interrupt capabilities was thoroughly tested and debugged across two sessions. We discovered and fixed **10 critical bugs** including race conditions, cache safety issues, and shutdown crashes. The system now successfully handles the primary use case: interrupting a looping playlist with button-triggered animations during Zoom meetings, with automatic playlist resumption.

### Bugs Found and Fixed

During testing with the cooperative scheduler, we discovered and fixed 10 critical bugs:

#### Bug #1: Playlist Loop - Infinite Animation Scheduling
**Problem:** Playlist was scheduling new animations every frame instead of waiting for the current animation to complete.

**Root Cause:** `scheduleAnimation()` was returning the **starting frame** instead of the **ending frame** for cooperative scheduler animations.

```cpp
// BUG: src/server/animation/player.cpp:65
return Result<framenum_t>{startingFrame};  // Wrong!
```

**Fix:** Calculate the correct last frame based on animation duration:
```cpp
// Fixed in src/server/animation/player.cpp:70-71
framenum_t framesPerAnimFrame = animation.metadata.milliseconds_per_frame / EVENT_LOOP_PERIOD_MS;
framenum_t lastFrame = startingFrame + framesPerAnimFrame * (animation.metadata.number_of_frames - 1);
```

**Impact:** Playlist now correctly waits for animations to complete before scheduling the next one.

---

#### Bug #2: Concurrent Animation Playback
**Problem:** Regular `/api/v1/animation/play` endpoint allowed animations to run concurrently, causing DMX frame conflicts and servo overlap.

**Root Cause:** No cancellation of existing playback when starting new animations with cooperative scheduler.

**Fix:** Added cancellation and playlist stopping logic to `Database::playStoredAnimation()`:
```cpp
// src/server/animation/play.cpp:71-102
if (config->getAnimationSchedulerType() == Configuration::AnimationSchedulerType::Cooperative) {
    // Cancel existing session
    auto existingSession = sessionManager->getCurrentSession(universe);
    if (existingSession && !existingSession->isCancelled()) {
        existingSession->cancel();
    }

    // Stop running playlist (removes from cache)
    if (runningPlaylists->contains(universe)) {
        runningPlaylists->remove(universe);
        // Notify clients playlist stopped
    }
}
```

**Impact:** Regular play endpoint now properly cancels existing playback and stops playlists before playing new animations.

---

#### Bug #3: Interrupted Playlists Resume Too Early
**Problem:** When using interrupt endpoint, queued `PlaylistEvent` instances would fire and interrupt the interrupt animation!

**Timeline:**
1. Playlist schedules `PlaylistEvent` for frame 26217
2. Call `/api/v1/animation/interrupt` at frame 22212
3. Interrupt animation starts
4. Frame 26217: Old `PlaylistEvent` fires → schedules playlist animation → **interrupts the interrupt!**

**Root Cause:** `PlaylistEvent` didn't check if playlist was in interrupted state before scheduling new animations.

**Fix (Part 1):** Make `PlaylistEvent` check interrupted state:
```cpp
// src/server/eventloop/events/playlist.cpp:58-64
if (sessionManager->hasInterruptedPlaylist(activeUniverse)) {
    info("Playlist on universe {} is interrupted, skipping scheduled event", activeUniverse);
    span->setAttribute("playlist_interrupted", true);
    return Result<framenum_t>{this->frameNumber}; // Exit quietly
}
```

**Fix (Part 2):** Implement automatic playlist resumption when interrupt finishes:
```cpp
// src/server/animation/CooperativeAnimationScheduler.cpp:182-196
session->setOnFinishCallback([universe]() {
    if (sessionManager->hasInterruptedPlaylist(universe)) {
        info("Animation finished on universe {} - resuming interrupted playlist", universe);
        sessionManager->resumePlaylist(universe); // Clear interrupted flag

        // Schedule new PlaylistEvent to continue playlist
        auto nextPlaylistEvent = std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe);
        eventLoop->scheduleEvent(nextPlaylistEvent);
    }
});
```

**Impact:** Interrupt animations now play uninterrupted, and playlists automatically resume when the interrupt completes.

---

#### Bug #4: Code Duplication - Same Bug Appeared Twice

**Problem:** The interrupted playlist check bug existed in two places - one checking the cache for stopped playlists, and another checking if interrupted. This DRY violation meant the bug would need to be fixed twice.

**User Feedback:** "That bug should have only appeared once. Correct is better than quick."

**Root Cause:** No single source of truth for playlist state management.

**Fix:** Implemented unified state management with `PlaylistState` enum:

```cpp
// src/server/animation/SessionManager.h:19-24
enum class PlaylistState {
    None,        // No playlist registered on this universe
    Active,      // Playlist is currently playing normally
    Interrupted, // Playlist is temporarily paused (will resume after interrupt)
    Stopped      // Playlist was explicitly stopped (will not resume)
};

PlaylistState getPlaylistState(universe_t universe) const;
```

Replaced all duplicate checks with single unified check:
```cpp
// src/server/eventloop/events/playlist.cpp:46-75
// src/server/animation/play.cpp:71-82
auto playlistState = sessionManager->getPlaylistState(activeUniverse);
switch (playlistState) {
    case PlaylistState::None:
    case PlaylistState::Stopped:
        // Clean up and exit
        break;
    case PlaylistState::Interrupted:
        // Skip event, will resume later
        break;
    case PlaylistState::Active:
        // Continue normal processing
        break;
}
```

**Impact:** Single source of truth for playlist state eliminates duplicate code and prevents similar bugs from appearing in multiple places.

---

#### Bug #5: Playlist Never Starts After DRY Refactoring

**Problem:** After implementing `getPlaylistState()`, playlists immediately cleaned up on first PlaylistEvent.

**Root Cause:** Playlists weren't being registered with SessionManager when started via `PlaylistService.startPlaylist()`.

**Fix:** Added `SessionManager.startPlaylist()` method and called it from `PlaylistService`:
```cpp
// src/server/ws/service/PlaylistService.cpp:361
creatures::sessionManager->startPlaylist(universe, std::string(playlistId));
```

**Impact:** SessionManager now tracks playlists from the moment they start, making `getPlaylistState()` accurate.

---

#### Bug #6: Playlist Never Resumes After Interrupt

**Problem:** After fixing the registration bug, playlists still didn't resume after interrupts finished.

**Root Cause:** `SessionManager.registerSession()` was completely replacing the universe state, losing the `isPlaylist` and `isInterrupted` flags when the interrupt animation registered.

**Fix:** Modified `registerSession()` to preserve existing playlist state:
```cpp
// src/server/animation/SessionManager.cpp:45-52
if (it != universeStates_.end()) {
    // Preserve existing playlist state (isPlaylist, playlistId, isInterrupted, etc.)
    it->second.currentSession = session;
    // Only update isPlaylist if we're explicitly setting it to true
    if (isPlaylist) {
        it->second.isPlaylist = true;
    }
} else {
    // Create new state
}
```

**Impact:** Playlist state (interrupted, playlistId, etc.) now persists across interrupt animations.

---

#### Bug #7: Animation Gets Cancelled Immediately

**Problem:** When playlist resumed after interrupt, the new animation was cancelled before it could play.

**Root Cause:** `registerSession()` called `cancel()` on the previous session, but the interrupt animation session had already finished and calling `cancel()` again triggered unwanted side effects.

**Fix:** Added check for `isCancelled()` before calling `cancel()`:
```cpp
// src/server/animation/SessionManager.cpp:32-34
if (!it->second.currentSession->isCancelled()) {
    debug("SessionManager: cancelling existing session on universe {} for new session", universe);
    it->second.currentSession->cancel();
}
```

**Impact:** Only actively-running sessions get cancelled, not already-finished ones.

---

#### Bug #8: "Key not found" Exception - Creature Cache Empty

**Problem:** After previous fix, animation scheduled successfully but threw "Key not found" exception during playback. Animation never started playing.

**Root Cause:** User disabled Kenny's sensor health reporting to make logs clearer. Creature cache was empty because creatures only got cached when sending health events. When `PlaybackRunnerEvent` tried to fetch creature data for DMX emission, `creatureCache->get()` threw `std::out_of_range` exception.

**User Insight:** "I bet the creature cache is empty since Kenny is not sending health events."

**Fix:** Modified playback runner to fetch creature from database if not in cache:
```cpp
// src/server/eventloop/events/playback-runner.cpp:167-182
std::shared_ptr<Creature> creature;

// First check if it's in the cache
if (creatureCache->contains(trackState.creatureId)) {
    creature = creatureCache->get(trackState.creatureId);
} else {
    // Not in cache - fetch from database and cache it
    debug("Creature {} not in cache, fetching from database", trackState.creatureId);
    auto creatureResult = db->getCreature(trackState.creatureId, nullptr);
    if (!creatureResult.isSuccess()) {
        std::string errorMsg = fmt::format("Creature {} not found in database during playback", trackState.creatureId);
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }
    creature = std::make_shared<Creature>(creatureResult.getValue().value());
    creatureCache->put(trackState.creatureId, creature);
    debug("Cached creature {} for playback", trackState.creatureId);
}
```

**Impact:** Animations can now play even when creatures aren't actively sending health events. Creature data is fetched on-demand from the database and cached for future frames.

---

#### Bug #9: Race Condition - Concurrent Animations on Interrupt

**Problem:** When interrupting a playlist, both the interrupt animation AND a new playlist animation would play concurrently, causing DMX conflicts. This happened intermittently (data race).

**Root Cause:** The `onFinish` callback fires for BOTH cancelled animations and naturally-finished animations. When a playlist animation was cancelled (interrupted), its `onFinish` callback would:
1. See `hasInterruptedPlaylist() == true` (just set by the interrupt)
2. Immediately resume the playlist
3. Schedule a new PlaylistEvent for the next frame
4. The new playlist animation would start playing WHILE the interrupt animation was still running

**Timeline of the race:**
```
Frame 32886: Interrupt endpoint called
            → Cancels current playlist animation
            → Schedules interrupt animation for frame 32886
Frame 32886: Cancelled playlist animation's onFinish fires
            → Checks hasInterruptedPlaylist() → TRUE
            → Resumes playlist, schedules PlaylistEvent for frame 32889
Frame 32889: PlaylistEvent executes
            → Schedules new playlist animation
Result:      Interrupt animation + playlist animation running concurrently!
```

**User Observation:** "There might be a data race here. This time when I did it I got both animations playing at once the first time."

**Fix:** Modified `onFinish` callback to check if session was cancelled before resuming:
```cpp
// src/server/animation/CooperativeAnimationScheduler.cpp:174-189
session->setOnFinishCallback([universe, weakSession]() {
    debug("PlaybackSession finishing for universe {}", universe);

    // Check if this session was cancelled vs finished naturally
    bool wasCancelled = false;
    if (auto session = weakSession.lock()) {
        wasCancelled = session->isCancelled();
    }

    if (wasCancelled) {
        // This animation was cancelled (interrupted), don't resume playlists
        // The interrupt animation that replaced this one will handle resume when IT finishes
        debug("PlaybackSession was cancelled, skipping resume logic");
        sessionManager->clearCurrentSession(universe);
        return;
    }

    // Animation finished naturally (not cancelled) - proceed with resume logic
    // ...
});
```

**Impact:**
- Cancelled animations skip resume logic entirely
- Only naturally-finished animations trigger playlist resumption
- Interrupt animation plays alone, then triggers resume when IT finishes
- Eliminates race condition and concurrent playback

---

#### Bug #10: Old Queued PlaylistEvents Firing During Interrupts

**Problem:** Even after fixing the race condition, "Kenny Stress Test" revealed both animations still played concurrently occasionally.

**Root Cause:** Different race than Bug #9. Old `PlaylistEvent` instances that were scheduled BEFORE the interrupt still existed in the event queue. When they fired, they would schedule new animations even though the playlist was interrupted or there was already an active session playing.

**Timeline:**
```
Frame 1000:  Playlist schedules PlaylistEvent for frame 5000
Frame 3000:  Interrupt called, schedules interrupt animation
Frame 5000:  OLD PlaylistEvent fires (was scheduled at frame 1000)
            → Checks playlist state → Active (not interrupted, different race)
            → Schedules new playlist animation
Result:      Interrupt animation + playlist animation running concurrently!
```

**Fix:** Added `isPlaying()` check in PlaylistEvent before scheduling:
```cpp
// src/server/eventloop/events/playlist.cpp:79-85
// Additional check: Don't schedule if there's ANY animation currently playing
// This prevents old queued PlaylistEvents from scheduling concurrent animations
if (sessionManager->isPlaying(activeUniverse)) {
    info("Playlist on universe {} has active session, skipping this scheduled event (likely old queued event)",
         activeUniverse);
    span->setAttribute("reason", "active_session_present");
    span->setSuccess();
    return Result<framenum_t>{this->frameNumber};
}
```

**Impact:**
- Old queued PlaylistEvents become no-ops if any animation is currently playing
- Prevents concurrent playback from stale event queue entries
- Works in conjunction with interrupted state check (defense in depth)

**User Feedback:** "I think we've finally got it"

---

#### Integration: Live Streaming and Recording Modes

**Feature:** Extended SessionManager integration to all motion control paths, not just scheduled animations.

**Paths Integrated:**

1. **Live Streaming Mode** (`src/server/ws/messaging/StreamFrameHandler.cpp:98-111`)
   - Used by CreatureDetail UI for real-time creature control
   - Cancels active sessions when first stream frame arrives
   - Stops any active playlists
   - Live streaming takes priority over all scheduled playback

```cpp
// Cancel any active playback on this universe (live streaming takes priority)
auto existingSession = creatures::sessionManager->getCurrentSession(frame.universe);
if (existingSession && !existingSession->isCancelled()) {
    appLogger->info("Cancelling active session on universe {} for live streaming", frame.universe);
    existingSession->cancel();

    // Also stop any playlist state
    auto playlistState = creatures::sessionManager->getPlaylistState(frame.universe);
    if (playlistState == PlaylistState::Active || playlistState == PlaylistState::Interrupted) {
        appLogger->info("Stopping playlist on universe {} for live streaming", frame.universe);
        creatures::sessionManager->stopPlaylist(frame.universe);
    }
}
```

2. **Recording Mode** (RecordTrack)
   - Same integration as live streaming
   - Cancels active playback when recording starts
   - Recording frames take priority like live streaming

**User Feedback:** "Very good, those work!"

---

#### Code Audit: Creature Cache Safety

**Motivation:** After discovering Bug #8 (creature cache empty), audited entire codebase for unsafe `creatureCache->get()` calls that could throw `std::out_of_range`.

**Findings and Fixes:**

1. **LegacyAnimationScheduler.cpp** (lines 141-156)
   - **Problem:** Assumed creatures would be in cache from earlier validation
   - **Fix:** Added `contains()` check before `get()`, fetch from DB if missing
   - **Impact:** Legacy scheduler now works when creatures not sending health events

```cpp
// Get the creature for this track - check cache first, then DB
std::shared_ptr<Creature> creature;
if (creatureCache->contains(creatureId)) {
    creature = creatureCache->get(creatureId);
} else {
    // Not in cache - fetch from database and cache it
    debug("Creature {} not in cache, fetching from database", creatureId);
    auto creatureResult = db->getCreature(creatureId, nullptr);
    if (!creatureResult.isSuccess()) {
        std::string errorMsg = fmt::format("Creature {} not found in database during legacy playback", creatureId);
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }
    creature = std::make_shared<Creature>(creatureResult.getValue().value());
    creatureCache->put(creatureId, creature);
    debug("Cached creature {} for playback", creatureId);
}
```

2. **playlist.cpp** (lines 88-98)
   - **Problem:** `runningPlaylists->get()` could throw if cache entry removed
   - **Fix:** Added `contains()` check before `get()`, graceful cleanup if missing
   - **Impact:** Prevents crashes from race conditions between SessionManager state and cache

```cpp
// Go fetch the active playlist - check cache first
if (!runningPlaylists->contains(activeUniverse)) {
    std::string errorMessage = fmt::format(
        "Playlist state is Active but runningPlaylists cache doesn't have entry for universe {}. Cleaning up.",
        activeUniverse);
    warn(errorMessage);
    sessionManager->stopPlaylist(activeUniverse);
    sendEmptyPlaylistUpdate(activeUniverse);
    span->setAttribute("reason", "cache_missing");
    return Result<framenum_t>{this->frameNumber};
}
```

**Audit Results:**
- All `creatureCache->get()` calls now protected with `contains()` checks
- All `runningPlaylists->get()` calls now protected
- On-demand database fetching when cache misses occur
- Graceful degradation instead of exceptions

---

#### WebSocket Shutdown Improvements

**Problem:** Server crashed during shutdown when WebSocket clients were connected, due to threads still accessing `websocketOutgoingMessages` queue during global destructors.

**Solution:** Made ClientCafe worker threads exit gracefully instead of running forever.

**Changes:**

1. **ClientCafe.h** (lines 58, 63, 68, 86)
   - Removed `[[noreturn]]` attribute from `runPingLoop()` and `runMessageLoop()`
   - Added `requestShutdown()` method
   - Added `std::atomic<bool> shutdownRequested{false}` flag

2. **ClientCafe.cpp**

   **Message Loop** (lines 82-97):
   ```cpp
   void ClientCafe::runMessageLoop() {
       setThreadName("ClientCafe::runMessageLoop");
       std::string message;

       while (!shutdownRequested.load()) {
           // Use wait_dequeue_timed instead of wait_dequeue to allow checking shutdown flag
           if (creatures::websocketOutgoingMessages->wait_dequeue_timed(message, std::chrono::milliseconds(100))) {
               if (!shutdownRequested.load()) {
                   broadcastMessage(message);
               }
           }
       }
       appLogger->info("Message loop exiting gracefully");
   }
   ```

   **Ping Loop** (lines 99-134):
   ```cpp
   void ClientCafe::runPingLoop(...) {
       while (!shutdownRequested.load()) {
           // Sleep in smaller chunks to allow checking shutdown flag
           do {
               std::this_thread::sleep_for(std::chrono::milliseconds(100));
               if (shutdownRequested.load()) {
                   break;
               }
           } while (elapsed < interval);

           // Only send pings if not shutting down
           if (!shutdownRequested.load()) {
               // Send pings...
           }
       }
       appLogger->info("Ping loop exiting gracefully");
   }
   ```

3. **App.h/cpp** - Thread management
   - Stored `pingThread` and `messageLoopThread` as member variables
   - `shutdown()` calls `cafe->requestShutdown()` then joins threads
   - Destructor ensures threads are joined before destruction

**Impact:**
- Ping and message loop threads exit cleanly when shutdown requested
- Threads joined before global destructors run
- Eliminates most WebSocket-related shutdown crashes

**Known Limitation:**
- Server may still crash during `std::exit()` in some cases (harmless, already quitting)
- User decided this is acceptable: "Let's just let it crash. It's harmless."

---

### Files Modified (Bug Fixes)

1. **src/server/animation/player.cpp**
   - Fixed `lastFrame` calculation for cooperative scheduler
   - Added session registration with SessionManager

2. **src/server/animation/play.cpp**
   - Added cancellation of existing sessions
   - Added playlist stopping logic
   - Added WebSocket broadcast for playlist stopped status

3. **src/server/eventloop/events/playlist.cpp**
   - Added interrupted state check before scheduling animations
   - Prevents queued events from interfering with interrupts

4. **src/server/animation/CooperativeAnimationScheduler.cpp**
   - Implemented automatic playlist resumption in onFinish callback
   - Added cancelled session check to prevent race condition
   - Only naturally-finished animations trigger resume (not cancelled ones)
   - Schedules new PlaylistEvent when interrupt completes

5. **src/server/animation/SessionManager.h**
   - Added `PlaylistState` enum (None, Active, Interrupted, Stopped)
   - Added `getPlaylistState()` method as single source of truth
   - Added `stopPlaylist()`, `startPlaylist()`, `clearCurrentSession()` methods
   - Updated documentation for all methods

6. **src/server/animation/SessionManager.cpp**
   - Implemented `getPlaylistState()` with switch-case logic
   - Modified `registerSession()` to preserve existing playlist state
   - Added `isCancelled()` check before calling `cancel()`
   - Implemented `stopPlaylist()`, `startPlaylist()`, `clearCurrentSession()`
   - Updated `resumePlaylist()` to clear interrupted flags

7. **src/server/ws/service/PlaylistService.cpp**
   - Added call to `sessionManager->startPlaylist()` when playlists start
   - Ensures SessionManager tracks playlists from the beginning

8. **src/server/eventloop/events/playback-runner.cpp**
   - Added on-demand creature fetching from database when not in cache
   - Uses `contains()` check before `get()` to avoid exceptions
   - Caches fetched creatures for future frames
   - Enables playback even when creatures aren't sending health events

9. **src/server/ws/messaging/StreamFrameHandler.cpp**
   - Added SessionManager integration for live streaming mode
   - Cancels active sessions when streaming starts
   - Stops active playlists when streaming starts
   - Live streaming takes priority over all scheduled playback

10. **src/server/animation/LegacyAnimationScheduler.cpp**
    - Added on-demand creature fetching from database (same as cooperative scheduler)
    - Uses `contains()` check before `get()` to avoid exceptions
    - Enables legacy scheduler to work when creatures not sending health events

11. **src/server/ws/websocket/ClientCafe.h**
    - Removed `[[noreturn]]` attribute from loop methods
    - Added `requestShutdown()` method
    - Added `shutdownRequested` atomic flag for graceful shutdown

12. **src/server/ws/websocket/ClientCafe.cpp**
    - Modified `runMessageLoop()` to use timed wait and check shutdown flag
    - Modified `runPingLoop()` to sleep in chunks and check shutdown flag
    - Implemented `requestShutdown()` method
    - Added graceful exit logging

13. **src/server/ws/App.h**
    - Added `pingThread` and `messageLoopThread` member variables
    - Added destructor declaration
    - Added `shutdown()` method declaration

14. **src/server/ws/App.cpp**
    - Stored worker threads as member variables
    - Implemented `shutdown()` to signal and join worker threads
    - Implemented destructor to ensure threads joined before destruction

---

### Final Behavior (Tested & Working)

#### Regular Play Endpoint
```bash
POST /api/v1/animation/play {"universe": 666, "animation_id": "..."}
```
- ✅ Cancels current animation
- ✅ Stops playlist permanently (removed from cache)
- ✅ Plays single animation without conflicts
- ✅ Future PlaylistEvents become no-ops

#### Interrupt Endpoint (Primary Use Case)
```bash
POST /api/v1/animation/interrupt {
  "universe": 666,
  "animation_id": "96cf405b-...",
  "resumePlaylist": true
}
```
- ✅ Cancels current playlist animation
- ✅ Marks playlist as interrupted
- ✅ Plays interrupt animation (uninterrupted by queued PlaylistEvents)
- ✅ **Automatically resumes playlist** when interrupt finishes
- ℹ️ Playlist restarts from beginning (acceptable for looping animations)

**Use Case:** Interactive Zoom meetings where creature (Mango) plays lean animation on loop, gets interrupted by button press from iPhone for reaction animations, then automatically returns to leaning.

---

### Testing Results

**Environment:** macOS with cooperative scheduler enabled

**Scenarios Tested:**
1. ✅ Playlist playing → interrupt → automatic resume
2. ✅ Regular play cancels concurrent animations
3. ✅ Playlist events respect interrupted state
4. ✅ Multiple interrupts in sequence
5. ✅ Playlist restarts cleanly after interrupt
6. ✅ Playback works when creatures not sending health events (on-demand DB fetch)
7. ✅ DRY refactoring: Single source of truth for playlist state
8. ✅ State preservation across interrupt animations
9. ✅ Old queued PlaylistEvents handled (Kenny Stress Test)
10. ✅ Live streaming cancels active sessions and playlists
11. ✅ Recording mode integration (same as live streaming)
12. ✅ WebSocket threads shut down gracefully (mostly)

**Bugs Fixed During Testing:** 10 critical bugs identified and resolved
- Frame calculation errors
- Concurrent playback conflicts
- Interrupted playlist resumption timing
- Code duplication (DRY violation)
- State management issues
- Session lifecycle bugs
- Creature cache dependency
- Race condition: concurrent animations on interrupt (2 different races!)
- Old queued events firing during interrupts
- WebSocket thread shutdown crashes

**Additional Improvements:**
- Complete creature cache audit - all `get()` calls protected with `contains()`
- On-demand database fetching when cache misses
- Integration with all motion control paths (scheduled, streaming, recording)
- Graceful WebSocket worker thread shutdown

**Known Limitations:**
- Playlist resumes from the beginning (not mid-animation) - **Acceptable per user**
- Server may crash during `std::exit()` in some cases - **Harmless, user accepted**
- No playlist queue/priority system - future enhancement if needed

---

### TODOs Completed

- ✅ ~~Automatic playlist resumption after interrupt completes~~
- ✅ ~~SessionManager.resumePlaylist() implementation~~
- ✅ ~~Auto-registering sessions in CooperativeAnimationScheduler~~

### TODOs Deferred (Not Needed)

- ❌ Resume playlist mid-animation (restarting from beginning is acceptable)
- ❌ Add resume timeout/auto-cleanup (not needed for current use case)

---

## Original Implementation Session

**Date:** 2025-10-13
**Status:** ✅ Implementation Complete - Tested 2025-10-18

## What Was Implemented

### Core Feature: Animation Interrupt System
Implemented a frame-by-frame cooperative animation scheduler with real-time interrupt capabilities for interactive Zoom meetings (creature behind desk can be interrupted with button press from iPhone).

### Key Components

**1. SessionManager** (`src/server/animation/SessionManager.h/cpp`)
- Thread-safe session tracking per universe
- `interrupt(universe, animation, shouldResumePlaylist)` method
- Stores interrupted playlist state for future resumption
- Full OpenTelemetry spans with detailed attributes

**2. REST API Endpoint** (`src/server/ws/controller/AnimationController.h:243`)
- `POST /api/v1/animation/interrupt`
- Request body: `{animation_id, universe, resumePlaylist}`
- Returns 400 if legacy scheduler active (requires `--scheduler cooperative`)
- Returns 500 if scheduling fails
- Returns 200 on success
- Full OTel instrumentation on all code paths

**3. PlayAnimationRequestDto** (`src/server/ws/dto/PlayAnimationRequestDto.h`)
- Added optional `resumePlaylist` boolean parameter (defaults to false)
- Client controls whether playlist should auto-resume after interrupt

**4. Command-Line Interface** (`src/server/config/CommandLine.cpp`)
- `--scheduler legacy` (default, safe, no interrupts)
- `--scheduler cooperative` (experimental, enables interrupts)

**5. Global State** (`src/server/main.cpp:218`)
- SessionManager initialized at startup
- Available globally via `creatures::sessionManager`

### Documentation & Testing

**Documentation** (`docs/`)
- `cooperative-scheduler.md` - Complete architecture doc with 2 Mermaid sequence diagrams
- `TESTING.md` - Testing guide with 5 scenarios and troubleshooting
- `README.md` - Documentation index
- `test-interrupt.sh` - Interactive test script with color-coded output

**Test Script Features:**
- Validates animation exists before interrupting
- Color-coded success/error output
- Pretty JSON formatting (if jq available)
- Usage examples and helpful error messages

## File Changes

### Created
- `src/server/animation/SessionManager.h`
- `src/server/animation/SessionManager.cpp`
- `docs/cooperative-scheduler.md`
- `docs/TESTING.md`
- `docs/README.md`
- `docs/test-interrupt.sh` (executable)

### Modified
- `src/server/ws/controller/AnimationController.h` - Added interrupt endpoint
- `src/server/ws/dto/PlayAnimationRequestDto.h` - Added resumePlaylist field
- `src/server/config/CommandLine.cpp` - Added --scheduler option
- `src/server/config/Configuration.h/cpp` - Added AnimationSchedulerType enum
- `src/server/main.cpp` - Initialize SessionManager

## Current State

### ✅ Complete & Built
- All code compiles cleanly
- clang-format applied to all modified files
- SessionManager linked into executable
- Test script is executable and ready to use

### 🔄 Ready for Testing
Start server with:
```bash
cd build/
./creature-server --scheduler cooperative
```

Run tests with:
```bash
./docs/test-interrupt.sh YOUR_ANIMATION_ID 1 true
```

### 📊 Observability
All operations fully instrumented with OpenTelemetry:
- RequestSpan: `POST /api/v1/animation/interrupt`
- OperationSpan: `SessionManager.interrupt`
- OperationSpan: `SessionManager.registerSession`

**Key attributes:**
- `universe`, `animation.id`, `resume_playlist`
- `interrupt.should_resume_playlist`, `interrupted_playlist`
- `error.type`, `error.message` (on failures)
- `http.status_code`, `scheduler_type`

## What Needs Testing

### Core Functionality
1. ✓ Interrupt single animation
2. ✓ Interrupt playlist without resume
3. ✓ Interrupt playlist with resume (`resumePlaylist: true`)
4. ✓ Validate 400 error with legacy scheduler
5. ✓ Multiple interrupts in sequence
6. ✓ Interrupt when nothing is playing

### Edge Cases to Verify
- Thread safety under concurrent requests
- Interrupt during playlist vs. single animation
- SessionManager state after interrupt completes
- Proper cancellation of previous session

### Known TODOs
- [ ] Automatic playlist resumption after interrupt completes (state is saved, logic not yet implemented)
- [ ] SessionManager.resumePlaylist() is stubbed - needs full implementation
- [ ] Consider auto-registering sessions in CooperativeAnimationScheduler
- [ ] Add resume timeout/auto-cleanup for interrupted playlists

## Testing Workflow

```bash
# Terminal 1: Start server
cd build/
./creature-server --scheduler cooperative

# Terminal 2: Run tests
cd /Users/april/code/creature-server
./docs/test-interrupt.sh

# Watch for:
# - Green ✓ success messages
# - [info] SessionManager logs
# - Honeycomb traces (if configured)
```

## Likely Issues to Watch For

1. **Timing Issues**
   - Interrupt might arrive between frames
   - Check atomic cancellation flag is working
   - Verify mutex locking is correct

2. **State Management**
   - Playlist state might not be preserved correctly
   - Check `shouldResumePlaylist` flag persists
   - Verify session cleanup happens properly

3. **Error Handling**
   - Invalid animation IDs
   - Universe that doesn't exist
   - Concurrent interrupts on same universe

4. **Observability**
   - Verify all spans are closing properly
   - Check parent/child span relationships
   - Ensure no span leaks

## Architecture Notes

### Thread Safety Strategy
- `SessionManager` uses `std::mutex` for all operations
- `PlaybackSession.cancelled_` is `std::atomic<bool>` for lock-free checks
- Cooperative scheduler checks cancellation before each frame

### Interrupt Flow
1. Client → REST API → SessionManager.interrupt()
2. SessionManager cancels current session (sets atomic flag)
3. If playlist: mark interrupted, save state, set shouldResumePlaylist
4. Schedule new interrupt animation via CooperativeAnimationScheduler
5. Register new session (preserving interrupt state)
6. Frame-by-frame playback checks isCancelled() between frames

### Why Cooperative Scheduler?
Legacy scheduler schedules all frames upfront (bulk events), making cancellation impossible. Cooperative scheduler schedules one frame at a time, checking cancellation between each frame.

## Next Session

When testing reveals issues, focus on:
1. SessionManager state transitions
2. Atomic flag timing and visibility
3. Span relationships in Honeycomb
4. Actual interrupt behavior on hardware

If testing goes well, next features:
1. Implement automatic playlist resumption
2. Add interrupt queue/priority system
3. Add fade transitions between animations
4. WebSocket notifications for interrupt events

## Quick Reference

**Scheduler Types:**
- Legacy (default): Bulk event scheduling, no interrupts
- Cooperative: Frame-by-frame, enables interrupts

**Command Line:**
```bash
./creature-server --scheduler cooperative
```

**Test Script:**
```bash
./docs/test-interrupt.sh [animation_id] [universe] [resumePlaylist]
```

**API Endpoint:**
```bash
POST /api/v1/animation/interrupt
{
  "animation_id": "507f1f77bcf86cd799439011",
  "universe": 1,
  "resumePlaylist": true
}
```

**Key Files:**
- SessionManager: `src/server/animation/SessionManager.{h,cpp}`
- Interrupt API: `src/server/ws/controller/AnimationController.h:243`
- Test Script: `docs/test-interrupt.sh`
- Architecture Docs: `docs/cooperative-scheduler.md`
- Testing Guide: `docs/TESTING.md`

---

## Session Timeline

### Initial Testing Session (2025-10-18)
- Fixed Bugs #1-9 (playlist loop, concurrent playback, race conditions, cache issues)
- Implemented DRY refactoring with PlaylistState enum
- Added SessionManager integration throughout the codebase
- Fixed all creature cache safety issues

### Follow-up Session (Context Continuation)
- Fixed Bug #10 (old queued PlaylistEvents)
- Integrated with live streaming and recording modes
- Completed creature cache audit (LegacyAnimationScheduler, playlist.cpp)
- Fixed WebSocket thread shutdown crashes
- Decided to accept harmless shutdown crashes on exit

---

## Sound Playback During Animations - Bug Fix Session

**Date:** 2025-10-23
**Status:** ✅ Fixed and Verified
**Context:** Sound playback broke during cooperative scheduler implementation

### Summary

After implementing the interrupt system, sound playback during animations completely stopped working. Controllers saw SSRC rotation but received no audio data. Through systematic debugging (including Wireshark packet capture), we discovered and fixed **four critical bugs** in the audio dispatch pipeline.

### Bugs Found and Fixed

#### Bug #11: Stale Frame Numbers After Audio Loading
**Problem:** Audio loading in `CooperativeAnimationScheduler` is synchronous and blocks. By the time `scheduleAnimation()` finished loading the audio buffer, the `startingFrame` was already in the past, causing timing issues.

**Root Cause:** Unlike `MusicEvent` which loads audio asynchronously in a background thread and recalculates the frame number afterward, `CooperativeAnimationScheduler` loaded audio synchronously without adjusting the starting frame.

**Fix:** Recalculate `startingFrame` after audio loading completes, matching the pattern in `MusicEvent::scheduleRtpAudio()`:
```cpp
// src/server/animation/CooperativeAnimationScheduler.cpp:76-86
// Audio loading is synchronous and heavy I/O - recalculate starting frame
startingFrame = eventLoop->getNextFrameNumber() + 2; // +2 to allow for reset event

// Apply animation delay for audio sync compensation if configured
uint32_t delayMs = config->getAnimationDelayMs();
if (delayMs > 0) {
    framenum_t delayFrames = delayMs / EVENT_LOOP_PERIOD_MS;
    startingFrame += delayFrames;
}

session->setStartingFrame(startingFrame);
```

**Impact:** Animation and audio now start at the correct synchronized time.

---

#### Bug #12: Missing SSRC Rotation for Animation Audio
**Problem:** No `RtpEncoderResetEvent` was scheduled before animation audio playback, so controllers never detected the new audio stream (SSRC remained unchanged).

**Root Cause:** `MusicEvent` explicitly schedules `RtpEncoderResetEvent` to rotate SSRC values before streaming audio, but `CooperativeAnimationScheduler` did not implement this step.

**Fix:** Schedule `RtpEncoderResetEvent` one frame before animation starts (for RTP audio mode):
```cpp
// src/server/animation/CooperativeAnimationScheduler.cpp:90-96
// If using RTP audio, schedule encoder reset event before playback starts
// This rotates SSRC values so controllers can detect the new audio stream
if (config->getAudioMode() == Configuration::AudioMode::RTP) {
    framenum_t resetFrame = startingFrame - 1;
    auto resetEvent = std::make_shared<RtpEncoderResetEvent>(resetFrame, 4); // 4 silent frames
    eventLoop->scheduleEvent(resetEvent);
    debug("Scheduled RtpEncoderResetEvent for frame {} (one frame before animation starts)", resetFrame);
}
```

**Impact:** Controllers now detect the SSRC rotation and prepare to receive audio.

---

#### Bug #13: Thread-Local hasStarted Bug
**Problem:** After the first animation played, subsequent animations would never call `onStart()`, so the audio transport was never started. Audio would work once, then fail for all subsequent animations.

**Root Cause:** `PlaybackRunnerEvent` used `static thread_local bool hasStarted` which was shared across **ALL sessions on the same thread**. Once set to true by the first animation, it remained true forever.

```cpp
// BUG: src/server/eventloop/events/playback-runner.cpp:77-81 (before fix)
static thread_local bool hasStarted = false;
if (!hasStarted) {
    session_->invokeOnStart();
    hasStarted = true;  // ❌ Never resets! Affects all future animations!
}
```

**Fix:** Moved `hasStarted_` to be per-session in `PlaybackSession` class instead of thread-local static:

```cpp
// src/server/animation/PlaybackSession.h:164-173
bool invokeOnStart() {
    if (hasStarted_) {
        return false;
    }
    hasStarted_ = true;
    if (onStart_) {
        onStart_();
    }
    return true;
}
```

```cpp
// src/server/eventloop/events/playback-runner.cpp:77
// Invoke onStart callback on first execution
session_->invokeOnStart();  // ✅ Each session tracks its own state
```

**Impact:** Every animation now properly starts its audio transport, not just the first one.

---

#### Bug #14: Audio Playing Too Slowly
**Problem:** After fixing bugs #11-13, audio played at the correct time and SSRC rotated correctly, but the audio playback was extremely slow (e.g., 5x slower than normal).

**Root Cause:** `RtpAudioTransport::dispatchNextChunk()` only sent **one audio frame per call**. Since `PlaybackRunnerEvent` runs at the animation's frame rate (e.g., every 100ms for slow animations) but audio frames need to be sent every 20ms, only 1 frame was sent per 100ms instead of 5 frames.

**Timeline:**
- Animation frame rate: 100ms per frame
- Audio frame rate: 20ms per frame (50Hz)
- Expected: 5 audio frames sent per animation frame
- Actual: 1 audio frame sent per animation frame
- Result: Audio plays at 1/5 speed

**Fix:** Modified `dispatchNextChunk()` to send **all frames that are due**, not just one:

```cpp
// src/server/audio/RtpAudioTransport.cpp:68-86
// Dispatch all audio frames that are due (might be multiple if PlaybackRunnerEvent runs infrequently)
while (currentFrame >= nextDispatchFrame_ && currentFrameIndex_ < totalFrames_ && !stopped_) {
    // Send this frame to all 17 RTP channels (16 creatures + 1 BGM)
    for (int ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
        rtpServer_->send(static_cast<uint8_t>(ch),
                         audioBuffer->getEncodedFrame(static_cast<uint8_t>(ch), currentFrameIndex_));
    }

    // Advance to next frame
    currentFrameIndex_++;

    // Calculate next dispatch time
    if (currentFrameIndex_ < kPrefillFrames) {
        nextDispatchFrame_ = nextDispatchFrame_ + 1; // 1ms later
    } else {
        nextDispatchFrame_ = nextDispatchFrame_ + (RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS); // 20ms later
    }
}
```

**Impact:** Audio now plays at the correct speed, with multiple audio frames dispatched in a batch to catch up when necessary.

---

### New Feature: Animation Delay for Audio Sync

**Motivation:** SDL → pipewire → speakers introduces buffering latency. Animations were starting slightly before the audio became audible, causing sync issues.

**Implementation:** Added `--animation-delay-ms` command line parameter to delay animation start:

```cpp
// src/server/config/CommandLine.cpp:134-138
program.add_argument("--animation-delay-ms")
    .help("delay in milliseconds before starting animation playback (for audio sync compensation)")
    .default_value(0)
    .scan<'i', int>()
    .nargs(1);
```

**Usage:**
```bash
./creature-server --scheduler cooperative --animation-delay-ms 100
```

**How it works:** Delay is added to the animation starting frame after audio loading completes, effectively delaying DMX output while audio plays at the correct time.

**Tuning:** Users can adjust the delay value to compensate for their specific audio pipeline latency (SDL, pipewire, hardware buffer bloat, etc.).

---

### Configuration Change: Cooperative Scheduler Now Default

**Motivation:** After a week of production testing, the cooperative scheduler proved stable and reliable. All known bugs were fixed and the interrupt system works flawlessly.

**Change:** Updated default scheduler from `Legacy` to `Cooperative`:

```cpp
// src/server/config/Configuration.h:205
AnimationSchedulerType animationSchedulerType =
    AnimationSchedulerType::Cooperative; ///< Default to cooperative (tested and stable)
```

```cpp
// src/server/config/CommandLine.cpp:131
.default_value(std::string("cooperative"))
```

**Fallback:** Legacy scheduler remains available via `--scheduler legacy` if needed.

**Production Features:**
- ✅ Frame-by-frame cooperative scheduling
- ✅ Real-time animation interrupts
- ✅ Automatic playlist resumption
- ✅ Sound playback during animations
- ✅ SSRC rotation and encoder resets
- ✅ Audio sync compensation
- ✅ Live streaming integration
- ✅ Recording mode integration

---

### Files Modified (Sound Playback Fix)

1. **src/server/animation/CooperativeAnimationScheduler.cpp**
   - Recalculate starting frame after audio loading
   - Schedule RtpEncoderResetEvent for SSRC rotation
   - Apply animation delay for audio sync compensation

2. **src/server/animation/PlaybackSession.h**
   - Added `hasStarted_` member variable (per-session)
   - Modified `invokeOnStart()` to return bool and track state
   - Added `setStartingFrame()` method declaration

3. **src/server/animation/PlaybackSession.cpp**
   - Implemented `setStartingFrame()` to update starting frame and all track states

4. **src/server/eventloop/events/playback-runner.cpp**
   - Removed broken `static thread_local bool hasStarted`
   - Changed to use session's `invokeOnStart()` method

5. **src/server/audio/RtpAudioTransport.cpp**
   - Modified `dispatchNextChunk()` to send all frames that are due (while loop)
   - Fixed audio playback speed issue

6. **src/server/config/CommandLine.cpp**
   - Added `--animation-delay-ms` parameter
   - Changed default scheduler to `cooperative`
   - Updated help text

7. **src/server/config/Configuration.h**
   - Added `animationDelayMs` member variable
   - Added getter/setter declarations
   - Changed default scheduler to `Cooperative`

8. **src/server/config/Configuration.cpp**
   - Implemented `getAnimationDelayMs()` and `setAnimationDelayMs()`

---

### Debugging Process

**Discovery Method:** Wireshark packet capture on macOS revealed that RTP packets WERE being sent correctly, proving the audio dispatch logic worked. The issue was just macOS routing multicast packets to the wrong interface.

**Key Insight:** "Don't over-index on SSRC rotation" - the real issue wasn't just SSRC rotation, but also stale frame numbers, thread-local bugs, and audio dispatch rate.

**Testing Approach:**
1. Added multicast route on macOS: `sudo route add -net 239.19.63.0/24 -interface en10`
2. Verified SSRC rotation in Wireshark
3. Discovered audio was too slow
4. Fixed dispatch rate bug
5. Tested audio sync with delay parameter

---

### Final Verification

**Test Results:**
- ✅ SSRC rotation visible in Wireshark
- ✅ RTP packets sent at correct rate (every 20ms)
- ✅ Audio plays at correct speed
- ✅ Animation and audio synchronized
- ✅ First animation works
- ✅ Subsequent animations work (thread-local bug fixed)
- ✅ Animation delay parameter allows fine-tuning sync

**Production Status:** ✅ Sound playback during animations fully functional

---

**Build Status:** ✅ Clean build, no errors
**Code Format:** ✅ clang-format applied to all modified files
**Testing:** ✅ Verified on macOS with multicast routing
**Production Status:** ✅ Ready for deployment with sound playback!

## Ad-Hoc Speech Animations (Nov 2025)

**Status:** ✅ API & job pipeline shipped

### Highlights
- Creatures now expose `speech_loop_animation_ids`, letting the server pick randomized loop bases when injecting Rhubarb mouth data.
- New `/api/v1/animation/adhoc` endpoint queues an `AdHocSpeech` job (returns `JobCreatedDto` so clients rely on the existing job-progress websocket flow).
- Job pipeline: ElevenLabs speech → multi-channel WAV conversion → Rhubarb JSON → `SoundDataProcessor` mouth-slot splice → Mongo insert into TTL-backed `adhoc_animations` collection → cooperative interrupt playback on the controller’s current universe.
- Generated artifacts live under `${TMPDIR}/creature-adhoc/<job_id>/adhoc_<creature>_<timestamp>_<slug>.{wav,mp3,json}`; directories older than `--adhoc-animation-ttl-hours` (default 12h) are cleaned at startup alongside the Mongo TTL.
- Shared `SpeechGenerationManager` keeps `VoiceService` and the ad-hoc job DRY; scheduler remains cooperative-only (job fails fast otherwise).

### Key Files
- `src/model/Creature.*`, `src/server/creature/helpers.cpp` – schema update (speech loop IDs).
- `src/server/ws/controller/AnimationController.h` – new POST `/api/v1/animation/adhoc`.
- `src/server/voice/SpeechGenerationManager.*` – centralized speech + conversion helper.
- `src/server/jobs/JobWorker.*` – `AdHocSpeech` job handler with temp-file lifecycle.
- `src/server/config/*`, `src/server/database.*`, `src/server/animation/upsert.cpp` – TTL config/index insertion.
- `src/server/animation/CooperativeAnimationScheduler.cpp`, `src/server/audio/LocalSdlAudioTransport.cpp` – absolute/relative sound path handling for ad-hoc WAVs.

**Next:** consider periodic runtime cleanup for temp dirs (currently done on startup) and add console UX affordances for launching/monitoring ad-hoc speech from the field.

### 2025-11-xx Usage Note
- API now fails fast if a creature lacks `speech_loop_animation_ids`, returning `422 Unprocessable Entity` with the creature’s display name.
- `/api/v1/animation/adhoc` changed to port 8000 for local curl testing.
- Heavy usage warning: job queue + temp directory cleanup share the same TTL (`--adhoc-animation-ttl-hours`). Keep an eye on disk if ad-hoc usage spikes faster than cleanup cadence.
- WebSocket cache invalidations now emit `cache_type` values `ad-hoc-animation-list` and `ad-hoc-sound-list` whenever new ad-hoc jobs land. Have clients listen for these strings to refresh `/api/v1/animation/ad-hoc` or `/api/v1/sound/ad-hoc` datasets.

### 2025-11-xx Latest Changes
- Added `GET /api/v1/animation/ad-hoc/{id}` so clients can pull the fully synthesized animation payload (metadata + tracks). Docs updated with curl/test guidance.
- Client cache enums now understand the two ad-hoc cache invalidation strings; SwiftUI processor currently just logs pending refresh logic.
- Job worker titles ad-hoc animations as `<Creature> - <timestamp> - <slug>` to match console expectations.
- Database lookup for ad-hoc fetch checks both `id` and `metadata.animation_id`, fixing the earlier 404 when clients used the DTO’s `animation_id`.
