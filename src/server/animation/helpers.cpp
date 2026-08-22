#include "server/database.h"

#include <utility>

#include "model/Animation.h"
#include "model/Track.h"

namespace creatures {

Result<Track> Database::parseTrackJson(json trackJson) { return creatures::trackFromJson(trackJson); }

Result<Animation> Database::parseAnimationJson(json animationJson) {
    return creatures::animationFromJson(animationJson, AnimationJsonSource::Api);
}

} // namespace creatures
