#pragma once

#include <string>
#include <string_view>

#include "server/config.h"

namespace creatures::voice {

/// Built-in default for speech where quality and audio tags matter more than
/// latency: multi-character dialog and pre-rendered voice files (issue #220). v4
/// performs inline tags like [laughs], and its text-to-dialogue timestamps track
/// forced alignment to ~40 ms where eleven_v3's were off by most of a second.
///
/// Prefer expressiveModelId() over this: the model is configurable (issue #244),
/// and this is only where it lands when nothing says otherwise.
inline constexpr const char *kDefaultExpressiveModelId = DEFAULT_EXPRESSIVE_VOICE_MODEL;

/// Model to suggest in errors when a creature's model can't serve ad-hoc speech.
inline constexpr const char *kRecommendedAdHocModelId = "eleven_flash_v2_5";

/// True for models that act out inline audio tags instead of reading them aloud.
inline bool supportsAudioTags(std::string_view modelId) {
    return modelId.rfind("eleven_v3", 0) == 0 || modelId.rfind("eleven_v4", 0) == 0;
}

/// Ad-hoc speech is latency-bound, so it needs one of the fast models. The v3
/// and v4 families take ~3x longer to render a sentence, and the TTS stream-input
/// WebSocket rejects them outright (HTTP 400 for v4, "use the text-to-dialogue
/// websocket"). The older multilingual/monolingual models don't stream either.
inline bool supportsAdHocSpeech(std::string_view modelId) {
    if (supportsAudioTags(modelId))
        return false;
    return modelId != "eleven_multilingual_v2" && modelId != "eleven_monolingual_v1" &&
           modelId != "eleven_multilingual_v1";
}

/// The configured model for dialog and pre-rendered voice files.
///
/// Falls back to the built-in default when there is no Configuration, which is
/// the normal case in the test binary — computeCacheKey() alone is called from
/// a couple of dozen tests that never stand one up.
///
/// Out of line on purpose: reaching the global Configuration from this header
/// would put an `extern creatures::config` into every file that includes it,
/// which shadows CommandLine.cpp's own local `config`.
std::string expressiveModelId();

} // namespace creatures::voice
