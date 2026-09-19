#include "server/voice/PromotedTake.h"

#include <algorithm>
#include <unordered_map>

#include <fmt/format.h>

#include "server/audio/DecodedAudioStream.h"
#include "server/storage/Storage.h"
#include "server/voice/IxmlReader.h"

namespace creatures::voice {

namespace {

constexpr uint16_t kDialogWavChannels = 17;
constexpr uint32_t kDialogSampleRate = 48000;

} // namespace

Result<DialogAssembled> loadAssembledTakeFromPromotedFile(const std::string &soundFile,
                                                          const std::vector<PromotedTakeLane> &lanes) {
    using TakeResult = Result<DialogAssembled>;
    if (soundFile.empty()) {
        return TakeResult{ServerError(ServerError::InvalidData, "promoted take has no sound file")};
    }
    const auto path = storage::resolveSoundPath(soundFile);
    const auto ixml = readIxmlChunk(path);
    if (!ixml) {
        return TakeResult{ServerError(ServerError::InvalidData,
                                      fmt::format("promoted take '{}' has no embedded provenance", soundFile))};
    }
    const auto provenance = parseIxmlProvenance(*ixml);
    if (provenance.lipsync.empty() && provenance.wordAlignment.empty()) {
        return TakeResult{ServerError(ServerError::InvalidData,
                                      fmt::format("accepted take '{}' has no timing on file; it cannot be rendered "
                                                  "without regenerating — re-accept a take for this script",
                                                  soundFile))};
    }

    auto opened = audio::DecodedAudioStream::open(path.string(), kDialogSampleRate, kDialogWavChannels);
    if (!opened.isSuccess()) {
        return TakeResult{opened.getError().value()};
    }
    auto stream = opened.getValue().value();

    // Which lanes to lift: every named track in the file that the job knows a
    // voice for. Channel 17 is BGM and never a creature.
    std::unordered_map<uint16_t, std::string> voiceByChannel;
    for (const auto &lane : lanes) {
        if (lane.channel >= 1 && lane.channel < kDialogWavChannels) {
            voiceByChannel.emplace(lane.channel, lane.voiceId);
        }
    }
    std::vector<uint16_t> channels;
    for (const auto &track : provenance.tracks) {
        if (!track.name.empty() && voiceByChannel.contains(track.channel)) {
            channels.push_back(track.channel);
        }
    }
    // Older files may lack a TRACK_LIST; the lip-sync tracks name lanes too.
    for (const auto &track : provenance.lipsync) {
        if (voiceByChannel.contains(track.channel) &&
            std::find(channels.begin(), channels.end(), track.channel) == channels.end()) {
            channels.push_back(track.channel);
        }
    }
    if (channels.empty()) {
        return TakeResult{
            ServerError(ServerError::InvalidData,
                        fmt::format("promoted take '{}' names no lane this script's creatures are on", soundFile))};
    }
    std::sort(channels.begin(), channels.end());

    DialogAssembled assembled;
    assembled.sampleRate = kDialogSampleRate;
    std::unordered_map<uint16_t, std::size_t> laneIndex;
    for (const auto channel : channels) {
        DialogPerCreature lane;
        lane.voiceId = voiceByChannel.at(channel);
        lane.mouthCues = std::vector<RhubarbMouthCue>{};
        for (const auto &track : provenance.lipsync) {
            if (track.channel != channel)
                continue;
            for (const auto &cue : track.cues) {
                lane.mouthCues->push_back({cue.start, cue.end, cue.shape});
            }
        }
        for (const auto &track : provenance.wordAlignment) {
            if (track.channel == channel) {
                lane.words = track.words;
            }
        }
        laneIndex.emplace(channel, assembled.perCreature.size());
        assembled.perCreature.push_back(std::move(lane));
    }

    // De-interleave in bounded blocks to EOF; a long scene is hundreds of
    // MB, and the decoder doesn't report a length up front by design.
    constexpr std::size_t kFramesPerBlock = 48000;
    std::vector<int16_t> block(kFramesPerBlock * kDialogWavChannels);
    while (true) {
        auto read = stream->readFrames(block);
        if (!read.isSuccess()) {
            return TakeResult{read.getError().value()};
        }
        const auto frames = read.getValue().value();
        if (frames == 0) {
            break;
        }
        for (const auto channel : channels) {
            auto &pcm = assembled.perCreature[laneIndex.at(channel)].pcm;
            pcm.reserve(pcm.size() + frames);
            for (std::size_t f = 0; f < frames; ++f) {
                pcm.push_back(block[f * kDialogWavChannels + (channel - 1)]);
            }
        }
        assembled.totalSamples += frames;
    }
    if (assembled.totalSamples == 0) {
        return TakeResult{ServerError(ServerError::InvalidData, fmt::format("promoted take '{}' is empty", soundFile))};
    }
    // A render's timeline runs to the end of its music, so a prior render
    // read back as the take carries a silent creature-lane tail. The
    // assembled timeline was tightened to the last spoken sample, so any
    // trailing all-lane silence can only be that tail; drop it.
    std::size_t lastSound = 0;
    for (const auto &lane : assembled.perCreature) {
        for (std::size_t i = lane.pcm.size(); i > lastSound; --i) {
            if (lane.pcm[i - 1] != 0) {
                lastSound = i;
                break;
            }
        }
    }
    if (lastSound > 0 && lastSound < assembled.totalSamples) {
        for (auto &lane : assembled.perCreature) {
            lane.pcm.resize(lastSound);
        }
        assembled.totalSamples = lastSound;
    }
    return TakeResult{std::move(assembled)};
}

} // namespace creatures::voice
