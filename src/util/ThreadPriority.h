#pragma once

namespace creatures::util {

/// Drop the calling thread to background priority, so housekeeping that
/// runs beside the show (publishing a streamed sentence's audio to the disk
/// cache after it has been scheduled, issue #197) yields to the event loop
/// and the RTP output whenever they want the CPU. Best-effort: returns false
/// when the platform refuses, and the caller carries on at normal priority.
bool lowerCurrentThreadPriority();

} // namespace creatures::util
