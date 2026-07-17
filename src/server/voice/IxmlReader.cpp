#include "server/voice/IxmlReader.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#include "server/voice/IxmlTimedTokens.h"

namespace creatures::voice {

namespace {

template <typename T> bool readLE(std::ifstream &in, T &out) {
    in.read(reinterpret_cast<char *>(&out), sizeof(T));
    return in.good();
}

} // namespace

std::optional<std::string> readIxmlChunk(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    char riff[4]{};
    uint32_t riffSize = 0;
    char wave[4]{};
    in.read(riff, 4);
    if (!readLE(in, riffSize)) {
        return std::nullopt;
    }
    in.read(wave, 4);
    if (!in.good() || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return std::nullopt;
    }

    // Walk subchunks looking for iXML, skipping every other chunk's payload
    // (including the large `data` chunk) by its declared size.
    while (in.good()) {
        char chunkId[4]{};
        uint32_t chunkSize = 0;
        in.read(chunkId, 4);
        if (!readLE(in, chunkSize)) {
            break;
        }

        if (std::memcmp(chunkId, "iXML", 4) == 0) {
            std::vector<char> payload(chunkSize);
            if (chunkSize > 0) {
                in.read(payload.data(), static_cast<std::streamsize>(chunkSize));
                if (in.gcount() != static_cast<std::streamsize>(chunkSize)) {
                    return std::nullopt; // truncated chunk
                }
            }
            return std::string(payload.begin(), payload.end());
        }

        // Skip this chunk's payload (+1 if odd-sized, for RIFF word alignment).
        std::streamoff skip = static_cast<std::streamoff>(chunkSize);
        if (chunkSize % 2 == 1) {
            skip += 1;
        }
        in.seekg(skip, std::ios::cur);
    }

    return std::nullopt;
}

namespace {

// Defined further down (same anonymous namespace); extractIxmlField delegates to it.
std::optional<std::string> extractField(std::string_view doc, std::string_view tag);

// Reverse of IxmlWriter's xmlEscape. Handles exactly the five entities we emit.
std::string xmlUnescape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] == '&') {
            if (in.compare(i, 5, "&amp;") == 0) {
                out += '&';
                i += 5;
                continue;
            }
            if (in.compare(i, 4, "&lt;") == 0) {
                out += '<';
                i += 4;
                continue;
            }
            if (in.compare(i, 4, "&gt;") == 0) {
                out += '>';
                i += 4;
                continue;
            }
            if (in.compare(i, 6, "&quot;") == 0) {
                out += '"';
                i += 6;
                continue;
            }
            if (in.compare(i, 6, "&apos;") == 0) {
                out += '\'';
                i += 6;
                continue;
            }
        }
        out += in[i];
        ++i;
    }
    return out;
}

} // namespace

std::optional<std::string> extractIxmlField(const std::string &ixmlDocument, const std::string &tag) {
    return extractField(ixmlDocument, tag);
}

namespace {

// The inner text of the first `<tag>…</tag>` at/after `from`, as a view into `doc`
// (no copy — the caller's document outlives the parse), plus the offset just past
// the closing tag. Used to scope sections/tracks; leaf values that need XML
// unescaping go through extractField, which allocates a fresh string.
struct ElementSpan {
    std::string_view content;
    std::size_t endPos;
};

std::optional<ElementSpan> firstElement(std::string_view doc, std::string_view tag, std::size_t from) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const auto start = doc.find(open, from);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto contentStart = start + open.size();
    const auto end = doc.find(close, contentStart);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return ElementSpan{doc.substr(contentStart, end - contentStart), end + close.size()};
}

// The XML-unescaped inner text of the first `<tag>…</tag>`, or nullopt. The
// string_view sibling of the public extractIxmlField (which delegates here).
std::optional<std::string> extractField(std::string_view doc, std::string_view tag) {
    if (auto span = firstElement(doc, tag, 0)) {
        return xmlUnescape(std::string(span->content));
    }
    return std::nullopt;
}

// The inner content of the first `<tag>…</tag>` as a raw view, for scoping a parent
// section (TRACK_LIST, LIPSYNC, WORD_ALIGNMENT) before iterating its children.
std::optional<std::string_view> sectionOf(std::string_view doc, std::string_view tag) {
    if (auto span = firstElement(doc, tag, 0)) {
        return span->content;
    }
    return std::nullopt;
}

uint16_t parseChannel(const std::optional<std::string> &s) {
    if (!s) {
        return 0;
    }
    return static_cast<uint16_t>(std::strtoul(s->c_str(), nullptr, 10));
}

// A section's `<TRACK>` rows as (channel, name, packed tokens). Shared by the
// LIPSYNC and WORD_ALIGNMENT parsers — they differ only in the section/payload tag
// and the struct they map the tokens into. Tracks without a CHANNEL_INDEX are
// skipped (consistent with the TRACK_LIST parser).
struct TimedTrack {
    uint16_t channel;
    std::string name;
    std::vector<TimedToken> tokens;
};

std::vector<TimedTrack> parseTimedTrackSection(std::string_view doc, std::string_view sectionTag,
                                               std::string_view payloadTag) {
    std::vector<TimedTrack> tracks;
    const auto section = sectionOf(doc, sectionTag);
    if (!section) {
        return tracks;
    }
    std::size_t pos = 0;
    while (auto track = firstElement(*section, "TRACK", pos)) {
        pos = track->endPos;
        auto channel = extractField(track->content, "CHANNEL_INDEX");
        if (!channel) {
            continue;
        }
        TimedTrack t;
        t.channel = parseChannel(channel);
        t.name = extractField(track->content, "NAME").value_or("");
        if (auto payload = extractField(track->content, payloadTag)) {
            t.tokens = parseTimedTokens(*payload);
        }
        tracks.push_back(std::move(t));
    }
    return tracks;
}

} // namespace

std::vector<DialogTrackInfo> parseIxmlTrackList(const std::string &ixmlDocument) {
    std::vector<DialogTrackInfo> tracks;
    const auto section = sectionOf(ixmlDocument, "TRACK_LIST");
    if (!section) {
        return tracks;
    }
    std::size_t pos = 0;
    // "<TRACK>" (with the closing '>') won't match "<TRACK_COUNT>", so the count
    // element is skipped naturally.
    while (auto track = firstElement(*section, "TRACK", pos)) {
        pos = track->endPos;
        auto channel = extractField(track->content, "CHANNEL_INDEX");
        if (!channel) {
            continue;
        }
        tracks.push_back(DialogTrackInfo{parseChannel(channel), extractField(track->content, "NAME").value_or("")});
    }
    return tracks;
}

std::vector<DialogLipsyncTrack> parseIxmlLipsync(const std::string &ixmlDocument) {
    std::vector<DialogLipsyncTrack> lipsync;
    for (auto &track : parseTimedTrackSection(ixmlDocument, "LIPSYNC", "CUES")) {
        DialogLipsyncTrack lt;
        lt.channel = track.channel;
        lt.name = std::move(track.name);
        lt.cues.reserve(track.tokens.size());
        for (auto &tok : track.tokens) {
            lt.cues.push_back(DialogLipsyncCue{tok.start, tok.end, std::move(tok.token)});
        }
        lipsync.push_back(std::move(lt));
    }
    return lipsync;
}

std::vector<DialogWordTrack> parseIxmlWordAlignment(const std::string &ixmlDocument) {
    std::vector<DialogWordTrack> words;
    for (auto &track : parseTimedTrackSection(ixmlDocument, "WORD_ALIGNMENT", "WORDS")) {
        DialogWordTrack wt;
        wt.channel = track.channel;
        wt.name = std::move(track.name);
        wt.words.reserve(track.tokens.size());
        for (auto &tok : track.tokens) {
            wt.words.push_back(DialogWordTiming{std::move(tok.token), tok.start, tok.end});
        }
        words.push_back(std::move(wt));
    }
    return words;
}

std::vector<DialogScriptLine> parseDialogScriptTurns(const std::string &scriptBlob) {
    std::vector<DialogScriptLine> turns;
    std::size_t i = 0;
    while (i < scriptBlob.size()) {
        const auto nl = scriptBlob.find('\n', i);
        const auto line = scriptBlob.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? scriptBlob.size() : nl + 1;
        if (line.empty()) {
            continue; // skip blank lines (e.g. a trailing newline)
        }
        // Split on the FIRST ": " so a line body that itself contains ": " survives.
        const auto sep = line.find(": ");
        if (sep == std::string::npos) {
            turns.push_back(DialogScriptLine{"", line});
        } else {
            turns.push_back(DialogScriptLine{line.substr(0, sep), line.substr(sep + 2)});
        }
    }
    return turns;
}

} // namespace creatures::voice
