#include "util/ThreadPriority.h"

#include <sys/resource.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace creatures::util {

bool lowerCurrentThreadPriority() {
#if defined(__APPLE__)
    return setpriority(PRIO_DARWIN_THREAD, 0, PRIO_DARWIN_BG) == 0;
#elif defined(__linux__)
    // On Linux a thread's nice value is its own; PRIO_PROCESS with the
    // kernel thread id addresses just this thread.
    const auto tid = static_cast<id_t>(syscall(SYS_gettid));
    return setpriority(PRIO_PROCESS, tid, 10) == 0;
#else
    return false;
#endif
}

} // namespace creatures::util
