
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/voice/IxmlWriter.h" // structured dialog metadata parsed from the embedded iXML (#56)

namespace creatures {

struct Sound {
    std::string fileName;
    uint32_t size = 0;
    std::string transcript;
    std::string lipsync;
    // Embedded iXML provenance (dialog renders). Empty / false for plain sounds.
    std::string title;               // human scene title, for display instead of a UUID
    std::string sourceScriptId;      // links back to the dialog script, if any
    std::string script;              // the full readable dialog text, if embedded
    std::string generationIds;       // comma-separated ElevenLabs generation ids, if embedded
    bool hasEmbeddedScript = false;  // true when the file carries iXML script text
    bool hasEmbeddedLipsync = false; // true when the file carries iXML lip-sync (mouth cues)

    // Structured dialog metadata, parsed from the embedded iXML "when we have it"
    // (#56). All empty for plain sounds and non-dialog WAVs. `wordTracks` also stays
    // empty for renders made before the pipeline persists word timings (Part 2).
    std::vector<creatures::voice::DialogScriptLine> scriptTurns;     // ordered [{speaker, text}]
    std::vector<creatures::voice::DialogTrackInfo> tracks;           // [{channel, name}]
    std::vector<creatures::voice::DialogLipsyncTrack> lipsyncTracks; // per-track mouth cues
    std::vector<creatures::voice::DialogWordTrack> wordTracks;       // per-track word timings
};

#include OATPP_CODEGEN_BEGIN(DTO)

/// One turn of the rendered dialog script (issue #56). Structured form of the
/// `DIALOG_SCRIPT` blob.
class DialogTurnDto : public oatpp::DTO {
    DTO_INIT(DialogTurnDto, DTO)
    DTO_FIELD_INFO(speaker) { info->description = "Speaker (resolved creature name) for this turn."; }
    DTO_FIELD(String, speaker);
    DTO_FIELD_INFO(line) { info->description = "The spoken line for this turn."; }
    DTO_FIELD(String, line);
};

/// One interleaved track: which creature (or BGM) is on which 1-based channel (#56).
class SoundTrackDto : public oatpp::DTO {
    DTO_INIT(SoundTrackDto, DTO)
    DTO_FIELD_INFO(channel) { info->description = "1-based interleaved audio channel (BGM is channel 17)."; }
    DTO_FIELD(UInt16, channel);
    DTO_FIELD_INFO(creature_name) { info->description = "Creature name on this channel, or 'BGM' for the music lane."; }
    DTO_FIELD(String, creature_name);
};

/// One mouth cue: a mouth shape held over a time span (#56).
class MouthCueDto : public oatpp::DTO {
    DTO_INIT(MouthCueDto, DTO)
    DTO_FIELD(Float64, start_s);
    DTO_FIELD(Float64, end_s);
    DTO_FIELD_INFO(shape) { info->description = "Rhubarb mouth shape letter (A–H, X)."; }
    DTO_FIELD(String, shape);
};

/// Per-creature mouth cues for one channel (#56).
class TrackMouthCuesDto : public oatpp::DTO {
    DTO_INIT(TrackMouthCuesDto, DTO)
    DTO_FIELD(UInt16, channel);
    DTO_FIELD(String, creature_name);
    DTO_FIELD(List<Object<MouthCueDto>>, cues);
};

/// One word with its start/end timing in seconds (#56, Part 2).
class WordTimingDto : public oatpp::DTO {
    DTO_INIT(WordTimingDto, DTO)
    DTO_FIELD(String, word);
    DTO_FIELD(Float64, start_s);
    DTO_FIELD(Float64, end_s);
};

/// Per-creature word-level alignment for one channel (#56, Part 2).
class TrackWordsDto : public oatpp::DTO {
    DTO_INIT(TrackWordsDto, DTO)
    DTO_FIELD(UInt16, channel);
    DTO_FIELD(String, creature_name);
    DTO_FIELD(List<Object<WordTimingDto>>, words);
};

class SoundDto : public oatpp::DTO {

    DTO_INIT(SoundDto, DTO /* extends */)

    DTO_FIELD_INFO(file_name) {
        info->description = "The name of the sound file on the file system";
        info->required = true;
    }
    DTO_FIELD(String, file_name);

    DTO_FIELD_INFO(size) {
        info->description = "The size of the sound file in bytes";
        info->required = true;
    }
    DTO_FIELD(UInt32, size);

    DTO_FIELD_INFO(transcript) {
        info->description = "The file name of the sound file's transcript, if it has one";
        info->required = false;
    }
    DTO_FIELD(String, transcript);

    DTO_FIELD_INFO(lipsync) {
        info->description = "The file name of the sound file's lipsync data, if it has one";
        info->required = false;
    }
    DTO_FIELD(String, lipsync);

    DTO_FIELD_INFO(title) {
        info->description = "Human scene title embedded in the file's provenance (dialog renders), for display "
                            "instead of a UUID filename. Empty if the file has none.";
        info->required = false;
    }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(source_script_id) {
        info->description = "The dialog script this render came from, embedded in the file's provenance. Empty if "
                            "the file has none.";
        info->required = false;
    }
    DTO_FIELD(String, source_script_id);

    DTO_FIELD_INFO(has_embedded_script) {
        info->description = "True when the file carries embedded (iXML) script text — the readable dialog it was "
                            "rendered from.";
        info->required = false;
    }
    DTO_FIELD(Boolean, has_embedded_script);

    DTO_FIELD_INFO(script) {
        info->description = "The full readable dialog text embedded in the file's provenance ('Speaker: line' per "
                            "turn), if any.";
        info->required = false;
    }
    DTO_FIELD(String, script);

    DTO_FIELD_INFO(generation_ids) {
        info->description = "Comma-separated ElevenLabs generation ids embedded in the file's provenance, if any.";
        info->required = false;
    }
    DTO_FIELD(String, generation_ids);

    DTO_FIELD_INFO(has_embedded_lipsync) {
        info->description = "True when the file carries embedded (iXML) lip-sync — per-creature mouth cues derived "
                            "from the ElevenLabs alignment.";
        info->required = false;
    }
    DTO_FIELD(Boolean, has_embedded_lipsync);

    // Structured dialog metadata (issue #56), populated from the embedded iXML when present.
    // Empty for plain sounds and old renders — the client degrades gracefully.
    DTO_FIELD_INFO(script_turns) {
        info->description = "The dialog script as structured turns [{speaker, line}], from the embedded iXML. Empty "
                            "if the file carries no embedded script.";
        info->required = false;
    }
    DTO_FIELD(List<Object<DialogTurnDto>>, script_turns);

    DTO_FIELD_INFO(tracks) {
        info->description = "Interleaved track list [{channel, creature_name}] — which creature (or BGM) is on which "
                            "1-based channel. Empty if the file carries no embedded track list.";
        info->required = false;
    }
    DTO_FIELD(List<Object<SoundTrackDto>>, tracks);

    DTO_FIELD_INFO(mouth_cues) {
        info->description = "Per-track mouth cues [{channel, creature_name, cues:[{start_s, end_s, shape}]}] from the "
                            "embedded lip-sync. Empty if the file carries no embedded lip-sync.";
        info->required = false;
    }
    DTO_FIELD(List<Object<TrackMouthCuesDto>>, mouth_cues);

    DTO_FIELD_INFO(word_timings) {
        info->description = "Per-track word-level alignment [{channel, creature_name, words:[{word, start_s, end_s}]}] "
                            "for word-at-timestamp lookups. Empty until a render persists word timings (issue #56, "
                            "Part 2); old renders carry none.";
        info->required = false;
    }
    DTO_FIELD(List<Object<TrackWordsDto>>, word_timings);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<SoundDto> convertSoundToDto(const Sound &sound);
Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto);

} // namespace creatures
