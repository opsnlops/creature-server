
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/DialogPipeline.h"
#include "server/voice/DialogPreviewAssembly.h"
#include "server/voice/DialogWav.h"

#include "DialogPreviewService.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures ::ws {

namespace {

/// Assemble the embedded provenance for a preview generation (#50) from the
/// resolved creatures and the turns, so editor exports of this take can carry
/// the source script, channel layout, and full script text. Mirrors what the
/// permanent-render path stamps, sourced from the preview's own data.
creatures::voice::DialogWavProvenance
buildPreviewProvenance(const std::vector<creatures::voice::DialogInput> &inputs,
                       const std::unordered_map<std::string, DialogPreviewService::PreviewCreature> &resolved,
                       const std::string &generationId, const std::string &scriptId, const std::string &title) {
    creatures::voice::DialogWavProvenance p;
    p.sourceScriptId = scriptId;
    p.title = title;
    if (!generationId.empty()) {
        p.generationIds = {generationId};
    }

    // voiceId → display name, for speaker labels and the track list.
    std::unordered_map<std::string, std::string> nameByVoice;
    for (const auto &[cid, c] : resolved) {
        nameByVoice[c.voiceId] = c.name.empty() ? cid : c.name;
    }

    // Tracks: each creature on its 1-based channel (sorted) + the BGM lane (17).
    for (const auto &[cid, c] : resolved) {
        if (c.audioChannel >= 1 && c.audioChannel <= 16) {
            p.tracks.push_back({c.audioChannel, c.name.empty() ? cid : c.name});
        }
    }
    std::sort(p.tracks.begin(), p.tracks.end(),
              [](const creatures::voice::DialogTrackInfo &a, const creatures::voice::DialogTrackInfo &b) {
                  return a.channel < b.channel;
              });
    p.tracks.push_back({17, "BGM"});

    // Script: speaker name (resolved via voiceId) + text, in turn order.
    p.script.reserve(inputs.size());
    for (const auto &in : inputs) {
        auto it = nameByVoice.find(in.voiceId);
        p.script.push_back({it != nameByVoice.end() ? it->second : in.voiceId, in.text});
    }
    return p;
}

} // namespace

creatures::Result<std::unordered_map<std::string, DialogPreviewService::PreviewCreature>>
DialogPreviewService::resolveCreatures(const oatpp::List<oatpp::Object<DialogTurnDto>> &turns,
                                       const std::shared_ptr<creatures::OperationSpan> &span) {
    std::unordered_map<std::string, PreviewCreature> resolved;
    for (const auto &t : *turns) {
        const std::string cid(*t->creature_id);
        if (resolved.count(cid)) {
            continue;
        }
        auto jr = creatures::db->getCreatureJson(cid, span);
        if (!jr.isSuccess()) {
            return creatures::Result<std::unordered_map<std::string, PreviewCreature>>{creatures::ServerError(
                creatures::ServerError::InvalidData,
                fmt::format("creature '{}' lookup failed: {}", cid, jr.getError().value().getMessage()))};
        }
        const auto cj = jr.getValue().value();
        if (!cj.contains("voice") || !cj["voice"].is_object() || !cj["voice"].contains("voice_id") ||
            !cj["voice"]["voice_id"].is_string()) {
            return creatures::Result<std::unordered_map<std::string, PreviewCreature>>{creatures::ServerError(
                creatures::ServerError::InvalidData, fmt::format("creature '{}' has no voice.voice_id", cid))};
        }
        PreviewCreature pc;
        pc.creatureId = cid;
        pc.voiceId = cj["voice"]["voice_id"].get<std::string>();
        pc.name = cj.value("name", std::string{});
        pc.audioChannel =
            cj.contains("audio_channel") && cj["audio_channel"].is_number() ? cj["audio_channel"].get<uint16_t>() : 0;
        resolved.emplace(cid, std::move(pc));
    }
    return resolved;
}

std::vector<creatures::voice::DialogInput>
DialogPreviewService::buildDialogInputs(const oatpp::List<oatpp::Object<DialogTurnDto>> &turns,
                                        const std::unordered_map<std::string, PreviewCreature> &resolved) {
    std::vector<creatures::voice::DialogInput> out;
    out.reserve(turns->size());
    for (const auto &t : *turns) {
        const std::string cid(*t->creature_id);
        const std::string text(*t->text);
        const auto &c = resolved.at(cid);
        out.push_back({c.voiceId, text});
    }
    return out;
}

std::string DialogPreviewService::makeTurnsSummary(const std::vector<creatures::voice::DialogInput> &inputs) {
    std::string s;
    for (const auto &i : inputs) {
        if (!s.empty()) {
            s.push_back(' ');
        }
        s += creatures::voice::DialogClient::stripTags(i.text);
        if (s.size() >= 80) {
            s.resize(80);
            s += "…";
            break;
        }
    }
    return s;
}

void DialogPreviewService::populateMetaResponse(oatpp::Object<DialogPreviewMetaResponseDto> &dto,
                                                const creatures::voice::CachedGeneration &gen,
                                                const std::string &cacheKey, bool cached) {
    dto->cache_key = cacheKey.c_str();
    dto->generation_id = gen.generationId.c_str();
    dto->cached = cached;
    dto->audio_url =
        fmt::format("/api/v1/animation/dialog/preview/audio/{}/{}.wav", cacheKey, gen.generationId).c_str();
    dto->audio_format = "pcm_48000";
    dto->sample_rate = 48000;
    dto->duration_seconds = static_cast<double>(gen.audioPcm.size()) / (48000.0 * 2.0); // mono S16

    auto segs = oatpp::List<oatpp::Object<DialogPreviewVoiceSegmentDto>>::createShared();
    for (const auto &s : gen.voiceSegments) {
        auto sd = DialogPreviewVoiceSegmentDto::createShared();
        sd->voice_id = s.voiceId.c_str();
        sd->character_start_index = static_cast<v_uint64>(s.characterStartIndex);
        sd->character_end_index = static_cast<v_uint64>(s.characterEndIndex);
        sd->dialog_input_index = static_cast<v_uint64>(s.dialogInputIndex);
        segs->push_back(sd);
    }
    dto->voice_segments = segs;

    auto words = oatpp::List<oatpp::Object<DialogPreviewWordTimingDto>>::createShared();
    for (const auto &w : gen.forcedAlignment.words) {
        auto wd = DialogPreviewWordTimingDto::createShared();
        wd->text = w.text.c_str();
        wd->start = w.startSeconds;
        wd->end = w.endSeconds;
        words->push_back(wd);
    }
    dto->forced_alignment_words = words;

    auto chars = oatpp::List<oatpp::Object<DialogPreviewCharTimingDto>>::createShared();
    for (const auto &c : gen.forcedAlignment.characters) {
        auto cd = DialogPreviewCharTimingDto::createShared();
        cd->text = c.text.c_str();
        cd->start = c.startSeconds;
        cd->end = c.endSeconds;
        chars->push_back(cd);
    }
    dto->forced_alignment_chars = chars;

    dto->forced_alignment_loss = gen.forcedAlignment.loss;
}

creatures::Result<DialogPreviewService::CacheProbe>
DialogPreviewService::probeCache(const oatpp::Object<DialogPreviewRequestDto> &body,
                                 const std::shared_ptr<creatures::OperationSpan> &opSpan, const char *spanAttrName) {
    CacheProbe probe;

    auto resolvedResult = resolveCreatures(body->turns, opSpan);
    if (!resolvedResult.isSuccess()) {
        return creatures::Result<CacheProbe>{resolvedResult.getError().value()};
    }
    probe.resolved = resolvedResult.getValue().value();
    probe.inputs = buildDialogInputs(body->turns, probe.resolved);
    probe.cacheKey = creatures::voice::computeCacheKey(probe.inputs);
    probe.regenerate = body->regenerate ? static_cast<bool>(*body->regenerate) : false;

    if (opSpan) {
        opSpan->setAttribute("dialog.cache_key", probe.cacheKey);
        opSpan->setAttribute("dialog.turns", static_cast<int64_t>(probe.inputs.size()));
        if (spanAttrName) {
            opSpan->setAttribute("dialog.endpoint", std::string(spanAttrName));
        }
    }

    if (body->generation_id && !body->generation_id->empty()) {
        // Explicit generation_id — load that one specifically. NotFound if it's
        // been cron-cleaned.
        auto loadResult = creatures::voice::loadGeneration(probe.cacheKey, std::string(*body->generation_id));
        if (!loadResult.isSuccess()) {
            return creatures::Result<CacheProbe>{creatures::ServerError(
                creatures::ServerError::NotFound, fmt::format("generation '{}' not found (expired or never existed)",
                                                              std::string(*body->generation_id)))};
        }
        probe.generation = loadResult.getValue().value();
        probe.cached = true;
    } else if (!probe.regenerate) {
        if (auto latest = creatures::voice::findLatestGeneration(probe.cacheKey)) {
            auto loadResult = creatures::voice::loadGeneration(probe.cacheKey, *latest);
            if (loadResult.isSuccess()) {
                probe.generation = loadResult.getValue().value();
                probe.cached = true;
            }
            // If the load failed for some odd reason, leave generation empty —
            // the cache lookup is advisory.
        }
    }

    return probe;
}

creatures::Result<DialogPreviewService::MetaFastPath>
DialogPreviewService::tryServeFromCache(const oatpp::Object<DialogPreviewRequestDto> &body,
                                        const std::shared_ptr<creatures::OperationSpan> &opSpan,
                                        const char *spanAttrName) {
    auto probeResult = probeCache(body, opSpan, spanAttrName);
    if (!probeResult.isSuccess()) {
        return creatures::Result<MetaFastPath>{probeResult.getError().value()};
    }
    auto probe = probeResult.getValue().value();

    MetaFastPath fast;
    fast.cacheKey = probe.cacheKey;
    if (probe.generation) {
        PreviewOutcome outcome;
        outcome.generation = std::move(*probe.generation);
        outcome.cacheKey = probe.cacheKey;
        outcome.cached = probe.cached;
        outcome.inputs = std::move(probe.inputs);
        outcome.resolved = std::move(probe.resolved);
        fast.cacheHit = true;
        fast.outcome = std::move(outcome);
        if (opSpan) {
            opSpan->setAttribute("dialog.generation_id", fast.outcome->generation.generationId);
            opSpan->setAttribute("dialog.cached", true);
        }
    }
    return fast;
}

creatures::Result<DialogPreviewService::PreviewOutcome>
DialogPreviewService::loadOrGenerate(const oatpp::Object<DialogPreviewRequestDto> &body,
                                     const std::shared_ptr<creatures::OperationSpan> &opSpan, const char *spanAttrName,
                                     std::function<void(float)> progress, const std::string &jobId) {
    auto probeResult = probeCache(body, opSpan, spanAttrName);
    if (!probeResult.isSuccess()) {
        return creatures::Result<PreviewOutcome>{probeResult.getError().value()};
    }
    auto probe = probeResult.getValue().value();

    PreviewOutcome out;
    out.cacheKey = probe.cacheKey;
    out.inputs = std::move(probe.inputs);
    out.resolved = std::move(probe.resolved);

    if (probe.generation) {
        out.generation = std::move(*probe.generation);
        out.cached = probe.cached;
        if (progress) {
            progress(1.0f);
        }
        if (opSpan) {
            opSpan->setAttribute("dialog.generation_id", out.generation.generationId);
            opSpan->setAttribute("dialog.cached", out.cached);
        }
        return out;
    }

    // No usable cache hit (or regenerate requested) — fresh ElevenLabs. Long
    // scenes are split at turn boundaries (the per-call ~2000-char API cap is
    // real); each chunk is cached under its own key — the SAME entries the
    // render job reads, so render-after-preview reuses the auditioned audio.
    creatures::voice::DialogClient client;
    const std::string apiKey = creatures::config->getVoiceApiKey();

    auto chunksResult = creatures::voice::chunkTurns(out.inputs);
    if (!chunksResult.isSuccess()) {
        // No 422 mapping exists in serverErrorToStatusCode; InvalidData → 400 is
        // the closest fit for an unprocessable chunking request.
        return creatures::Result<PreviewOutcome>{
            creatures::ServerError(creatures::ServerError::InvalidData, chunksResult.getError().value().getMessage())};
    }
    const auto chunks = chunksResult.getValue().value();
    if (opSpan) {
        opSpan->setAttribute("dialog.chunks", static_cast<int64_t>(chunks.size()));
    }

    // Resolve one chunk under its own child span so a slow/uncached chunk is visible
    // in Honeycomb — same shape and `dialog.*` attributes as the render job's
    // DialogJob.chunk.N spans, so preview and render traces read identically.
    auto resolveChunk = [&](std::size_t ci, const std::vector<creatures::voice::DialogInput> &chunk,
                            const std::string &chunkKey) -> creatures::Result<creatures::voice::CachedGeneration> {
        auto chunkSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                        fmt::format("DialogPreviewJob.chunk.{}", ci), opSpan)
                                                  : nullptr;
        std::size_t chunkChars = 0;
        for (const auto &input : chunk) {
            chunkChars += creatures::voice::DialogClient::stripTags(input.text).size();
        }
        if (chunkSpan) {
            chunkSpan->setAttribute("dialog.chunk_index", static_cast<int64_t>(ci));
            chunkSpan->setAttribute("dialog.chunk_char_count", static_cast<int64_t>(chunkChars));
            chunkSpan->setAttribute("dialog.cache_key", chunkKey);
            if (!jobId.empty()) {
                chunkSpan->setAttribute("job.id", jobId);
            }
        }

        if (!probe.regenerate) {
            if (auto latest = creatures::voice::findLatestGeneration(chunkKey)) {
                auto loadResult = creatures::voice::loadGeneration(chunkKey, *latest);
                if (loadResult.isSuccess()) {
                    auto gen = loadResult.getValue().value();
                    if (chunkSpan) {
                        chunkSpan->setAttribute("dialog.cache_hit", true);
                        chunkSpan->setAttribute("dialog.generation_id", gen.generationId);
                        chunkSpan->setSuccess();
                    }
                    return gen;
                }
            }
        }

        auto genResult = creatures::voice::generateChunkWithAlignment(client, apiKey, chunk, chunkKey, chunkSpan);
        if (!genResult.isSuccess()) {
            if (chunkSpan) {
                chunkSpan->setError(genResult.getError().value().getMessage());
            }
            return creatures::Result<creatures::voice::CachedGeneration>{genResult.getError().value()};
        }
        auto gen = genResult.getValue().value();
        if (chunkSpan) {
            chunkSpan->setAttribute("dialog.cache_hit", false);
            chunkSpan->setAttribute("dialog.generation_id", gen.generationId);
            chunkSpan->setSuccess();
        }
        return gen;
    };

    if (chunks.size() == 1) {
        auto genResult = resolveChunk(0, out.inputs, out.cacheKey);
        if (!genResult.isSuccess()) {
            return creatures::Result<PreviewOutcome>{genResult.getError().value()};
        }
        out.generation = genResult.getValue().value();
        // A single-chunk scene is cached iff its one chunk was a cache hit; but the
        // scene key == chunk key here, so the caller's `cached` probe already ran.
        out.cached = false;
        if (progress) {
            progress(1.0f);
        }
    } else {
        std::vector<creatures::voice::CachedGeneration> chunkGens;
        std::vector<std::size_t> turnCounts;
        chunkGens.reserve(chunks.size());
        turnCounts.reserve(chunks.size());

        for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
            const auto &chunk = chunks[ci];
            const auto chunkKey = creatures::voice::computeCacheKey(chunk);
            auto genResult = resolveChunk(ci, chunk, chunkKey);
            if (!genResult.isSuccess()) {
                return creatures::Result<PreviewOutcome>{genResult.getError().value()};
            }
            chunkGens.push_back(genResult.getValue().value());
            turnCounts.push_back(chunk.size());
            if (progress) {
                progress(static_cast<float>(ci + 1) / static_cast<float>(chunks.size()));
            }
        }

        auto merged = creatures::voice::mergeChunkGenerations(chunkGens, turnCounts);
        merged.turnsSummary = makeTurnsSummary(out.inputs);

        auto saveResult = creatures::voice::saveGeneration(out.cacheKey, merged);
        if (!saveResult.isSuccess()) {
            warn("Dialog preview: saveGeneration (merged) failed: {}", saveResult.getError().value().getMessage());
        }

        out.generation = std::move(merged);
        // The merged scene take is freshly assembled from the chunks; whether the
        // individual chunks were cached is captured per-chunk-span above.
        out.cached = false;
    }

    // Stamp provenance (#50) so editor exports of this take carry the script.
    // Built from the preview's own resolved creatures + turns. Held in memory
    // for the 17-channel export (which uses `out` directly); persisted to the
    // cache sidecar so the mono/Ogg export endpoints — which load only by id —
    // can read it back. Skip the (json-only) persist if the generation already
    // carried provenance from a prior run.
    {
        // The preview request carries only turns — no saved script id/title (those
        // arrive at render time). So provenance here is the track layout + script
        // text + generation id.
        const bool alreadyPersisted = !out.generation.provenance.empty();
        out.generation.provenance = buildPreviewProvenance(out.inputs, out.resolved, out.generation.generationId,
                                                           /*scriptId=*/"", /*title=*/"");
        if (!alreadyPersisted && !out.generation.provenance.empty()) {
            auto r = creatures::voice::updateGenerationProvenance(out.cacheKey, out.generation.generationId,
                                                                  out.generation.provenance);
            if (!r.isSuccess()) {
                warn("Dialog preview: persisting provenance for {}/{} failed: {}", out.cacheKey,
                     out.generation.generationId, r.getError().value().getMessage());
            }
        }
    }

    if (opSpan) {
        opSpan->setAttribute("dialog.generation_id", out.generation.generationId);
        opSpan->setAttribute("dialog.cached", out.cached);
    }
    return out;
}

creatures::Result<void>
DialogPreviewService::exportMultichannel(const PreviewOutcome &outcome, const std::filesystem::path &wavPath,
                                         const std::shared_ptr<creatures::OperationSpan> &opSpan) {
    // Validate the audio_channel mapping (distinct, in [1,17]). InvalidData →
    // 400 for callers that surface it as HTTP; the worker just fails the job.
    std::unordered_set<uint16_t> seenChannels;
    creatures::voice::VoiceChannelMap voiceToChannel;
    for (const auto &[cid, c] : outcome.resolved) {
        if (c.audioChannel < 1 || c.audioChannel > 17) {
            return creatures::Result<void>{creatures::ServerError(
                creatures::ServerError::InvalidData,
                fmt::format("creature '{}' has audio_channel {} (must be 1..17)", cid, c.audioChannel))};
        }
        if (!seenChannels.insert(c.audioChannel).second) {
            return creatures::Result<void>{creatures::ServerError(
                creatures::ServerError::InvalidData,
                fmt::format("audio_channel {} is assigned to more than one creature in this scene", c.audioChannel))};
        }
        voiceToChannel.emplace(c.voiceId, c.audioChannel);
    }

    creatures::voice::DialogResult dr;
    dr.audioData = outcome.generation.audioPcm;
    dr.audioFormat = "pcm_48000";
    dr.voiceSegments = outcome.generation.voiceSegments;
    auto assembleResult =
        creatures::voice::assembleChunk(outcome.inputs, dr, outcome.generation.forcedAlignment, 48000);
    if (!assembleResult.isSuccess()) {
        return creatures::Result<void>{assembleResult.getError().value()};
    }
    const auto assembled = assembleResult.getValue().value();

    std::error_code ec;
    std::filesystem::create_directories(wavPath.parent_path(), ec);
    const auto &prov = outcome.generation.provenance;
    auto writeResult =
        creatures::voice::writeDialogWav(assembled, voiceToChannel, wavPath, opSpan, prov.empty() ? nullptr : &prov);
    if (!writeResult.isSuccess()) {
        return creatures::Result<void>{writeResult.getError().value()};
    }
    return creatures::Result<void>{};
}

} // namespace creatures::ws
