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

    DTO_FIELD_INFO(generation_id) {
        info->description = "Optional: use a specific cached generation (from a prior /preview call) instead of "
                            "calling ElevenLabs again. If unset, the server auto-reuses the latest cached generation "
                            "for these turns if one exists, or generates fresh otherwise. If set but the cached "
                            "generation has expired (cron-cleaned), the worker logs a warning and regenerates.";
        info->required = false;
    }
    DTO_FIELD(String, generation_id);
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

// ---------------------------------------------------------------------------
// Preview / cache surface — POST /api/v1/animation/dialog/preview and friends.
// ---------------------------------------------------------------------------

/// POST /api/v1/animation/dialog/preview request body.
///
/// Same `turns` shape as the full dialog request. The preview endpoint
/// generates (or reuses a cached take of) the raw dialog audio without
/// committing to a full animation — useful for UI flows where the author
/// wants to iterate on a scene's content before deciding to persist it.
///
/// Cache semantics:
///   - If `generation_id` is set, the server returns that specific cached
///     generation (404 if it's expired). Lets the UI page through past takes.
///   - Else if `regenerate` is true, always create a fresh generation.
///   - Else: return the latest cached generation for these turns; create a
///     fresh one if nothing's cached.
class DialogPreviewRequestDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewRequestDto, DTO)

    DTO_FIELD_INFO(turns) {
        info->description = "The ordered list of turns. Same shape as POST /dialog.";
        info->required = true;
    }
    DTO_FIELD(List<Object<DialogTurnDto>>, turns);

    DTO_FIELD_INFO(generation_id) {
        info->description = "Return THIS specific cached generation rather than the latest. 404 if expired.";
        info->required = false;
    }
    DTO_FIELD(String, generation_id);

    DTO_FIELD_INFO(regenerate) {
        info->description = "If true, ignore any cached take and create a fresh generation. Use this when the author "
                            "wants a different reading of the same input.";
        info->required = false;
    }
    DTO_FIELD(Boolean, regenerate);

    DTO_FIELD_INFO(format) {
        info->description = "'mono' (default) returns the raw single-channel PCM as JSON with audio_base64 + "
                            "voice_segments + forced_alignment for UI playback. 'multichannel' returns the full "
                            "17-channel WAV bytes as audio/wav for downloading into Audacity (or wherever).";
        info->required = false;
    }
    DTO_FIELD(String, format);
};

/// One voice_segments entry on the wire (mirrors creatures::voice::DialogVoiceSegment).
class DialogPreviewVoiceSegmentDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewVoiceSegmentDto, DTO)

    DTO_FIELD_INFO(voice_id) { info->description = "ElevenLabs voice_id speaking this segment."; }
    DTO_FIELD(String, voice_id);

    DTO_FIELD_INFO(character_start_index) {
        info->description = "First character index (inclusive) of this segment in the alignment.characters array.";
    }
    DTO_FIELD(UInt64, character_start_index);

    DTO_FIELD_INFO(character_end_index) {
        info->description = "One-past-last character index of this segment in the alignment.characters array.";
    }
    DTO_FIELD(UInt64, character_end_index);

    DTO_FIELD_INFO(dialog_input_index) {
        info->description = "Index back into the original request's turns[] array (which turn this segment came from).";
    }
    DTO_FIELD(UInt64, dialog_input_index);
};

/// One forced-alignment word.
class DialogPreviewWordTimingDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewWordTimingDto, DTO)

    DTO_FIELD(String, text);
    DTO_FIELD(Float64, start);
    DTO_FIELD(Float64, end);
};

/// One forced-alignment character (single grapheme — includes spaces / punctuation).
class DialogPreviewCharTimingDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewCharTimingDto, DTO)

    DTO_FIELD(String, text);
    DTO_FIELD(Float64, start);
    DTO_FIELD(Float64, end);
};

/// Mono-format preview response (HTTP 200 application/json).
class DialogPreviewMonoResponseDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewMonoResponseDto, DTO)

    DTO_FIELD_INFO(cache_key) { info->description = "sha256(turns) — stable identifier for this exact input."; }
    DTO_FIELD(String, cache_key);

    DTO_FIELD_INFO(generation_id) {
        info->description = "UUID of the specific take being returned. Pass this to POST /dialog (or back to "
                            "/preview) to address this exact take again.";
    }
    DTO_FIELD(String, generation_id);

    DTO_FIELD_INFO(cached) {
        info->description = "True if this generation was served from the cache; false if it was just created by an "
                            "ElevenLabs call.";
    }
    DTO_FIELD(Boolean, cached);

    DTO_FIELD_INFO(audio_base64) {
        info->description = "Mono S16LE PCM @ 48 kHz, base64-encoded. NOT WAV-wrapped — the client should either "
                            "decode + play directly, or wrap with a 44-byte PCM WAV header for an <audio> element.";
    }
    DTO_FIELD(String, audio_base64);

    DTO_FIELD_INFO(audio_format) { info->description = "Always 'pcm_48000' for v1."; }
    DTO_FIELD(String, audio_format);

    DTO_FIELD_INFO(sample_rate) { info->description = "Sample rate in Hz. Always 48000 for v1."; }
    DTO_FIELD(UInt32, sample_rate);

    DTO_FIELD_INFO(duration_seconds) { info->description = "Audio duration (computed from byte count)."; }
    DTO_FIELD(Float64, duration_seconds);

    DTO_FIELD_INFO(voice_segments) {
        info->description = "Speaker → character-range mapping from text-to-dialogue. Character indices are reliable; "
                            "the times are NOT (eleven_v3 returns broken timestamps — use forced_alignment instead).";
    }
    DTO_FIELD(List<Object<DialogPreviewVoiceSegmentDto>>, voice_segments);

    DTO_FIELD_INFO(forced_alignment_words) { info->description = "Real per-word timing in seconds."; }
    DTO_FIELD(List<Object<DialogPreviewWordTimingDto>>, forced_alignment_words);

    DTO_FIELD_INFO(forced_alignment_chars) {
        info->description = "Real per-character timing in seconds (includes spaces between turns).";
    }
    DTO_FIELD(List<Object<DialogPreviewCharTimingDto>>, forced_alignment_chars);

    DTO_FIELD_INFO(forced_alignment_loss) {
        info->description = "Forced-alignment quality score — lower is better. <0.1 is typical on clean v3 dialog.";
    }
    DTO_FIELD(Float64, forced_alignment_loss);
};

/// One entry in the lookup response.
class DialogPreviewGenerationEntryDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewGenerationEntryDto, DTO)

    DTO_FIELD(String, generation_id);
    DTO_FIELD_INFO(created_at) { info->description = "ISO-8601 UTC timestamp."; }
    DTO_FIELD(String, created_at);
};

/// POST /api/v1/animation/dialog/preview/lookup request body — just the turns.
/// Cheap cache check; no audio work is done.
class DialogPreviewLookupRequestDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewLookupRequestDto, DTO)

    DTO_FIELD_INFO(turns) {
        info->description = "Same turns[] shape as the preview/dialog requests.";
        info->required = true;
    }
    DTO_FIELD(List<Object<DialogTurnDto>>, turns);
};

/// Lookup response — what's cached for these turns. Returns HTTP 200 with an
/// empty generations[] is never returned; absence is 404 instead (per design).
class DialogPreviewLookupResponseDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewLookupResponseDto, DTO)

    DTO_FIELD_INFO(cache_key) { info->description = "sha256(turns) — stable identifier for this exact input."; }
    DTO_FIELD(String, cache_key);

    DTO_FIELD_INFO(generations) {
        info->description = "All cached takes for these turns, NEWEST FIRST. Empty list would have been a 404 instead.";
    }
    DTO_FIELD(List<Object<DialogPreviewGenerationEntryDto>>, generations);

    DTO_FIELD_INFO(latest_generation_id) {
        info->description = "Convenience pointer to generations[0].generation_id (the most recent take).";
    }
    DTO_FIELD(String, latest_generation_id);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
