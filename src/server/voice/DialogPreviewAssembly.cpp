
#include <algorithm>
#include <chrono>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "server/namespace-stuffs.h"
#include "server/voice/DialogPreviewAssembly.h"
#include "server/voice/PcmWavWriter.h"
#include "util/uuidUtils.h"

namespace creatures::voice {

namespace {

/// First ~80 chars of the tag-stripped, space-joined turn text — the same
/// human-readable cache-directory summary the preview and render always built.
std::string makeTurnsSummary(const std::vector<DialogInput> &inputs) {
    std::string s;
    for (const auto &i : inputs) {
        if (!s.empty()) {
            s.push_back(' ');
        }
        s += DialogClient::stripTags(i.text);
        if (s.size() >= 80) {
            s.resize(80);
            s += "…";
            break;
        }
    }
    return s;
}

} // namespace

Result<CachedGeneration> generateChunkWithAlignment(DialogClient &client, const std::string &apiKey,
                                                    const std::vector<DialogInput> &chunk, const std::string &cacheKey,
                                                    std::shared_ptr<OperationSpan> span) {

    auto dialogResult = client.generateDialog(apiKey, chunk, "pcm_48000", span);
    if (!dialogResult.isSuccess()) {
        return Result<CachedGeneration>{dialogResult.getError().value()};
    }
    const auto dialog = dialogResult.getValue().value();

    std::string transcript;
    for (std::size_t t = 0; t < chunk.size(); ++t) {
        if (t > 0) {
            transcript.push_back(' ');
        }
        transcript += DialogClient::stripTags(chunk[t].text);
    }

    const auto wavBytes = wrapMonoPcmAsWav(dialog.audioData, 48000);
    auto alignResult = client.forcedAlignment(apiKey, wavBytes, "audio/wav", transcript, span);
    if (!alignResult.isSuccess()) {
        return Result<CachedGeneration>{alignResult.getError().value()};
    }

    CachedGeneration gen;
    gen.generationId = util::generateUUID();
    gen.audioPcm = dialog.audioData;
    gen.voiceSegments = dialog.voiceSegments;
    gen.forcedAlignment = alignResult.getValue().value();
    gen.createdAt = std::chrono::system_clock::now();
    gen.turnsSummary = makeTurnsSummary(chunk);

    auto saveResult = saveGeneration(cacheKey, gen);
    if (!saveResult.isSuccess()) {
        // Generation succeeded but couldn't be persisted — the caller still gets
        // the audio; we just lose the next-time cache benefit.
        warn("generateChunkWithAlignment: saveGeneration failed: {}", saveResult.getError().value().getMessage());
    }

    return Result<CachedGeneration>{gen};
}

CachedGeneration mergeChunkGenerations(const std::vector<CachedGeneration> &chunkGens,
                                       const std::vector<std::size_t> &turnCounts, uint32_t sampleRate,
                                       double interChunkGapSecs) {

    CachedGeneration merged;
    merged.generationId = util::generateUUID();
    merged.createdAt = std::chrono::system_clock::now();
    merged.forcedAlignment.loss = 0.0;

    const auto gapSamples = static_cast<std::size_t>(interChunkGapSecs * sampleRate);
    const auto bytesPerSecond = static_cast<double>(sampleRate) * 2.0; // mono S16

    double timeOffset = 0.0;
    std::size_t turnOffset = 0;
    std::size_t segmentCharOffset = 0;

    for (std::size_t i = 0; i < chunkGens.size(); ++i) {
        const auto &gen = chunkGens[i];

        if (i > 0) {
            // Seam: silence in the audio, and a synthetic space character so the merged
            // character stream still matches the space-joined whole-scene transcript.
            merged.audioPcm.insert(merged.audioPcm.end(), gapSamples * 2, 0);
            ForcedAlignmentChar seamSpace;
            seamSpace.text = " ";
            seamSpace.startSeconds = timeOffset;
            seamSpace.endSeconds = timeOffset + interChunkGapSecs;
            merged.forcedAlignment.characters.push_back(seamSpace);
            timeOffset += interChunkGapSecs;
        }

        merged.audioPcm.insert(merged.audioPcm.end(), gen.audioPcm.begin(), gen.audioPcm.end());

        for (auto word : gen.forcedAlignment.words) {
            word.startSeconds += timeOffset;
            word.endSeconds += timeOffset;
            merged.forcedAlignment.words.push_back(std::move(word));
        }
        for (auto ch : gen.forcedAlignment.characters) {
            ch.startSeconds += timeOffset;
            ch.endSeconds += timeOffset;
            merged.forcedAlignment.characters.push_back(std::move(ch));
        }
        merged.forcedAlignment.loss = std::max(merged.forcedAlignment.loss, gen.forcedAlignment.loss);

        std::size_t chunkSegmentChars = 0;
        for (auto segment : gen.voiceSegments) {
            chunkSegmentChars = std::max(chunkSegmentChars, segment.characterEndIndex);
            segment.characterStartIndex += segmentCharOffset;
            segment.characterEndIndex += segmentCharOffset;
            segment.dialogInputIndex += turnOffset;
            segment.startTimeSeconds += timeOffset; // diagnostics only, like the source values
            segment.endTimeSeconds += timeOffset;
            merged.voiceSegments.push_back(std::move(segment));
        }

        segmentCharOffset += chunkSegmentChars + 1; // +1 for the joining space
        turnOffset += i < turnCounts.size() ? turnCounts[i] : 0;
        timeOffset += static_cast<double>(gen.audioPcm.size()) / bytesPerSecond;
    }

    debug("merged {} dialog chunks: {} samples, {} words, loss={}", chunkGens.size(), merged.audioPcm.size() / 2,
          merged.forcedAlignment.words.size(), merged.forcedAlignment.loss);
    return merged;
}

} // namespace creatures::voice
