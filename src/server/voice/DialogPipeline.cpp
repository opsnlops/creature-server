#include "DialogPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "server/namespace-stuffs.h"

namespace creatures::voice {

namespace {

/// Split on runs of whitespace and count non-empty tokens. Matches Python's
/// `str.split()` with no argument — the same primitive show.py uses.
std::size_t countWords(const std::string &s) {
    std::size_t n = 0;
    bool inWord = false;
    for (char c : s) {
        const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
        if (!ws && !inWord) {
            ++n;
            inWord = true;
        } else if (ws) {
            inWord = false;
        }
    }
    return n;
}

} // namespace

Result<std::vector<std::vector<DialogInput>>> chunkTurns(const std::vector<DialogInput> &turns, std::size_t maxChars) {
    if (turns.empty()) {
        return Result<std::vector<std::vector<DialogInput>>>{
            ServerError(ServerError::InvalidData, "chunkTurns requires at least one turn")};
    }
    if (maxChars == 0) {
        return Result<std::vector<std::vector<DialogInput>>>{
            ServerError(ServerError::InvalidData, "chunkTurns: maxChars must be > 0")};
    }

    std::vector<std::vector<DialogInput>> chunks;
    std::vector<DialogInput> current;
    std::size_t currentChars = 0;

    for (std::size_t i = 0; i < turns.size(); ++i) {
        const auto &t = turns[i];
        if (t.text.size() > maxChars) {
            // A single turn that exceeds the cap can't be split without losing
            // joint generation — the author needs to break it up themselves.
            return Result<std::vector<std::vector<DialogInput>>>{ServerError(
                ServerError::InvalidData,
                fmt::format("chunkTurns: turn {} is {} chars (max {}); split the text at a sentence boundary", i,
                            t.text.size(), maxChars))};
        }
        if (!current.empty() && currentChars + t.text.size() > maxChars) {
            chunks.push_back(std::move(current));
            current.clear();
            currentChars = 0;
        }
        currentChars += t.text.size();
        current.push_back(t);
    }
    if (!current.empty()) {
        chunks.push_back(std::move(current));
    }
    return chunks;
}

Result<DialogAssembled> assembleChunk(const std::vector<DialogInput> &turns, const DialogResult &dialog,
                                      const ForcedAlignmentResult &alignment, uint32_t sampleRate) {
    if (turns.empty()) {
        return Result<DialogAssembled>{
            ServerError(ServerError::InvalidData, "assembleChunk requires at least one turn")};
    }
    if (dialog.audioData.empty()) {
        return Result<DialogAssembled>{
            ServerError(ServerError::InvalidData, "assembleChunk requires non-empty dialog audio")};
    }
    if (dialog.audioData.size() % 2 != 0) {
        return Result<DialogAssembled>{
            ServerError(ServerError::InvalidData,
                        "assembleChunk: dialog audio size is not 2-byte aligned (mono S16 PCM expected)")};
    }
    if (alignment.words.empty() || alignment.characters.empty()) {
        return Result<DialogAssembled>{
            ServerError(ServerError::InvalidData, "assembleChunk: forced alignment has no words or characters")};
    }
    if (sampleRate == 0) {
        return Result<DialogAssembled>{ServerError(ServerError::InvalidData, "assembleChunk: sampleRate must be > 0")};
    }

    // PCM here is mono S16LE. On macOS / Linux that's the host byte order, so
    // memcpy of the byte buffer into an int16_t vector is the cheapest correct
    // way to get an aligned sample buffer to read from.
    const std::size_t srcSamples = dialog.audioData.size() / 2;
    std::vector<int16_t> src(srcSamples);
    std::memcpy(src.data(), dialog.audioData.data(), dialog.audioData.size());

    const auto srD = static_cast<double>(sampleRate);
    auto secsToSamples = [&](double t) -> std::ptrdiff_t {
        const std::ptrdiff_t r = static_cast<std::ptrdiff_t>(std::llround(t * srD));
        return std::clamp<std::ptrdiff_t>(r, 0, static_cast<std::ptrdiff_t>(srcSamples));
    };

    // ---- Step 1: walk turns; bind each to its FA word range (for time
    // boundaries) and FA char range (for mouth timing). Maintain a voice ↦
    // perCreature-index map as we go (first-seen order).
    struct TurnInfo {
        std::size_t voiceIndex;
        double startSecs;
        double endSecs;
        std::vector<TextToViseme::CharTiming> chars; // pre-shift (still on the ORIGINAL timeline)
        std::vector<DialogWordTiming> words;         // pre-shift words (ORIGINAL timeline, seconds)
    };
    std::vector<TurnInfo> turnInfos;
    turnInfos.reserve(turns.size());

    std::vector<std::string> voiceIds;
    std::unordered_map<std::string, std::size_t> voiceIndex;
    auto indexFor = [&](const std::string &vid) -> std::size_t {
        auto it = voiceIndex.find(vid);
        if (it != voiceIndex.end()) {
            return it->second;
        }
        const std::size_t idx = voiceIds.size();
        voiceIds.push_back(vid);
        voiceIndex.emplace(vid, idx);
        return idx;
    };

    // ElevenLabs forced-alignment returns words[] with the inter-word
    // whitespace tokens INTERLEAVED as separate entries (a 4-word turn comes
    // back as 7 entries: "Beaky," " " "are" " " "you" " " "awake?"). show.py
    // filters those out before consuming; do the same here so the wordCursor
    // math indexes real words.
    std::vector<const ForcedAlignmentWord *> spokenWords;
    spokenWords.reserve(alignment.words.size());
    for (const auto &w : alignment.words) {
        if (countWords(w.text) > 0) { // non-whitespace token
            spokenWords.push_back(&w);
        }
    }

    std::size_t wordCursor = 0;
    std::size_t charCursor = 0;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const auto &turn = turns[i];
        const std::string stripped = DialogClient::stripTags(turn.text);

        const std::size_t k = countWords(stripped);
        if (k == 0) {
            return Result<DialogAssembled>{ServerError(
                ServerError::InvalidData,
                fmt::format("assembleChunk: turn {} has no words after tag stripping: '{}'", i, turn.text))};
        }
        if (wordCursor + k > spokenWords.size()) {
            return Result<DialogAssembled>{ServerError(
                ServerError::InvalidData,
                fmt::format("assembleChunk: forced alignment ran out of words at turn {} (need {} more, have {})", i, k,
                            spokenWords.size() - wordCursor))};
        }
        const std::size_t wordBase = wordCursor;
        const auto &wFirst = *spokenWords[wordCursor];
        const auto &wLast = *spokenWords[wordCursor + k - 1];
        wordCursor += k;

        TurnInfo ti;
        ti.voiceIndex = indexFor(turn.voiceId);
        ti.startSecs = wFirst.startSeconds;
        ti.endSecs = wLast.endSeconds;

        // Keep this turn's words (ORIGINAL-timeline seconds) for the word-alignment
        // block; they get the same tightened-timeline shift as the chars in step 3.
        ti.words.reserve(k);
        for (std::size_t j = 0; j < k; ++j) {
            const auto &w = *spokenWords[wordBase + j];
            ti.words.push_back(DialogWordTiming{w.text, w.startSeconds, w.endSeconds});
        }

        const std::size_t cn = stripped.size();
        if (charCursor + cn > alignment.characters.size()) {
            return Result<DialogAssembled>{
                ServerError(ServerError::InvalidData,
                            fmt::format("assembleChunk: forced alignment ran out of characters at turn {} "
                                        "(need {} more, have {})",
                                        i, cn, alignment.characters.size() - charCursor))};
        }
        ti.chars.reserve(cn);
        for (std::size_t c = 0; c < cn; ++c) {
            const auto &fc = alignment.characters[charCursor + c];
            TextToViseme::CharTiming ct{};
            ct.character = fc.text.empty() ? ' ' : fc.text[0];
            ct.startTimeMs = fc.startSeconds * 1000.0; // shift applied below, in step 2
            ct.durationMs = (fc.endSeconds - fc.startSeconds) * 1000.0;
            ti.chars.push_back(ct);
        }
        charCursor += cn;
        // Skip the single " " separator joining turns in the forced-alignment
        // transcript. show.py's exact rule.
        if (charCursor < alignment.characters.size()) {
            ++charCursor;
        }

        turnInfos.push_back(std::move(ti));
    }

    // Drift checks — warn but don't fail. show.py's same posture: a small
    // off-by-one means the mouth tracks slightly off in one turn, not a
    // pipeline crash.
    if (wordCursor != spokenWords.size()) {
        warn("assembleChunk: word cursor drift — consumed {} of {} spoken-word forced-alignment entries (raw "
             "alignment had {} tokens including whitespace)",
             wordCursor, spokenWords.size(), alignment.words.size());
    }
    if (charCursor != alignment.characters.size()) {
        warn("assembleChunk: char cursor drift — consumed {} of {} forced-alignment characters", charCursor,
             alignment.characters.size());
    }

    // ---- Step 2: slice. Per show.py: clamp each turn's slice to the midpoint
    // of its inter-turn gap (so we never grab the neighbor's audio), keep
    // PAD_IN onset + PAD_OUT release, apply linear ~8ms fade in/out to
    // de-click the cut.
    const std::ptrdiff_t padIn = static_cast<std::ptrdiff_t>(std::llround(dialog_pipeline::PAD_IN_SECS * srD));
    const std::ptrdiff_t padOut = static_cast<std::ptrdiff_t>(std::llround(dialog_pipeline::PAD_OUT_SECS * srD));
    const std::ptrdiff_t gap = static_cast<std::ptrdiff_t>(std::llround(dialog_pipeline::INTER_TURN_GAP_SECS * srD));
    const std::ptrdiff_t fade = static_cast<std::ptrdiff_t>(std::llround(dialog_pipeline::SEAM_FADE_SECS * srD));

    struct Slice {
        std::size_t voiceIndex;
        std::vector<int16_t> seg;
        std::ptrdiff_t origStart; // 'a' — needed to shift mouth timing later
        std::vector<TextToViseme::CharTiming> chars;
        std::vector<DialogWordTiming> words;
    };
    std::vector<Slice> slices;
    slices.reserve(turnInfos.size());

    for (std::size_t i = 0; i < turnInfos.size(); ++i) {
        // Non-const so the per-turn char/word vectors can be moved into the slice
        // below — neighbor lookups only read scalar start/end secs, so moving the
        // current turn's vectors is safe, and turnInfos is unused after this loop.
        auto &ti = turnInfos[i];
        const std::ptrdiff_t sSample = secsToSamples(ti.startSecs);
        const std::ptrdiff_t eSample = secsToSamples(ti.endSecs);

        const std::ptrdiff_t lo = (i == 0) ? 0 : secsToSamples((turnInfos[i - 1].endSecs + ti.startSecs) / 2.0);
        const std::ptrdiff_t hi = (i + 1 == turnInfos.size())
                                      ? static_cast<std::ptrdiff_t>(srcSamples)
                                      : secsToSamples((ti.endSecs + turnInfos[i + 1].startSecs) / 2.0);

        const std::ptrdiff_t a = std::max(lo, sSample - padIn);
        const std::ptrdiff_t b = std::min(hi, eSample + padOut);
        if (b <= a) {
            warn("assembleChunk: empty slice for turn {} (a={}, b={}); skipping", i, static_cast<long long>(a),
                 static_cast<long long>(b));
            continue;
        }

        Slice sl;
        sl.voiceIndex = ti.voiceIndex;
        sl.origStart = a;
        sl.chars = std::move(ti.chars);
        sl.words = std::move(ti.words);
        sl.seg.assign(src.begin() + a, src.begin() + b);

        // Linear fade in + fade out across `fade` samples (clamped to seg length).
        const std::ptrdiff_t segLen = static_cast<std::ptrdiff_t>(sl.seg.size());
        const std::ptrdiff_t maxFade = std::min(fade, segLen);
        if (fade > 0) {
            for (std::ptrdiff_t j = 0; j < maxFade; ++j) {
                const double g = static_cast<double>(j) / static_cast<double>(fade);
                sl.seg[j] = static_cast<int16_t>(static_cast<double>(sl.seg[j]) * g);
                sl.seg[segLen - 1 - j] = static_cast<int16_t>(static_cast<double>(sl.seg[segLen - 1 - j]) * g);
            }
        }

        slices.push_back(std::move(sl));
    }

    if (slices.empty()) {
        return Result<DialogAssembled>{
            ServerError(ServerError::InvalidData, "assembleChunk: every turn produced an empty slice")};
    }

    // ---- Step 3: place slices on the tightened timeline. Total length =
    // sum(slice lengths) + (N-1) * gap. Each slice writes into its voice's
    // buffer at the running pos; char timings shift by (pos - a)/SR.
    std::size_t total = 0;
    for (const auto &sl : slices) {
        total += sl.seg.size();
    }
    if (slices.size() > 1) {
        total += static_cast<std::size_t>(gap) * (slices.size() - 1);
    }

    DialogAssembled out;
    out.sampleRate = sampleRate;
    out.totalSamples = total;
    out.perCreature.resize(voiceIds.size());
    for (std::size_t i = 0; i < voiceIds.size(); ++i) {
        out.perCreature[i].voiceId = voiceIds[i];
        out.perCreature[i].pcm.assign(total, 0);
    }

    std::size_t pos = 0;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const auto &sl = slices[i];
        auto &dst = out.perCreature[sl.voiceIndex];

        std::copy(sl.seg.begin(), sl.seg.end(), dst.pcm.begin() + static_cast<std::ptrdiff_t>(pos));

        // shift = (pos - a) / SR. Same amount for chars (ms) and words (seconds),
        // since both live on the ORIGINAL forced-alignment timeline.
        const double shiftSec = (static_cast<double>(pos) - static_cast<double>(sl.origStart)) / srD;
        const double shiftMs = shiftSec * 1000.0;
        dst.mouth.reserve(dst.mouth.size() + sl.chars.size());
        for (const auto &c : sl.chars) {
            TextToViseme::CharTiming shifted = c;
            shifted.startTimeMs = c.startTimeMs + shiftMs;
            dst.mouth.push_back(shifted);
        }
        dst.words.reserve(dst.words.size() + sl.words.size());
        for (const auto &w : sl.words) {
            dst.words.push_back(DialogWordTiming{w.word, w.start + shiftSec, w.end + shiftSec});
        }

        pos += sl.seg.size();
        if (i + 1 < slices.size()) {
            pos += static_cast<std::size_t>(gap);
        }
    }

    debug("assembleChunk: {} turns -> {} voices, {} samples ({:.2f}s) at {} Hz", turns.size(), voiceIds.size(), total,
          static_cast<double>(total) / srD, sampleRate);
    return out;
}

Result<DialogAssembled> concatChunks(const std::vector<DialogAssembled> &chunks) {
    if (chunks.empty()) {
        return Result<DialogAssembled>{
            ServerError(ServerError::InvalidData, "concatChunks requires at least one chunk")};
    }
    if (chunks.size() == 1) {
        return chunks.front();
    }

    const uint32_t sr = chunks.front().sampleRate;
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        if (chunks[i].sampleRate != sr) {
            return Result<DialogAssembled>{
                ServerError(ServerError::InvalidData,
                            fmt::format("concatChunks: sample rate mismatch (chunk 0 = {}, chunk {} = {})", sr, i,
                                        chunks[i].sampleRate))};
        }
    }

    // Union the voice IDs across chunks, preserving first-seen order so the
    // output's perCreature[] matches the natural reading order of the scene.
    std::vector<std::string> voiceIds;
    std::unordered_map<std::string, std::size_t> voiceIndex;
    for (const auto &c : chunks) {
        for (const auto &pc : c.perCreature) {
            if (voiceIndex.find(pc.voiceId) == voiceIndex.end()) {
                voiceIndex.emplace(pc.voiceId, voiceIds.size());
                voiceIds.push_back(pc.voiceId);
            }
        }
    }

    const std::size_t gapSamples =
        static_cast<std::size_t>(std::llround(dialog_pipeline::INTER_CHUNK_GAP_SECS * static_cast<double>(sr)));

    std::size_t total = 0;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        total += chunks[i].totalSamples;
        if (i + 1 < chunks.size()) {
            total += gapSamples;
        }
    }

    DialogAssembled out;
    out.sampleRate = sr;
    out.totalSamples = total;
    out.perCreature.resize(voiceIds.size());
    for (std::size_t i = 0; i < voiceIds.size(); ++i) {
        out.perCreature[i].voiceId = voiceIds[i];
        out.perCreature[i].pcm.assign(total, 0);
    }

    std::size_t offset = 0;
    for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
        const auto &chunk = chunks[ci];
        const double offsetMs = static_cast<double>(offset) / static_cast<double>(sr) * 1000.0;

        for (const auto &pc : chunk.perCreature) {
            const std::size_t idx = voiceIndex.at(pc.voiceId);
            auto &dst = out.perCreature[idx];

            // Sanity: each chunk's per-voice PCM has length == chunk.totalSamples.
            // Tolerate shorter (zero-pad the tail) but log; longer would be a bug.
            const std::size_t copyLen = std::min(pc.pcm.size(), chunk.totalSamples);
            std::copy(pc.pcm.begin(), pc.pcm.begin() + static_cast<std::ptrdiff_t>(copyLen),
                      dst.pcm.begin() + static_cast<std::ptrdiff_t>(offset));
            if (pc.pcm.size() != chunk.totalSamples) {
                warn("concatChunks: voice {} in chunk {} has {} samples (expected {})", pc.voiceId, ci, pc.pcm.size(),
                     chunk.totalSamples);
            }

            dst.mouth.reserve(dst.mouth.size() + pc.mouth.size());
            for (const auto &c : pc.mouth) {
                TextToViseme::CharTiming shifted = c;
                shifted.startTimeMs = c.startTimeMs + offsetMs;
                dst.mouth.push_back(shifted);
            }
            const double offsetSec = static_cast<double>(offset) / static_cast<double>(sr);
            dst.words.reserve(dst.words.size() + pc.words.size());
            for (const auto &w : pc.words) {
                dst.words.push_back(DialogWordTiming{w.word, w.start + offsetSec, w.end + offsetSec});
            }
        }
        offset += chunk.totalSamples;
        if (ci + 1 < chunks.size()) {
            offset += gapSamples;
        }
    }

    debug("concatChunks: {} chunks -> {} voices, {} samples ({:.2f}s) at {} Hz", chunks.size(), voiceIds.size(), total,
          static_cast<double>(total) / static_cast<double>(sr), sr);
    return out;
}

} // namespace creatures::voice
