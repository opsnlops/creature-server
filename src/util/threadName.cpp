

#include <thread>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

#include "threadName.h"

/**
 * Give the current thread a name
 *
 * This currently only works on macOS and Linux, since the thread spec
 * is a bit different between them.
 *
 * @param name the name to assign to a thread
 */
void setThreadName(const std::string &name) {
#if defined(__linux__)
    // Linux implementation
    pthread_setname_np(pthread_self(), name.c_str());
#elif defined(__APPLE__)
    // macOS implementation
    pthread_setname_np(name.c_str());
#elif
#warning "Threads won't be named, I don't know what platform this is"
#endif
}