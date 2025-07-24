
#pragma once

#include <thread>
#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

// Allow threads to be named
void setThreadName(const std::string &name);
