

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>

#include "Track.h"

namespace creatures {

// Convert TrackDto to Track
Track convertFromDto(const std::shared_ptr<TrackDto> &trackDto) {
    Track track;
    track.id = trackDto->id;
    track.creature_id = trackDto->creature_id ? std::string(trackDto->creature_id) : "";
    track.fixture_id = trackDto->fixture_id ? std::string(trackDto->fixture_id) : "";
    track.animation_id = trackDto->animation_id;

    // Ensure the list is initialized before iterating
    track.frames = std::vector<std::string>();
    if (trackDto->frames) { // Check if the list is not null
        for (const auto &frame : *trackDto->frames) {
            track.frames.push_back(std::string(frame));
        }
    }

    return track;
}

// Convert Track to TrackDto
oatpp::Object<TrackDto> convertToDto(const Track &track) {
    auto trackDto = TrackDto::createShared();
    trackDto->id = track.id;
    if (!track.creature_id.empty()) {
        trackDto->creature_id = track.creature_id;
    }
    if (!track.fixture_id.empty()) {
        trackDto->fixture_id = track.fixture_id;
    }
    trackDto->animation_id = track.animation_id;

    trackDto->frames = oatpp::List<oatpp::String>::createShared();
    for (const auto &frame : track.frames) {
        trackDto->frames->emplace_back(frame);
    }

    return trackDto;
}

} // namespace creatures