#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "server/voice/IxmlWriter.h" // reuse the writer's structs as the parse output

namespace creatures::voice {

/// Read the iXML chunk from a RIFF/WAVE file, returning its raw document string
/// (the BWFXML written by buildDialogIxml), or std::nullopt if the file has no
/// iXML chunk or isn't a readable WAV.
///
/// Walks the RIFF chunk list rather than assuming any fixed offset — the iXML
/// chunk is written after `data` (see writeDialogWav) but this tolerates it
/// anywhere. The audio `data` chunk is skipped without being read into memory.
[[nodiscard]] std::optional<std::string> readIxmlChunk(const std::filesystem::path &path);

/// Extract the (XML-unescaped) inner text of the first `<tag>...</tag>` element
/// in an iXML document, or std::nullopt if the tag is absent. Intended for the
/// flat, non-nested elements this project writes (SOURCE_SCRIPT_ID, TITLE,
/// DIALOG_SCRIPT, …) — it is a deliberately small extractor, not a general XML
/// parser.
[[nodiscard]] std::optional<std::string> extractIxmlField(const std::string &ixmlDocument, const std::string &tag);

/// Parse the `<TRACK_LIST>` into structured tracks (channel → name). Returns an
/// empty vector if there is no track list. The inverse of the writer's TRACK_LIST
/// emission — only the leaf `CHANNEL_INDEX` + `NAME` are read (INTERLEAVE_INDEX is
/// redundant with CHANNEL_INDEX for our poly WAVs).
[[nodiscard]] std::vector<DialogTrackInfo> parseIxmlTrackList(const std::string &ixmlDocument);

/// Parse the `<USER><LIPSYNC>` block into structured per-creature mouth cues,
/// unpacking the packed `"start end shape;start end shape;..."` `<CUES>` string
/// back into `DialogLipsyncCue`s. Empty vector if there is no LIPSYNC block.
[[nodiscard]] std::vector<DialogLipsyncTrack> parseIxmlLipsync(const std::string &ixmlDocument);

/// Parse the `<USER><WORD_ALIGNMENT>` block (issue #56, Part 2) into structured
/// per-creature word timings. Empty vector if the file carries none (old renders,
/// non-dialog sounds) — the field is present in the contract but fills in only
/// once a render persists it.
[[nodiscard]] std::vector<DialogWordTrack> parseIxmlWordAlignment(const std::string &ixmlDocument);

/// Split a `DIALOG_SCRIPT` blob ("Speaker: line\nSpeaker: line\n...") into
/// structured turns. Best-effort: splits each line on the FIRST ": " only, so a
/// line body containing ": " is preserved. A line with no ": " becomes a turn with
/// an empty speaker and the whole line as the body.
[[nodiscard]] std::vector<DialogScriptLine> parseDialogScriptTurns(const std::string &scriptBlob);

} // namespace creatures::voice
