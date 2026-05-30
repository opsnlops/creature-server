#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/// One turn in a multi-character dialog scene: which creature speaks, and what
/// they say. The text may contain ElevenLabs inline audio tags like `[giggles]`
/// or `[whispering]`; they're stripped before forced alignment and re-applied
/// only as expressive hints to the dialog model.
class DialogTurnDto : public oatpp::DTO {

    DTO_INIT(DialogTurnDto, DTO)

    DTO_FIELD_INFO(creature_id) {
        info->description = "The UUID of the creature speaking this turn.";
        info->required = true;
    }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(text) {
        info->description = "What the creature says. May contain inline ElevenLabs audio tags like [giggles] for "
                            "expressive delivery; tags are removed from the spoken text but kept in the model input.";
        info->required = true;
    }
    DTO_FIELD(String, text);
};

/// POST /api/v1/animation/dialog request body.
///
/// Submits a multi-character dialog scene. The server validates well-formedness
/// synchronously and returns 202 with a job_id; the rest of the work runs in a
/// background worker. Clients receive progress + completion updates over the
/// existing WebSocket job-progress stream (filter by job_id).
class DialogRequestDto : public oatpp::DTO {

    DTO_INIT(DialogRequestDto, DTO)

    DTO_FIELD_INFO(turns) {
        info->description = "The ordered list of turns in the scene. Order matters — it's both the speaking order "
                            "and the order ElevenLabs uses for cross-speaker reactivity.";
        info->required = true;
    }
    DTO_FIELD(List<Object<DialogTurnDto>>, turns);

    DTO_FIELD_INFO(persistence) {
        info->description = "Where the assembled animation gets stored. 'adhoc' = expiring TTL collection (good for "
                            "reactive use where the scene won't be needed again). 'permanent' = normal animations "
                            "collection (good for show-prep, e.g. a scripted scene for a wedding).";
        info->required = true;
    }
    DTO_FIELD(String, persistence);

    DTO_FIELD_INFO(autoplay) {
        info->description = "If true, the server also calls SessionManager::interrupt() to play the assembled "
                            "animation immediately on the creatures' shared universe. Requires all creatures in the "
                            "scene to be registered with the controller on the same universe. Defaults to false.";
        info->required = false;
    }
    DTO_FIELD(Boolean, autoplay);

    DTO_FIELD_INFO(title) {
        info->description = "Human-readable scene title. Stored in animation metadata. Defaults to 'Dialog {job_id}' "
                            "if omitted.";
        info->required = false;
    }
    DTO_FIELD(String, title);
};

/// Result payload embedded in JobState::result when a dialog job completes
/// successfully. Sent over the WebSocket job-complete message as a JSON string
/// that clients can deserialize into this same DTO.
class DialogJobResultDto : public oatpp::DTO {

    DTO_INIT(DialogJobResultDto, DTO)

    DTO_FIELD_INFO(animation_id) {
        info->description = "UUID of the assembled multi-track Animation. Use the standard animation endpoints to "
                            "fetch or play it.";
    }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(number_of_frames) { info->description = "Total frames in every track of the assembled animation."; }
    DTO_FIELD(UInt32, number_of_frames);

    DTO_FIELD_INFO(milliseconds_per_frame) {
        info->description = "Frame rate of the assembled animation (typically 20 ms, but pulled from the chosen "
                            "speech_loop base animation per creature).";
    }
    DTO_FIELD(UInt32, milliseconds_per_frame);

    DTO_FIELD_INFO(duration_seconds) {
        info->description = "Total scene duration in seconds (number_of_frames * milliseconds_per_frame / 1000).";
    }
    DTO_FIELD(Float64, duration_seconds);

    DTO_FIELD_INFO(persistence) {
        info->description = "Which animations collection the scene was stored in ('adhoc' or 'permanent') — echoes "
                            "back what the request asked for.";
    }
    DTO_FIELD(String, persistence);

    DTO_FIELD_INFO(autoplayed) {
        info->description = "True if the server also called SessionManager::interrupt() to play the scene immediately. "
                            "False either because autoplay was not requested, or because the autoplay call failed "
                            "after the animation was successfully persisted.";
    }
    DTO_FIELD(Boolean, autoplayed);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
