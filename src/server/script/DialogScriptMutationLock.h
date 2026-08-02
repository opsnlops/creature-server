#pragma once

#include <mutex>

namespace creatures::script {

/// Serializes server-side read/modify/write mutations of a DialogScript.
///
/// Dialog script updates replace the full MongoDB document. Promotion also
/// needs to preserve every current script field while attaching its verified
/// background-music reference, so the two operations must not interleave.
/// Creature Server is the sole writer for this collection; keeping the lock in
/// one inline accessor makes that invariant explicit across controllers and
/// services without duplicating mutexes.
inline std::mutex &mutationMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace creatures::script
