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
};

/// One turn of the rendered script, for the iXML `<USER><DIALOG_SCRIPT>` block.
struct DialogScriptLine {
    std::string speaker; // resolved creature name (falls back to creature_id)
    std::string text;
};

/// Everything we want to stamp into a permanent dialog WAV so an otherwise
/// anonymous UUID-named file can be traced back to the script that made it
/// (issue #47). A point-in-time snapshot, mirroring the semantics of
/// `Animation.metadata.source_script_turns` — a copy, not a live pointer.
struct DialogWavProvenance {
    std::string sourceScriptId;             // may be empty (ad-hoc renders have none)
    std::string title;                      // scene title; may be empty
    std::vector<std::string> generationIds; // per-chunk ElevenLabs generations, in order
    std::vector<DialogTrackInfo> tracks;    // channel → name (creature lanes + BGM)
    std::vector<DialogScriptLine> script;   // ordered turns, speaker + text

    /// True when there's nothing worth embedding — writers can skip the chunk.
    [[nodiscard]] bool empty() const {
        return sourceScriptId.empty() && title.empty() && generationIds.empty() && tracks.empty() && script.empty();
    }
};

/// Build the iXML document (a BWFXML string) describing a dialog WAV's
/// provenance. All values are XML-escaped. This is the payload that goes inside
/// the RIFF `iXML` chunk.
[[nodiscard]] std::string buildDialogIxml(const DialogWavProvenance &provenance);

/// Wrap an iXML document string as a complete RIFF `iXML` chunk: the 4-byte id,
/// a little-endian 4-byte size, the payload, and a pad byte if the payload
/// length is odd (RIFF chunks are word-aligned). The returned bytes append
/// directly after a WAV's `data` chunk; the caller bumps the outer RIFF size.
[[nodiscard]] std::vector<uint8_t> makeIxmlChunk(const std::string &ixmlDocument);

} // namespace creatures::voice
