#pragma once

#include <string>

#include "server/voice/DialogClient.h"

namespace creatures::voice {

/**
 * The two forms one spoken turn's text takes.
 *
 * They differ only when the provider performs inline audio tags. A model that
 * acts out `[sighs]` has to be handed it; one that would read it aloud must
 * not be. Either way the server records the spoken words alone, so the
 * transcript, the animation title and the exchange provenance never quote a
 * cue nobody said.
 */
struct TurnText {
    std::string forProvider; ///< what ElevenLabs is asked to say
    std::string forRecord;   ///< transcript, turn list, animation title, provenance
};

/**
 * Split a turn's text as written into what the provider hears and what the
 * server remembers.
 *
 * Free and inline so the policy is one named thing that can be tested without
 * standing up a session, the way the alignment parser is. Getting it wrong is
 * silent in both directions: a fast model says "sighs" out loud, and a
 * tag-capable one never sighs at all (issue #253).
 *
 * `forProvider` is the line exactly as written, tags and all. Only the record
 * is normalized, because only the record is compared, excerpted and slugified.
 */
inline TurnText splitTurnText(const std::string &rawText, const bool performsAudioTags) {
    TurnText split;
    split.forRecord = DialogClient::stripTags(rawText);
    split.forProvider = performsAudioTags ? rawText : split.forRecord;
    return split;
}

} // namespace creatures::voice
