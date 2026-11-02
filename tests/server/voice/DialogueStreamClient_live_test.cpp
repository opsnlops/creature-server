#include <cstdlib>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

#include "server/voice/DialogueStreamClient.h"

namespace creatures::voice {

// A LIVE gate. Everything else about the dialogue socket is tested without a
// network — the protocol was worked out against the real API in #247 and the
// alignment parsing is covered in DialogueWire_test — but none of that
// exercises THIS code path talking to ElevenLabs. The things that can only
// break here are the ones Python could not tell us: whether IXWebSocket
// delivers the xi-api-key header the way the socket wants, whether the
// handoff between its thread and the caller actually settles, and whether
// turn-then-flush ordering holds over sendUtf8Text.
//
// That matters because the session path has no fallback by design (#248), so a
// bug here is every ad-hoc turn failing rather than a quiet degradation.
//
// Skipped unless explicitly opted into, so CI never reaches the network:
//
//   CREATURE_LIVE_ELEVENLABS=1 VOICE_API_KEY=... \
//       ./creature-server-test --gtest_filter='DialogueStreamClientLive.*'
//
// Costs a couple of ElevenLabs renders per run.

namespace {

/// Beaky's actual voice (Gigi), confirmed against the creature export. Override
/// with CREATURE_LIVE_VOICE_ID to point at another one.
constexpr const char *kDefaultVoiceId = "n7Wi4g1bhpw4Bs8HK5ph";

/// Returns the API key when the gate is open, or an empty string to skip.
std::string liveKeyOrEmpty() {
    const char *gate = std::getenv("CREATURE_LIVE_ELEVENLABS");
    const char *key = std::getenv("VOICE_API_KEY");
    if (gate == nullptr || std::string(gate) != "1") {
        return {};
    }
    if (key == nullptr || *key == '\0') {
        return {};
    }
    return std::string(key);
}

std::string liveVoiceId() {
    const char *voiceId = std::getenv("CREATURE_LIVE_VOICE_ID");
    return (voiceId != nullptr && *voiceId != '\0') ? std::string(voiceId) : std::string(kDefaultVoiceId);
}

} // namespace

TEST(DialogueStreamClientLive, RendersTurnsOverOneWarmSocket) {
    const auto apiKey = liveKeyOrEmpty();
    if (apiKey.empty()) {
        GTEST_SKIP() << "set CREATURE_LIVE_ELEVENLABS=1 and VOICE_API_KEY to run the live dialogue-socket gate";
    }
    const auto voiceId = liveVoiceId();

    DialogueStreamClient client;
    auto connected = client.connect(apiKey, {voiceId}, "eleven_v4_turbo", "pcm_48000");
    ASSERT_TRUE(connected.isSuccess()) << connected.getError()->getMessage();
    EXPECT_TRUE(client.connected());

    // A performed tag, so a human listening to the bytes can tell whether v4
    // acted it out or read it aloud.
    auto first = client.renderTurn(voiceId, "I don't believe it. [sighs] Fine, we'll try again.");
    ASSERT_TRUE(first.isSuccess()) << first.getError()->getMessage();
    const auto firstTurn = first.getValue().value();

    // pcm_48000 is 16-bit mono at 48 kHz, so ~96 kB per second. A sentence this
    // long cannot come back in under a second of audio unless something is
    // truncating it.
    EXPECT_GT(firstTurn.audioData.size(), 96000u) << "less than a second of audio";
    EXPECT_GT(firstTurn.audioDurationSeconds, 1.0);
    EXPECT_EQ(firstTurn.audioFormat, "pcm_48000");

    // The real point of the gate: alignment only arrives if sync_alignment was
    // accepted AND the snake_case dialect matched. Either mistake yields good
    // audio and a motionless beak.
    EXPECT_GT(firstTurn.charTimings.size(), 20u) << "no alignment — lip sync would be dead";
    EXPECT_FALSE(firstTurn.alignmentText.empty());

    // The alignment has to SPAN the audio, not merely exist. This socket sends
    // chunks whose times restart at zero, and concatenating them shipped a
    // version where the alignment covered 11% of the audio — plenty of
    // character timings, all stacked at the start, and birds whose beaks barely
    // moved (issue #252). Counting entries cannot see that; only the span can.
    const double lastStartMs = firstTurn.charTimings.back().startTimeMs;
    const double audioMs = firstTurn.audioDurationSeconds * 1000.0;
    EXPECT_GT(lastStartMs, audioMs * 0.7)
        << "alignment spans only " << (100.0 * lastStartMs / audioMs) << "% of the audio; chunk offsets are wrong";
    EXPECT_LT(lastStartMs, audioMs * 1.1) << "alignment runs past the end of the audio";

    for (std::size_t i = 1; i < firstTurn.charTimings.size(); ++i) {
        ASSERT_GE(firstTurn.charTimings[i].startTimeMs, firstTurn.charTimings[i - 1].startTimeMs)
            << "character timings go backwards at index " << i;
    }

    // What the socket does with the tag's own characters, which decides whether
    // the beak has to be held shut through the sigh (#253). Either shape is
    // handled — charTimingsToMouthCues skips bracketed characters, and a hole
    // in the timeline rests on its own — so this records which one arrived
    // rather than demanding one. The mouth is checked against both in
    // TurnText_test.
    std::cerr << "[ tag wire ] alignment text: " << firstTurn.alignmentText << "\n";
    const bool echoesTheTag = firstTurn.alignmentText.find("[sighs]") != std::string::npos;
    std::cerr << "[ tag wire ] tag characters are " << (echoesTheTag ? "echoed in" : "absorbed before")
              << " the alignment\n";
    if (echoesTheTag) {
        // Where the performance sits, which is the stretch the beak is held
        // shut across. Printed rather than asserted: the beak rests either way,
        // but a span that collapsed to nothing would mean the sigh had moved
        // somewhere the alignment cannot see.
        double tagStartMs = -1.0;
        double tagEndMs = -1.0;
        int depth = 0;
        for (const auto &timing : firstTurn.charTimings) {
            if (timing.character == '[') {
                ++depth;
                if (tagStartMs < 0.0)
                    tagStartMs = timing.startTimeMs;
            }
            if (depth > 0)
                tagEndMs = timing.startTimeMs + timing.durationMs;
            if (timing.character == ']' && depth > 0)
                --depth;
        }
        std::cerr << "[ tag wire ] tag span: " << tagStartMs << " ms -> " << tagEndMs << " ms of " << audioMs
                  << " ms of audio\n";
    }

    // The sigh has to actually take time. A tag that was silently dropped
    // would come back as the two sentences alone, and nothing else in this
    // test can tell the difference.
    const double words = static_cast<double>(firstTurn.alignmentText.size());
    EXPECT_GT(firstTurn.audioDurationSeconds, words * 0.055)
        << "only " << firstTurn.audioDurationSeconds << "s for " << words
        << " characters; the tag looks dropped rather than performed";

    // A second turn on the SAME connection. This is what the whole design rests
    // on: the socket stays warm so later turns keep the earlier context, and
    // the per-turn flush closes each one out on time.
    auto second = client.renderTurn(voiceId, "[laughs] You always say that.");
    ASSERT_TRUE(second.isSuccess()) << second.getError()->getMessage();
    const auto secondTurn = second.getValue().value();
    EXPECT_GT(secondTurn.audioData.size(), 10000u);
    EXPECT_GT(secondTurn.charTimings.size(), 5u);

    client.close();
    EXPECT_FALSE(client.connected());
}

// A voice that was never registered must fail rather than silently render in
// the wrong voice — the socket refuses anything missing from the first message.
TEST(DialogueStreamClientLive, RefusesAnUnregisteredVoice) {
    const auto apiKey = liveKeyOrEmpty();
    if (apiKey.empty()) {
        GTEST_SKIP() << "set CREATURE_LIVE_ELEVENLABS=1 and VOICE_API_KEY to run the live dialogue-socket gate";
    }

    DialogueStreamClient client;
    auto connected = client.connect(apiKey, {liveVoiceId()}, "eleven_v4_turbo", "pcm_48000");
    ASSERT_TRUE(connected.isSuccess()) << connected.getError()->getMessage();

    // Shape-valid, never registered.
    auto turn = client.renderTurn("21m00Tcm4TlvDq8ikWAM", "This voice was never registered.");
    EXPECT_FALSE(turn.isSuccess()) << "an unregistered voice should not render";
    client.close();
}

} // namespace creatures::voice
