
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

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
};

#include OATPP_CODEGEN_BEGIN(DTO)

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
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<SoundDto> convertSoundToDto(const Sound &sound);
Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto);

} // namespace creatures
