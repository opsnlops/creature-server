#include "server/ws/service/SoundRenditionService.h"

#include <algorithm>

#include "server/audio/MonoWavDownmixer.h"
#include "server/audio/Mp3Writer.h"
#include "server/audio/OggOpusWriter.h"
#include "server/voice/IxmlReader.h"

namespace creatures::ws {

namespace {

// Everything we make comes from the workshop — used for the album and the "band"
// (ALBUMARTIST/TPE2), and as the artist when no performers are identifiable (#148).
constexpr const char *kWorkshopName = "April's Creature Workshop";

// The performing characters, in order of first appearance: script speakers when we have a
// script, otherwise the creature track lanes (skipping the BGM music lane).
std::string performerList(const creatures::voice::WavProvenance &provenance) {
    std::vector<std::string> names;
    const auto addName = [&names](const std::string &name) {
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    };
    for (const auto &line : provenance.script) {
        addName(line.speaker);
    }
    if (names.empty()) {
        for (const auto &track : provenance.tracks) {
            if (track.name != "BGM") {
                addName(track.name);
            }
        }
    }
    std::string joined;
    for (const auto &name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

} // namespace

SoundRenditionService::Comments
SoundRenditionService::provenanceTags(const creatures::voice::WavProvenance &provenance) {
    Comments comments;
    const auto add = [&](const char *key, const std::string &value) {
        if (!value.empty()) {
            comments.emplace_back(key, value);
        }
    };
    add("TITLE", provenance.title);
    // Display metadata so players don't show "Unknown Artist" (#148). The MP3 writer maps
    // these keys onto the standard ID3 frames; the Ogg writer passes them through as
    // standard Vorbis comments.
    const auto performers = performerList(provenance);
    add("ARTIST", performers.empty() ? std::string{kWorkshopName} : performers);
    add("ALBUMARTIST", kWorkshopName);
    add("ALBUM", kWorkshopName);
    add("PUBLISHER", kWorkshopName);
    // Music wins the genre: a dialog with a score is a soundtrack, not plain
    // spoken word (April's call on #148).
    if (provenance.music) {
        add("GENRE", "Soundtrack");
    } else if (!provenance.script.empty()) {
        add("GENRE", "Spoken Word");
    }
    if (provenance.music) {
        const auto &modelId = provenance.music->modelId;
        add("COMPOSER", modelId.empty() ? std::string{"ElevenLabs Music"} : "ElevenLabs Music (" + modelId + ")");
    }
    add("SOURCE_SCRIPT_ID", provenance.sourceScriptId);
    // The rest of the WAV's iXML provenance rides along as TXXX frames / Vorbis
    // comments, so a shared file is fully traceable back to its render (#148).
    add("FILE_UID", provenance.fileUid);
    add("TAKE", provenance.take);
    if (provenance.circled) {
        add("CIRCLED", *provenance.circled ? "true" : "false");
    }
    if (!provenance.generationIds.empty()) {
        std::string ids;
        for (const auto &id : provenance.generationIds) {
            if (!ids.empty())
                ids += ", ";
            ids += id;
        }
        add("GENERATION_IDS", ids);
    }
    if (!provenance.tracks.empty()) {
        std::string trackList;
        for (const auto &track : provenance.tracks) {
            if (!trackList.empty())
                trackList += "; ";
            trackList += std::to_string(track.channel) + ": " + track.name;
        }
        add("TRACK_LIST", trackList);
    }
    if (!provenance.script.empty()) {
        std::string script;
        for (const auto &line : provenance.script) {
            if (!script.empty())
                script += '\n';
            script += line.speaker + ": " + line.text;
        }
        add("DESCRIPTION", script);
        // The transcript doubles as lyrics (USLT in the MP3), so players with a lyrics
        // pane can show the dialog as it plays (#148).
        add("LYRICS", script);
    }
    if (provenance.music) {
        const auto &music = *provenance.music;
        add("MUSIC_PROMPT", music.prompt);
        add("MUSIC_MODEL_ID", music.modelId);
        if (music.sourceChannels > 0) {
            add("MUSIC_SOURCE_CHANNELS", std::to_string(music.sourceChannels));
        }
        add("MUSIC_CHANNEL_TRANSFORM", music.channelTransform);
        add("MUSIC_GENERATION_MODE", music.generationMode);
        add("SOURCE_DIALOG_DURATION_MS", std::to_string(music.sourceDialogDurationMs));
        add("MUSIC_DURATION_EXTENSION_MS", std::to_string(music.durationExtensionMs));
        add("MUSIC_LENGTH_MS", std::to_string(music.musicLengthMs));
        add("MUSIC_GENERATION_ID", music.musicGenerationId);
        add("SOURCE_DIALOG_CACHE_KEY", music.sourceDialogCacheKey);
        if (music.sourceScriptUpdatedAt > 0) {
            add("SOURCE_SCRIPT_UPDATED_AT", std::to_string(music.sourceScriptUpdatedAt));
        }
        add("ELEVENLABS_SONG_ID", music.songId);
        add("ELEVENLABS_REQUEST_ID", music.requestId);
        add("MUSIC_REQUEST_JSON", music.requestJson);
        add("MUSIC_RESPONSE_METADATA_JSON", music.responseMetadataJson);
        add("MUSIC_COMPOSITION_PLAN_JSON", music.compositionPlanJson);
        add("MUSIC_SONG_METADATA_JSON", music.songMetadataJson);
        add("PCM_SHA256", music.pcmSha256);
        if (provenance.script.empty()) {
            add("DESCRIPTION", music.prompt);
        }
    }
    return comments;
}

creatures::Result<SoundRendition>
SoundRenditionService::renderMonoPcm(const std::vector<int16_t> &samples, int sampleRate,
                                     const creatures::voice::WavProvenance &provenance,
                                     SoundRenditionFormat format) const {
    const auto comments = provenanceTags(provenance);
    creatures::Result<std::vector<uint8_t>> encoded =
        format == SoundRenditionFormat::Mp3
            ? creatures::audio::encodeMonoToMp3(samples, sampleRate, creatures::audio::kShareableMp3Bitrate, comments)
            : creatures::audio::encodeMonoToOggOpus(samples, sampleRate, creatures::audio::kShareableOpusBitrate,
                                                    comments);
    if (!encoded.isSuccess()) {
        return creatures::Result<SoundRendition>{encoded.getError().value()};
    }
    SoundRendition rendition;
    rendition.bytes = encoded.getValue().value();
    rendition.mimeType = format == SoundRenditionFormat::Mp3 ? "audio/mpeg" : "audio/ogg";
    rendition.extension = format == SoundRenditionFormat::Mp3 ? ".mp3" : ".ogg";
    return creatures::Result<SoundRendition>{std::move(rendition)};
}

creatures::Result<SoundRendition> SoundRenditionService::renderWav(const std::filesystem::path &wavPath,
                                                                   SoundRenditionFormat format,
                                                                   const TitleProvider &fallbackTitle) const {
    auto mono = creatures::audio::loadWavAsMono(wavPath.string());
    if (!mono.isSuccess()) {
        return creatures::Result<SoundRendition>{mono.getError().value()};
    }
    creatures::voice::WavProvenance provenance;
    if (const auto ixml = creatures::voice::readIxmlChunk(wavPath)) {
        provenance = creatures::voice::parseIxmlProvenance(*ixml);
    }
    if (provenance.title.empty() && fallbackTitle) {
        provenance.title = fallbackTitle();
    }
    const auto value = mono.getValue().value();
    return renderMonoPcm(value.samples, value.sampleRate, provenance, format);
}

} // namespace creatures::ws
