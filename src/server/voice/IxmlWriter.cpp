#include "server/voice/IxmlWriter.h"

#include <cstddef>
#include <string>
#include <vector>

#include "server/voice/IxmlTimedTokens.h"

namespace creatures::voice {

namespace {

// Escape the five XML metacharacters. Ampersand must be replaced first so the
// entity ampersands introduced by the others aren't double-escaped.
std::string xmlEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string joinGenerationIds(const std::vector<std::string> &ids) {
    std::string joined;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            joined += ',';
        }
        joined += ids[i];
    }
    return joined;
}

} // namespace

std::string buildDialogIxml(const DialogWavProvenance &provenance, int totalChannels) {
    std::string xml;
    xml.reserve(512 + provenance.script.size() * 64);

    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<BWFXML>\n";
    xml += "  <IXML_VERSION>1.5</IXML_VERSION>\n";
    xml += "  <PROJECT>creature-server</PROJECT>\n";

    std::string note = "Dialog render";
    if (!provenance.title.empty()) {
        note += ": " + provenance.title;
    }
    xml += "  <NOTE>" + xmlEscape(note) + "</NOTE>\n";

    // TRACK_LIST: which creature (or BGM) is on which interleaved channel.
    auto emitTrack = [&](int channel, const std::string &name) {
        const auto ch = std::to_string(channel);
        xml += "    <TRACK>";
        xml += "<CHANNEL_INDEX>" + ch + "</CHANNEL_INDEX>";
        xml += "<NAME>" + xmlEscape(name) + "</NAME>";
        xml += "<INTERLEAVE_INDEX>" + ch + "</INTERLEAVE_INDEX>";
        xml += "</TRACK>\n";
    };

    if (totalChannels > 0) {
        // Complete, contiguous list for a poly WAV: one TRACK per interleaved
        // channel with TRACK_COUNT == channel count. A sparse list (only the used
        // lanes) with a mismatched count is ignored by Wave Agent / DAWs.
        std::vector<std::string> nameByChannel(static_cast<std::size_t>(totalChannels));
        for (const auto &t : provenance.tracks) {
            if (t.channel >= 1 && t.channel <= totalChannels) {
                nameByChannel[static_cast<std::size_t>(t.channel - 1)] = t.name;
            }
        }
        xml += "  <TRACK_LIST>\n";
        xml += "    <TRACK_COUNT>" + std::to_string(totalChannels) + "</TRACK_COUNT>\n";
        for (int c = 1; c <= totalChannels; ++c) {
            emitTrack(c, nameByChannel[static_cast<std::size_t>(c - 1)]);
        }
        xml += "  </TRACK_LIST>\n";
    } else if (!provenance.tracks.empty()) {
        // Verbatim sparse list (used when the caller doesn't specify a channel
        // count). A mono export clears tracks, so it lands here with none.
        xml += "  <TRACK_LIST>\n";
        xml += "    <TRACK_COUNT>" + std::to_string(provenance.tracks.size()) + "</TRACK_COUNT>\n";
        for (const auto &t : provenance.tracks) {
            emitTrack(t.channel, t.name);
        }
        xml += "  </TRACK_LIST>\n";
    }

    // USER: the private provenance block — script id, title, generations, full text.
    xml += "  <USER>\n";
    xml += "    <SOURCE_SCRIPT_ID>" + xmlEscape(provenance.sourceScriptId) + "</SOURCE_SCRIPT_ID>\n";
    xml += "    <TITLE>" + xmlEscape(provenance.title) + "</TITLE>\n";
    xml += "    <GENERATION_IDS>" + xmlEscape(joinGenerationIds(provenance.generationIds)) + "</GENERATION_IDS>\n";

    std::string script;
    for (std::size_t i = 0; i < provenance.script.size(); ++i) {
        if (i > 0) {
            script += '\n';
        }
        script += provenance.script[i].speaker + ": " + provenance.script[i].text;
    }
    xml += "    <DIALOG_SCRIPT>" + xmlEscape(script) + "</DIALOG_SCRIPT>\n";

    // One <TRACK> row for a timed-token section (LIPSYNC / WORD_ALIGNMENT): the
    // channel + name plus a compact packed payload built by the shared codec, so
    // both sections use one packer and one row shape and can't drift.
    const auto emitTrackRow = [&](const char *payloadTag, uint16_t channel, const std::string &name,
                                  const std::vector<TimedToken> &tokens) {
        xml += "      <TRACK><CHANNEL_INDEX>" + std::to_string(channel) + "</CHANNEL_INDEX>";
        xml += "<NAME>" + xmlEscape(name) + "</NAME>";
        xml += "<" + std::string(payloadTag) + ">" + xmlEscape(packTimedTokens(tokens)) + "</" +
               std::string(payloadTag) + ">";
        xml += "</TRACK>\n";
    };

    // LIPSYNC: per-creature mouth cues (derived from ElevenLabs alignment, #53).
    // A private block only creature-console reads.
    if (!provenance.lipsync.empty()) {
        xml += "    <LIPSYNC>\n";
        for (const auto &lt : provenance.lipsync) {
            std::vector<TimedToken> tokens;
            tokens.reserve(lt.cues.size());
            for (const auto &c : lt.cues) {
                tokens.push_back({c.start, c.end, c.shape});
            }
            emitTrackRow("CUES", lt.channel, lt.name, tokens);
        }
        xml += "    </LIPSYNC>\n";
    }

    // WORD_ALIGNMENT: per-creature word timings (issue #56, Part 2), on the same
    // tightened timeline as the mouth cues — the console reads it for
    // word-at-timestamp lookups on the mouth-axis waveform hover.
    if (!provenance.wordAlignment.empty()) {
        xml += "    <WORD_ALIGNMENT>\n";
        for (const auto &wt : provenance.wordAlignment) {
            std::vector<TimedToken> tokens;
            tokens.reserve(wt.words.size());
            for (const auto &w : wt.words) {
                tokens.push_back({w.start, w.end, w.word});
            }
            emitTrackRow("WORDS", wt.channel, wt.name, tokens);
        }
        xml += "    </WORD_ALIGNMENT>\n";
    }

    xml += "  </USER>\n";

    xml += "</BWFXML>\n";
    return xml;
}

std::vector<uint8_t> makeIxmlChunk(const std::string &ixmlDocument) {
    const uint32_t payloadSize = static_cast<uint32_t>(ixmlDocument.size());
    const bool pad = (payloadSize % 2) == 1;

    std::vector<uint8_t> chunk;
    chunk.reserve(8 + payloadSize + (pad ? 1 : 0));

    // Chunk id.
    chunk.insert(chunk.end(), {'i', 'X', 'M', 'L'});
    // Little-endian 32-bit payload size (does NOT include the pad byte, per RIFF).
    chunk.push_back(static_cast<uint8_t>(payloadSize & 0xFF));
    chunk.push_back(static_cast<uint8_t>((payloadSize >> 8) & 0xFF));
    chunk.push_back(static_cast<uint8_t>((payloadSize >> 16) & 0xFF));
    chunk.push_back(static_cast<uint8_t>((payloadSize >> 24) & 0xFF));
    // Payload.
    chunk.insert(chunk.end(), ixmlDocument.begin(), ixmlDocument.end());
    // Word-align.
    if (pad) {
        chunk.push_back(0);
    }
    return chunk;
}

} // namespace creatures::voice
