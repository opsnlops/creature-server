

#include <string>

#include "Sound.h"

namespace creatures {

Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto) {
    Sound sound;
    sound.fileName = soundDto->file_name;
    sound.size = soundDto->size;
    sound.transcript = soundDto->transcript;
    sound.lipsync = soundDto->lipsync;
    sound.title = soundDto->title ? std::string(soundDto->title) : std::string();
    sound.sourceScriptId = soundDto->source_script_id ? std::string(soundDto->source_script_id) : std::string();
    sound.script = soundDto->script ? std::string(soundDto->script) : std::string();
    sound.generationIds = soundDto->generation_ids ? std::string(soundDto->generation_ids) : std::string();
    sound.hasEmbeddedScript = soundDto->has_embedded_script.getValue(false);
    sound.hasEmbeddedLipsync = soundDto->has_embedded_lipsync.getValue(false);

    // Structured dialog metadata (issue #56) — round-trip the DTO lists back into the
    // domain struct so the conversion is symmetric with convertSoundToDto. Each list
    // is optional and only present on a per-sound metadata payload.
    if (soundDto->script_turns) {
        for (const auto &turn : *soundDto->script_turns) {
            if (turn) {
                sound.scriptTurns.push_back({turn->speaker ? std::string(turn->speaker) : std::string(),
                                             turn->line ? std::string(turn->line) : std::string()});
            }
        }
    }
    if (soundDto->tracks) {
        for (const auto &track : *soundDto->tracks) {
            if (track) {
                sound.tracks.push_back({track->channel.getValue(0),
                                        track->creature_name ? std::string(track->creature_name) : std::string()});
            }
        }
    }
    if (soundDto->mouth_cues) {
        for (const auto &track : *soundDto->mouth_cues) {
            if (!track) {
                continue;
            }
            creatures::voice::DialogLipsyncTrack lt;
            lt.channel = track->channel.getValue(0);
            lt.name = track->creature_name ? std::string(track->creature_name) : std::string();
            if (track->cues) {
                for (const auto &cue : *track->cues) {
                    if (cue) {
                        lt.cues.push_back({cue->start_s.getValue(0.0), cue->end_s.getValue(0.0),
                                           cue->shape ? std::string(cue->shape) : std::string()});
                    }
                }
            }
            sound.lipsyncTracks.push_back(std::move(lt));
        }
    }
    if (soundDto->word_timings) {
        for (const auto &track : *soundDto->word_timings) {
            if (!track) {
                continue;
            }
            creatures::voice::DialogWordTrack wt;
            wt.channel = track->channel.getValue(0);
            wt.name = track->creature_name ? std::string(track->creature_name) : std::string();
            if (track->words) {
                for (const auto &word : *track->words) {
                    if (word) {
                        wt.words.push_back({word->word ? std::string(word->word) : std::string(),
                                            word->start_s.getValue(0.0), word->end_s.getValue(0.0)});
                    }
                }
            }
            sound.wordTracks.push_back(std::move(wt));
        }
    }

    return sound;
}

oatpp::Object<SoundDto> convertSoundToDto(const Sound &sound) {
    auto soundDto = SoundDto::createShared();
    soundDto->file_name = sound.fileName;
    soundDto->size = sound.size;
    soundDto->transcript = sound.transcript;
    soundDto->lipsync = sound.lipsync;
    soundDto->title = sound.title;
    soundDto->source_script_id = sound.sourceScriptId;
    soundDto->script = sound.script;
    soundDto->generation_ids = sound.generationIds;
    soundDto->has_embedded_script = sound.hasEmbeddedScript;
    soundDto->has_embedded_lipsync = sound.hasEmbeddedLipsync;

    // Structured dialog metadata (issue #56) — only materialized when the file
    // carried the corresponding embedded iXML, so plain sounds stay null/empty.
    if (!sound.scriptTurns.empty()) {
        auto turns = oatpp::List<oatpp::Object<DialogTurnDto>>::createShared();
        for (const auto &turn : sound.scriptTurns) {
            auto dto = DialogTurnDto::createShared();
            dto->speaker = turn.speaker;
            dto->line = turn.text;
            turns->push_back(dto);
        }
        soundDto->script_turns = turns;
    }

    if (!sound.tracks.empty()) {
        auto tracks = oatpp::List<oatpp::Object<SoundTrackDto>>::createShared();
        for (const auto &track : sound.tracks) {
            auto dto = SoundTrackDto::createShared();
            dto->channel = track.channel;
            dto->creature_name = track.name;
            tracks->push_back(dto);
        }
        soundDto->tracks = tracks;
    }

    if (!sound.lipsyncTracks.empty()) {
        auto mouthCues = oatpp::List<oatpp::Object<TrackMouthCuesDto>>::createShared();
        for (const auto &lt : sound.lipsyncTracks) {
            auto trackDto = TrackMouthCuesDto::createShared();
            trackDto->channel = lt.channel;
            trackDto->creature_name = lt.name;
            auto cues = oatpp::List<oatpp::Object<MouthCueDto>>::createShared();
            for (const auto &cue : lt.cues) {
                auto cueDto = MouthCueDto::createShared();
                cueDto->start_s = cue.start;
                cueDto->end_s = cue.end;
                cueDto->shape = cue.shape;
                cues->push_back(cueDto);
            }
            trackDto->cues = cues;
            mouthCues->push_back(trackDto);
        }
        soundDto->mouth_cues = mouthCues;
    }

    if (!sound.wordTracks.empty()) {
        auto wordTimings = oatpp::List<oatpp::Object<TrackWordsDto>>::createShared();
        for (const auto &wt : sound.wordTracks) {
            auto trackDto = TrackWordsDto::createShared();
            trackDto->channel = wt.channel;
            trackDto->creature_name = wt.name;
            auto words = oatpp::List<oatpp::Object<WordTimingDto>>::createShared();
            for (const auto &word : wt.words) {
                auto wordDto = WordTimingDto::createShared();
                wordDto->word = word.word;
                wordDto->start_s = word.start;
                wordDto->end_s = word.end;
                words->push_back(wordDto);
            }
            trackDto->words = words;
            wordTimings->push_back(trackDto);
        }
        soundDto->word_timings = wordTimings;
    }

    return soundDto;
}

} // namespace creatures