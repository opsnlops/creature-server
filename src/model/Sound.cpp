#include "model/Sound.h"

namespace creatures {

nlohmann::json soundToJson(const Sound &sound) {
    nlohmann::json result{{"file_name", sound.fileName},
                          {"size", sound.size},
                          {"transcript", sound.transcript},
                          {"lipsync", sound.lipsync},
                          {"title", sound.title},
                          {"source_script_id", sound.sourceScriptId},
                          {"has_embedded_script", sound.hasEmbeddedScript},
                          {"script", sound.script},
                          {"generation_ids", sound.generationIds},
                          {"has_embedded_lipsync", sound.hasEmbeddedLipsync}};
    if (!sound.scriptTurns.empty()) {
        result["script_turns"] = nlohmann::json::array();
        for (const auto &turn : sound.scriptTurns)
            result["script_turns"].push_back({{"speaker", turn.speaker}, {"line", turn.text}});
    }
    if (!sound.tracks.empty()) {
        result["tracks"] = nlohmann::json::array();
        for (const auto &track : sound.tracks)
            result["tracks"].push_back({{"channel", track.channel}, {"creature_name", track.name}});
    }
    if (!sound.lipsyncTracks.empty()) {
        result["mouth_cues"] = nlohmann::json::array();
        for (const auto &track : sound.lipsyncTracks) {
            nlohmann::json cues = nlohmann::json::array();
            for (const auto &cue : track.cues)
                cues.push_back({{"start_s", cue.start}, {"end_s", cue.end}, {"shape", cue.shape}});
            result["mouth_cues"].push_back(
                {{"channel", track.channel}, {"creature_name", track.name}, {"cues", std::move(cues)}});
        }
    }
    if (!sound.wordTracks.empty()) {
        result["word_timings"] = nlohmann::json::array();
        for (const auto &track : sound.wordTracks) {
            nlohmann::json words = nlohmann::json::array();
            for (const auto &word : track.words)
                words.push_back({{"word", word.word}, {"start_s", word.start}, {"end_s", word.end}});
            result["word_timings"].push_back(
                {{"channel", track.channel}, {"creature_name", track.name}, {"words", std::move(words)}});
        }
    }
    return result;
}

} // namespace creatures
