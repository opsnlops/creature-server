#include "server/ws/service/SoundRenditionService.h"

#include "server/audio/MonoWavDownmixer.h"
#include "server/audio/Mp3Writer.h"
#include "server/audio/OggOpusWriter.h"
#include "server/voice/IxmlReader.h"

namespace creatures::ws {

SoundRenditionService::Comments
SoundRenditionService::provenanceTags(const creatures::voice::WavProvenance &provenance) {
    Comments comments;
    const auto add = [&](const char *key, const std::string &value) {
        if (!value.empty()) {
            comments.emplace_back(key, value);
        }
    };
    add("TITLE", provenance.title);
    add("SOURCE_SCRIPT_ID", provenance.sourceScriptId);
    if (!provenance.script.empty()) {
        std::string script;
        for (const auto &line : provenance.script) {
            if (!script.empty())
                script += '\n';
            script += line.speaker + ": " + line.text;
        }
        add("DESCRIPTION", script);
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
                                                                   SoundRenditionFormat format) const {
    auto mono = creatures::audio::loadWavAsMono(wavPath.string());
    if (!mono.isSuccess()) {
        return creatures::Result<SoundRendition>{mono.getError().value()};
    }
    creatures::voice::WavProvenance provenance;
    if (const auto ixml = creatures::voice::readIxmlChunk(wavPath)) {
        provenance = creatures::voice::parseIxmlProvenance(*ixml);
    }
    const auto value = mono.getValue().value();
    return renderMonoPcm(value.samples, value.sampleRate, provenance, format);
}

} // namespace creatures::ws
