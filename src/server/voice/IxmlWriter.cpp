#include "server/voice/IxmlWriter.h"

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
            // XML 1.0 forbids C0 controls other than tab, LF, and CR. Prompt
            // and script text are untrusted UTF-8; retain every non-control
            // byte while replacing illegal controls so DAWs get valid XML.
            if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') {
                out += c;
            } else {
                out += ' ';
            }
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

std::string buildIxml(const WavProvenance &provenance, int totalChannels) {
    std::string xml;
    xml.reserve(512 + provenance.script.size() * 64);

    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<BWFXML>\n";
    xml += "  <IXML_VERSION>2.0</IXML_VERSION>\n";
    xml += "  <PROJECT>creature-server</PROJECT>\n";
    if (!provenance.title.empty()) {
        xml += "  <SCENE>" + xmlEscape(provenance.title) + "</SCENE>\n";
    }
    if (!provenance.take.empty()) {
        xml += "  <TAKE>" + xmlEscape(provenance.take) + "</TAKE>\n";
    }
    if (provenance.circled) {
        xml += std::string("  <CIRCLED>") + (*provenance.circled ? "TRUE" : "FALSE") + "</CIRCLED>\n";
    }
    if (!provenance.fileUid.empty()) {
        xml += "  <FILE_UID>" + xmlEscape(provenance.fileUid) + "</FILE_UID>\n";
    }
    xml += "  <SPEED><FILE_SAMPLE_RATE>48000</FILE_SAMPLE_RATE><AUDIO_BIT_DEPTH>16</AUDIO_BIT_DEPTH></SPEED>\n";

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

    if (provenance.music && !provenance.music->empty()) {
        const auto &music = *provenance.music;
        const auto field = [&](const char *tag, const std::string &value) {
            xml += "      <" + std::string(tag) + ">" + xmlEscape(value) + "</" + std::string(tag) + ">\n";
        };
        xml += "    <MUSIC_PROVENANCE>\n";
        field("PROVIDER", music.provider);
        field("ENDPOINT", music.endpoint);
        field("MODEL_ID", music.modelId);
        field("OUTPUT_FORMAT", music.outputFormat);
        field("SOURCE_CHANNELS", std::to_string(music.sourceChannels));
        field("CHANNEL_TRANSFORM", music.channelTransform);
        field("GENERATION_MODE", music.generationMode);
        field("MUSIC_PROMPT", music.prompt);
        field("SOURCE_DIALOG_DURATION_MS", std::to_string(music.sourceDialogDurationMs));
        field("DURATION_EXTENSION_MS", std::to_string(music.durationExtensionMs));
        field("MUSIC_LENGTH_MS", std::to_string(music.musicLengthMs));
        field("FORCE_INSTRUMENTAL", music.forceInstrumental ? "true" : "false");
        field("REQUEST_JSON", music.requestJson);
        field("RESPONSE_METADATA_JSON", music.responseMetadataJson);
        field("COMPOSITION_PLAN_JSON", music.compositionPlanJson);
        field("SONG_METADATA_JSON", music.songMetadataJson);
        field("SONG_ID", music.songId);
        field("REQUEST_ID", music.requestId);
        field("GENERATED_AT", music.generatedAt);
        field("MUSIC_GENERATION_ID", music.musicGenerationId);
        field("SOURCE_DIALOG_GENERATION_ID", music.sourceDialogGenerationId);
        field("SOURCE_DIALOG_CACHE_KEY", music.sourceDialogCacheKey);
        field("SOURCE_SCRIPT_UPDATED_AT", std::to_string(music.sourceScriptUpdatedAt));
        field("PCM_SHA256", music.pcmSha256);
        xml += "    </MUSIC_PROVENANCE>\n";
    }

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

nlohmann::json wavProvenanceToJson(const WavProvenance &p) {
    nlohmann::json j;
    j["file_uid"] = p.fileUid;
    j["take"] = p.take;
    if (p.circled) {
        j["circled"] = *p.circled;
    }
    j["source_script_id"] = p.sourceScriptId;
    j["title"] = p.title;
    j["generation_ids"] = p.generationIds;

    j["tracks"] = nlohmann::json::array();
    for (const auto &track : p.tracks) {
        j["tracks"].push_back({{"channel", track.channel}, {"name", track.name}});
    }
    j["script"] = nlohmann::json::array();
    for (const auto &line : p.script) {
        j["script"].push_back({{"speaker", line.speaker}, {"text", line.text}});
    }
    j["lipsync"] = nlohmann::json::array();
    for (const auto &track : p.lipsync) {
        nlohmann::json tj{{"channel", track.channel}, {"name", track.name}};
        tj["cues"] = nlohmann::json::array();
        for (const auto &cue : track.cues) {
            tj["cues"].push_back({{"start", cue.start}, {"end", cue.end}, {"shape", cue.shape}});
        }
        j["lipsync"].push_back(std::move(tj));
    }
    j["word_alignment"] = nlohmann::json::array();
    for (const auto &track : p.wordAlignment) {
        nlohmann::json tj{{"channel", track.channel}, {"name", track.name}};
        tj["words"] = nlohmann::json::array();
        for (const auto &word : track.words) {
            tj["words"].push_back({{"word", word.word}, {"start", word.start}, {"end", word.end}});
        }
        j["word_alignment"].push_back(std::move(tj));
    }
    if (p.music) {
        const auto &m = *p.music;
        j["music"] = {{"provider", m.provider},
                      {"endpoint", m.endpoint},
                      {"model_id", m.modelId},
                      {"output_format", m.outputFormat},
                      {"source_channels", m.sourceChannels},
                      {"channel_transform", m.channelTransform},
                      {"generation_mode", m.generationMode},
                      {"prompt", m.prompt},
                      {"source_dialog_duration_ms", m.sourceDialogDurationMs},
                      {"duration_extension_ms", m.durationExtensionMs},
                      {"music_length_ms", m.musicLengthMs},
                      {"force_instrumental", m.forceInstrumental},
                      {"request_json", m.requestJson},
                      {"response_metadata_json", m.responseMetadataJson},
                      {"composition_plan_json", m.compositionPlanJson},
                      {"song_metadata_json", m.songMetadataJson},
                      {"song_id", m.songId},
                      {"request_id", m.requestId},
                      {"generated_at", m.generatedAt},
                      {"music_generation_id", m.musicGenerationId},
                      {"source_dialog_generation_id", m.sourceDialogGenerationId},
                      {"source_dialog_cache_key", m.sourceDialogCacheKey},
                      {"source_script_updated_at", m.sourceScriptUpdatedAt},
                      {"pcm_sha256", m.pcmSha256}};
    }
    return j;
}

WavProvenance wavProvenanceFromJson(const nlohmann::json &j) {
    WavProvenance p;
    if (!j.is_object()) {
        return p;
    }
    p.fileUid = j.value("file_uid", std::string{});
    p.take = j.value("take", std::string{});
    if (j.contains("circled") && j["circled"].is_boolean()) {
        p.circled = j["circled"].get<bool>();
    }
    p.sourceScriptId = j.value("source_script_id", std::string{});
    p.title = j.value("title", std::string{});
    if (j.contains("generation_ids") && j["generation_ids"].is_array()) {
        p.generationIds = j["generation_ids"].get<std::vector<std::string>>();
    }
    if (j.contains("tracks") && j["tracks"].is_array()) {
        for (const auto &track : j["tracks"]) {
            p.tracks.push_back({track.value("channel", uint16_t{0}), track.value("name", std::string{})});
        }
    }
    if (j.contains("script") && j["script"].is_array()) {
        for (const auto &line : j["script"]) {
            p.script.push_back({line.value("speaker", std::string{}), line.value("text", std::string{})});
        }
    }
    if (j.contains("lipsync") && j["lipsync"].is_array()) {
        for (const auto &track : j["lipsync"]) {
            DialogLipsyncTrack item{track.value("channel", uint16_t{0}), track.value("name", std::string{}), {}};
            if (track.contains("cues") && track["cues"].is_array()) {
                for (const auto &cue : track["cues"]) {
                    item.cues.push_back(
                        {cue.value("start", 0.0), cue.value("end", 0.0), cue.value("shape", std::string{})});
                }
            }
            p.lipsync.push_back(std::move(item));
        }
    }
    if (j.contains("word_alignment") && j["word_alignment"].is_array()) {
        for (const auto &track : j["word_alignment"]) {
            DialogWordTrack item{track.value("channel", uint16_t{0}), track.value("name", std::string{}), {}};
            if (track.contains("words") && track["words"].is_array()) {
                for (const auto &word : track["words"]) {
                    item.words.push_back(
                        {word.value("word", std::string{}), word.value("start", 0.0), word.value("end", 0.0)});
                }
            }
            p.wordAlignment.push_back(std::move(item));
        }
    }
    if (j.contains("music") && j["music"].is_object()) {
        const auto &m = j["music"];
        MusicWavProvenance music;
        music.provider = m.value("provider", std::string{});
        music.endpoint = m.value("endpoint", std::string{});
        music.modelId = m.value("model_id", std::string{});
        music.outputFormat = m.value("output_format", std::string{});
        music.sourceChannels = m.value("source_channels", 0);
        music.channelTransform = m.value("channel_transform", std::string{});
        music.generationMode = m.value("generation_mode", std::string{});
        music.prompt = m.value("prompt", std::string{});
        music.sourceDialogDurationMs = m.value("source_dialog_duration_ms", int64_t{0});
        music.durationExtensionMs = m.value("duration_extension_ms", int64_t{0});
        music.musicLengthMs = m.value("music_length_ms", int64_t{0});
        music.forceInstrumental = m.value("force_instrumental", true);
        music.requestJson = m.value("request_json", std::string{});
        music.responseMetadataJson = m.value("response_metadata_json", std::string{});
        music.compositionPlanJson = m.value("composition_plan_json", std::string{});
        music.songMetadataJson = m.value("song_metadata_json", std::string{});
        music.songId = m.value("song_id", std::string{});
        music.requestId = m.value("request_id", std::string{});
        music.generatedAt = m.value("generated_at", std::string{});
        music.musicGenerationId = m.value("music_generation_id", std::string{});
        music.sourceDialogGenerationId = m.value("source_dialog_generation_id", std::string{});
        music.sourceDialogCacheKey = m.value("source_dialog_cache_key", std::string{});
        music.sourceScriptUpdatedAt = m.value("source_script_updated_at", int64_t{0});
        music.pcmSha256 = m.value("pcm_sha256", std::string{});
        p.music = std::move(music);
    }
    return p;
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
