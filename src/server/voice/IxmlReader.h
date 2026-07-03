#pragma once

#include <filesystem>
#include <optional>
#include <string>

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

} // namespace creatures::voice
