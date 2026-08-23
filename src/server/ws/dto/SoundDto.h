#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Sound.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class DialogTurnDto : public oatpp::DTO {
    DTO_INIT(DialogTurnDto, DTO)

    DTO_FIELD(String, speaker);
    DTO_FIELD(String, line);
};

class SoundTrackDto : public oatpp::DTO {
    DTO_INIT(SoundTrackDto, DTO)

    DTO_FIELD(UInt16, channel);
    DTO_FIELD(String, creature_name);
};

class MouthCueDto : public oatpp::DTO {
    DTO_INIT(MouthCueDto, DTO)

    DTO_FIELD(Float64, start_s);
    DTO_FIELD(Float64, end_s);
    DTO_FIELD(String, shape);
};

class TrackMouthCuesDto : public oatpp::DTO {
    DTO_INIT(TrackMouthCuesDto, DTO)

    DTO_FIELD(UInt16, channel);
    DTO_FIELD(String, creature_name);
    DTO_FIELD(List<Object<MouthCueDto>>, cues);
};

class WordTimingDto : public oatpp::DTO {
    DTO_INIT(WordTimingDto, DTO)

    DTO_FIELD(String, word);
    DTO_FIELD(Float64, start_s);
    DTO_FIELD(Float64, end_s);
};

class TrackWordsDto : public oatpp::DTO {
    DTO_INIT(TrackWordsDto, DTO)

    DTO_FIELD(UInt16, channel);
    DTO_FIELD(String, creature_name);
    DTO_FIELD(List<Object<WordTimingDto>>, words);
};

class SoundDto : public oatpp::DTO {
    DTO_INIT(SoundDto, DTO)

    DTO_FIELD(String, file_name);
    DTO_FIELD(UInt32, size);
    DTO_FIELD(String, transcript);
    DTO_FIELD(String, lipsync);
    DTO_FIELD(String, title);
    DTO_FIELD(String, source_script_id);
    DTO_FIELD(Boolean, has_embedded_script);
    DTO_FIELD(String, script);
    DTO_FIELD(String, generation_ids);
    DTO_FIELD(Boolean, has_embedded_lipsync);
    DTO_FIELD(List<Object<DialogTurnDto>>, script_turns);
    DTO_FIELD(List<Object<SoundTrackDto>>, tracks);
    DTO_FIELD(List<Object<TrackMouthCuesDto>>, mouth_cues);
    DTO_FIELD(List<Object<TrackWordsDto>>, word_timings);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<SoundDto> convertSoundToDto(const Sound &sound);
Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto);

} // namespace creatures
