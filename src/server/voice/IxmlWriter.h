#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace creatures::voice {

/// One interleaved track in a dialog WAV, for the iXML `<TRACK_LIST>`.
/// `channel` is 1-based (matches the creature `audio_channel` convention and
/// iXML's own 1-based CHANNEL_INDEX). The BGM lane is channel 17.
struct DialogTrackInfo {
    uint16_t channel;
    std::string name; // creature name, or "BGM" for the music lane

    bool operator==(const DialogTrackInfo &) const = default;
};

/// One turn of the rendered script, for the iXML `<USER><DIALOG_SCRIPT>` block.
struct DialogScriptLine {
    std::string speaker; // resolved creature name (falls back to creature_id)
    std::string text;

    bool operator==(const DialogScriptLine &) const = default;
};

/// One mouth cue: a mouth shape held over a time span (Rhubarb visemes).
struct DialogLipsyncCue {
    double start;      // seconds
    double end;        // seconds
    std::string shape; // Rhubarb phoneme letter (A–H, X)

    bool operator==(const DialogLipsyncCue &) const = default;
};

/// The lip-sync (mouth cues) for one creature's lane, for the iXML
/// `<USER><LIPSYNC>` block (issue #53). A private, creature-console-only block —
/// nothing else reads it, but it makes the file fully self-describing.
struct DialogLipsyncTrack {
    uint16_t channel; // 1-based audio channel this creature is on
    std::string name; // creature name
    std::vector<DialogLipsyncCue> cues;

    bool operator==(const DialogLipsyncTrack &) const = default;
};

/// Everything we want to stamp into a permanent dialog WAV so an otherwise
/// anonymous UUID-named file can be traced back to the script that made it
/// (issue #47). A point-in-time snapshot, mirroring the semantics of
/// `Animation.metadata.source_script_turns` — a copy, not a live pointer.
struct DialogWavProvenance {
    std::string sourceScriptId;              // may be empty (ad-hoc renders have none)
    std::string title;                       // scene title; may be empty
    std::vector<std::string> generationIds;  // per-chunk ElevenLabs generations, in order
    std::vector<DialogTrackInfo> tracks;     // channel → name (creature lanes + BGM)
    std::vector<DialogScriptLine> script;    // ordered turns, speaker + text
    std::vector<DialogLipsyncTrack> lipsync; // per-creature mouth cues (from ElevenLabs alignment)

    /// True when there's nothing worth embedding — writers can skip the chunk.
    [[nodiscard]] bool empty() const {
        return sourceScriptId.empty() && title.empty() && generationIds.empty() && tracks.empty() && script.empty() &&
               lipsync.empty();
    }

    bool operator==(const DialogWavProvenance &) const = default;
};

/// Build the iXML document (a BWFXML string) describing a dialog WAV's
/// provenance. All values are XML-escaped. This is the payload that goes inside
/// the RIFF `iXML` chunk.
///
/// `totalChannels`, when > 0, forces a **complete** TRACK_LIST: one TRACK per
/// interleaved channel 1..totalChannels, `TRACK_COUNT == totalChannels`, names
/// filled from `provenance.tracks` by channel and left empty on silent lanes.
/// Field recorders / Wave Agent / DAWs expect this (a sparse list with a
/// mismatched count is ignored). When 0 (the default, e.g. a mono export), the
/// TRACK_LIST is emitted only if `provenance.tracks` is non-empty, verbatim.
[[nodiscard]] std::string buildDialogIxml(const DialogWavProvenance &provenance, int totalChannels = 0);

/// Wrap an iXML document string as a complete RIFF `iXML` chunk: the 4-byte id,
/// a little-endian 4-byte size, the payload, and a pad byte if the payload
/// length is odd (RIFF chunks are word-aligned). The returned bytes append
/// directly after a WAV's `data` chunk; the caller bumps the outer RIFF size.
[[nodiscard]] std::vector<uint8_t> makeIxmlChunk(const std::string &ixmlDocument);

} // namespace creatures::voice
