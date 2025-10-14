//
// CooperativeAnimationScheduler.cpp
// Modern cooperative playback scheduler implementation
//

#include "CooperativeAnimationScheduler.h"

#include <memory>

#include "spdlog/spdlog.h"

#include "PlaybackSession.h"
#include "server/audio/AudioTransport.h"
#include "server/audio/LocalSdlAudioTransport.h"
#include "server/audio/RtpAudioTransport.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;

Result<std::shared_ptr<PlaybackSession>> CooperativeAnimationScheduler::scheduleAnimation(framenum_t startingFrame,
                                                                                          const Animation &animation,
                                                                                          universe_t universe) {
    // Create observability span
    auto scheduleSpan = observability->createOperationSpan("CooperativeAnimationScheduler.scheduleAnimation");
    if (scheduleSpan) {
        scheduleSpan->setAttribute("animation.id", animation.id);
        scheduleSpan->setAttribute("animation.title", animation.metadata.title);
        scheduleSpan->setAttribute("animation.universe", static_cast<int64_t>(universe));
        scheduleSpan->setAttribute("animation.starting_frame", static_cast<int64_t>(startingFrame));
        scheduleSpan->setAttribute("scheduler.type", "cooperative");
    }

    debug("CooperativeAnimationScheduler: scheduling animation '{}' on universe {} at frame {}",
          animation.metadata.title, universe, startingFrame);

    // Create playback session
    auto session = std::make_shared<PlaybackSession>(animation, universe, startingFrame, scheduleSpan);

    // Load audio buffer if animation has sound
    if (!animation.metadata.sound_file.empty()) {
        auto loadResult = loadAudioBuffer(animation, session, scheduleSpan);
        if (!loadResult.isSuccess()) {
            error("Failed to load audio buffer: {}", loadResult.getError()->getMessage());
            if (scheduleSpan) {
                scheduleSpan->setError(loadResult.getError()->getMessage());
            }
            return Result<std::shared_ptr<PlaybackSession>>{loadResult.getError().value()};
        }

        // Create audio transport
        auto audioTransport = createAudioTransport(session);
        session->setAudioTransport(audioTransport);
    }

    // Set up lifecycle callbacks
    setupLifecycleCallbacks(session, universe);

    // Schedule the initial PlaybackRunnerEvent
    auto initialRunner = std::make_shared<PlaybackRunnerEvent>(startingFrame, session);
    eventLoop->scheduleEvent(initialRunner);

    debug("CooperativeAnimationScheduler: scheduled initial PlaybackRunnerEvent for frame {}", startingFrame);

    // Increment metrics
    metrics->incrementAnimationsPlayed();

    if (scheduleSpan) {
        scheduleSpan->setSuccess();
    }

    info("✅ Scheduled cooperative animation '{}' for universe {} starting at frame {}", animation.metadata.title,
         universe, startingFrame);

    return Result<std::shared_ptr<PlaybackSession>>{session};
}

Result<void> CooperativeAnimationScheduler::loadAudioBuffer(const Animation &animation,
                                                            std::shared_ptr<PlaybackSession> session,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    auto loadSpan = observability->createChildOperationSpan("load_audio_buffer", parentSpan);
    if (loadSpan) {
        loadSpan->setAttribute("sound_file", animation.metadata.sound_file);
    }

    // Build full path to sound file
    std::string soundFilePath = config->getSoundFileLocation() + "/" + animation.metadata.sound_file;

    debug("Loading audio buffer from: {}", soundFilePath);

    // Load and encode audio buffer (heavy I/O operation)
    auto audioBuffer = rtp::AudioStreamBuffer::loadFromWavFile(soundFilePath, loadSpan);
    if (!audioBuffer) {
        std::string errorMsg = fmt::format("Failed to load audio buffer from '{}'", soundFilePath);
        error(errorMsg);
        if (loadSpan) {
            loadSpan->setError(errorMsg);
        }
        return Result<void>{ServerError(ServerError::NotFound, errorMsg)};
    }

    session->setAudioBuffer(audioBuffer);

    if (loadSpan) {
        loadSpan->setAttribute("frames_loaded", static_cast<int64_t>(audioBuffer->getFrameCount()));
        loadSpan->setSuccess();
    }

    debug("Audio buffer loaded successfully: {} frames", audioBuffer->getFrameCount());

    return Result<void>{};
}

std::shared_ptr<AudioTransport>
CooperativeAnimationScheduler::createAudioTransport(std::shared_ptr<PlaybackSession> /* session */) {

    // Check audio mode configuration
    auto audioMode = config->getAudioMode();

    if (audioMode == Configuration::AudioMode::RTP) {
        debug("Creating RTP audio transport");
        return std::make_shared<RtpAudioTransport>(rtpServer);
    } else {
        debug("Creating local SDL audio transport");
        return std::make_shared<LocalSdlAudioTransport>();
    }
}

void CooperativeAnimationScheduler::setupLifecycleCallbacks(std::shared_ptr<PlaybackSession> session,
                                                            universe_t universe) {
    // Use weak_ptr to avoid circular reference
    std::weak_ptr<PlaybackSession> weakSession = session;

    // OnStart callback: turn on status light and start audio
    session->setOnStartCallback([weakSession, universe]() {
        debug("PlaybackSession starting for universe {}", universe);

        auto statusLightOn =
            std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, true);
        eventLoop->scheduleEvent(statusLightOn);

        // Start audio transport if present
        if (auto session = weakSession.lock()) {
            auto audioTransport = session->getAudioTransport();
            if (audioTransport) {
                auto startResult = audioTransport->start(session);
                if (!startResult.isSuccess()) {
                    warn("Failed to start audio transport: {}", startResult.getError()->getMessage());
                }
            }
        }
    });

    // OnFinish callback: turn off status light
    session->setOnFinishCallback([universe]() {
        debug("PlaybackSession finishing for universe {}", universe);

        auto statusLightOff =
            std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, false);
        eventLoop->scheduleEvent(statusLightOff);
    });
}

} // namespace creatures
