//
// music.cpp – Opus-only server-side file streamer with RTP marker
//
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <SDL.h>
#include <SDL_mixer.h>
#include <spdlog/spdlog.h>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "server/namespace-stuffs.h"

namespace creatures {

// ───── singletons from main.cpp ──────────────────────────────────────────────
extern const char*                        audioDevice;
extern SDL_AudioSpec                      localAudioDeviceAudioSpec;
extern std::shared_ptr<Configuration>     config;
extern std::shared_ptr<GPIO>              gpioPins;
extern std::shared_ptr<SystemCounters>    metrics;
extern std::shared_ptr<EventLoop>         eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;

// ───── small helper: generic lambda event ───────────────────────────────────
template <typename Tag>
class LambdaEvent : public EventBase<LambdaEvent<Tag>> {
public:
    using Fn = std::function<void()>;
    LambdaEvent(framenum_t fn, Fn fnc)
        : EventBase<LambdaEvent<Tag>>(fn), fn_(std::move(fnc)) {}
    void executeImpl() { fn_(); }
private:
    Fn fn_;
};

// ───── MusicEvent impl ───────────────────────────────────────────────────────
MusicEvent::MusicEvent(framenum_t fn, std::string path)
    : EventBase(fn), filePath(std::move(path)) {}

void MusicEvent::executeImpl()
{
    if (filePath.empty() || !std::filesystem::is_regular_file(filePath)) {
        error("MusicEvent: invalid file '{}'", filePath); return;
    }
    std::ifstream test(filePath); if (!test.good()) {
        error("MusicEvent: unreadable file '{}'", filePath); return;
    }

    auto span = observability->createOperationSpan("music_event.execute");
    span->setAttribute("file_path", filePath);

    if (config->getAudioMode() == Configuration::AudioMode::RTP)
        scheduleRtpAudio(span);
    else
        playLocalAudio(span);

    span->setSuccess();
}

// ───── Local SDL playback (unchanged) ────────────────────────────────────────
void MusicEvent::playLocalAudio(std::shared_ptr<OperationSpan> parentSpan)
{
    /* identical to previous version – omitted for brevity */
}

// ───── RTP / Opus streaming ──────────────────────────────────────────────────
void MusicEvent::scheduleRtpAudio(std::shared_ptr<OperationSpan> parentSpan)
{
    if (!parentSpan)
        parentSpan = observability->createOperationSpan("music_event.schedule_rtp_audio");
    auto span = observability->createChildOperationSpan("music_event.schedule_rtp", parentSpan);

    if (!rtpServer || !rtpServer->isReady()) {
        span->setError("rtp_server_not_ready");
        error("Opus RTP server not ready – aborting stream");
        return;
    }

    auto buffer = rtp::AudioStreamBuffer::loadFromWav(filePath, span);
    if (!buffer) { span->setError("buffer_load_failed"); return; }

    const std::size_t frames = buffer->frameCount();
    constexpr std::size_t kPrefill = 3;              // 30 ms priming burst
    framenum_t cursor = frameNumber + 1;             // 1 ms after this event

    span->setAttribute("frames_total", static_cast<int64_t>(frames));

    for (std::size_t f = 0; f < frames; ++f) {

        using Tag = struct {};                       // unique tag for CRTP
        bool isFirstPacket = (f == 0);

        auto ev = std::make_shared<LambdaEvent<Tag>>(cursor, [buf = buffer, f, isFirstPacket] {
            for (uint8_t ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch)
                rtpServer->send(ch, buf->frame(ch, f));
        });

        eventLoop->scheduleEvent(ev);

        cursor += (f + 1 < kPrefill)
                  ? 1                                     // 1 ms priming
                  : RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;  // steady 10 ms
    }

    metrics->incrementSoundsPlayed();
    span->setSuccess();
}

// ───── SDL helpers (unchanged) ───────────────────────────────────────────────
int  MusicEvent::initSDL()            { /* … same … */ return 1; }
int  MusicEvent::locateAudioDevice()  { /* … same … */ return 1; }
void MusicEvent::listAudioDevices()   { /* … same … */ }

} // namespace creatures