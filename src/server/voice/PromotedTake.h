#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "server/voice/DialogPipeline.h"
#include "util/Result.h"

namespace creatures::voice {

/// Which creature voice sits on which lane of a 17-channel dialog WAV.
struct PromotedTakeLane {
    uint16_t channel; // 1-based
    std::string voiceId;
};

/// Rebuild an assembled take from a script's PROMOTED accepted-voice WAV
/// (#208). An acceptance made before the durable store existed (#146) is in
/// neither store, but its promoted 17-channel WAV is the render's own
/// output: one lane per creature, and an iXML chunk carrying each lane's
/// mouth cues and word timing. Everything a render needs is there, already
/// on the tightened timeline — so this reads it back into a DialogAssembled
/// with `mouthCues` pre-baked instead of re-deriving anything.
///
/// `lanes` maps channels to voices (from the job's creature cache); lanes the
/// file names in its TRACK_LIST that aren't in `lanes` are skipped, and a
/// named lane without lip-sync cues just renders with a closed mouth, which
/// is what the original render did. Fails when the file has no timing at
/// all rather than producing a silent-faced render.
///
/// Works on any 17-channel dialog WAV the server wrote — a promoted voice
/// take or a finished render of the same take (whose provenance names the
/// take in GENERATION_IDS). Trailing all-lane silence is dropped, since a
/// render's timeline runs to the end of its music.
Result<DialogAssembled> loadAssembledTakeFromPromotedFile(const std::string &soundFile,
                                                          const std::vector<PromotedTakeLane> &lanes);

} // namespace creatures::voice
