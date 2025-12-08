#pragma once

namespace creatures::runtime {

enum class ActivityState { Running, Idle, Disabled, Stopped };

enum class ActivityReason { Play, Playlist, AdHoc, Idle, Disabled, Cancelled };

inline const char *toString(ActivityState state) {
    switch (state) {
    case ActivityState::Running:
        return "running";
    case ActivityState::Idle:
        return "idle";
    case ActivityState::Disabled:
        return "disabled";
    case ActivityState::Stopped:
        return "stopped";
    }
    return "unknown";
}

inline const char *toString(ActivityReason reason) {
    switch (reason) {
    case ActivityReason::Play:
        return "play";
    case ActivityReason::Playlist:
        return "playlist";
    case ActivityReason::AdHoc:
        return "ad_hoc";
    case ActivityReason::Idle:
        return "idle";
    case ActivityReason::Disabled:
        return "disabled";
    case ActivityReason::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

} // namespace creatures::runtime
