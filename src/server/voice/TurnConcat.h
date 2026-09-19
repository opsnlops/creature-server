#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "model/Track.h"
#include "util/Result.h"

// Stitching a streamed dialog's turns into one animation (issue #186).
//
// Every turn of a streamed session is rendered as its own N-track animation,
// one track per participant, all the same length. At /finish the per-turn WAVs
// are concatenated sample-for-sample into one exchange WAV, and the tracks
// need concatenating to match — but each turn's frame count was rounded UP
// from its audio length, so simply appending frames drifts ahead of the audio
// by up to one frame per turn. Over a long exchange that is seconds of lip
// sync out of step at the end.
//
// The fix is to size each part from where its audio actually sits in the
// stitched file: part k contributes frames [F(start_k), F(end_k)) where F(s)
// is the frame that sample s falls in. Frames the turn rendered past that
// boundary are dropped; a turn that rendered short (never happens with
// ceil(), but be safe) repeats its last frame.
namespace creatures::voice {

/// One rendered turn: its tracks (one per participant, equal lengths) and
/// how many mono samples its audio occupies in the stitched WAV.
struct TurnTrackPart {
    std::vector<creatures::Track> tracks;
    uint64_t audioSamples{0};
};

struct ConcatenatedTurnTracks {
    // One track per participant, in the order of the first part's tracks,
    // stamped with fresh track ids and the given animation id.
    std::vector<creatures::Track> tracks;
    std::size_t totalFrames{0};
};

/// Concatenate the parts' tracks by participant (matched on creature_id),
/// sized so the result stays aligned with the concatenated audio.
///
/// InvalidData when: no parts, msPerFrame or sampleRate is 0, a part has no
/// tracks, a later part lacks a creature the first part has, or the total
/// would exceed MAX_ANIMATION_FRAMES_PER_TRACK.
Result<ConcatenatedTurnTracks> concatTurnTracks(const std::vector<TurnTrackPart> &parts, uint32_t msPerFrame,
                                                uint32_t sampleRate, const std::string &animationId);

} // namespace creatures::voice
