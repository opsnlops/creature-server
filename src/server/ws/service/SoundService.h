
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

#include "model/Sound.h"

#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures {
class RequestSpan;
} // namespace creatures

namespace creatures ::ws {

class SoundService {

  private:
    typedef oatpp::web::protocol::http::Status Status;

  public:
    SoundService() = default;
    virtual ~SoundService() = default;

    /**
     * Play a sound file for testing
     *
     * @param soundFile
     * @return
     */
    oatpp::Object<creatures::ws::StatusDto> playSound(const oatpp::String &soundFile);

    /**
     * Get all of the sound files
     */
    oatpp::Object<ListDto<oatpp::Object<creatures::SoundDto>>> getAllSounds();

    /**
     * Generate lip sync data for a sound file using Rhubarb Lip Sync
     *
     * @param soundFile The name of the sound file to process (must be .wav)
     * @param allowOverwrite Whether to allow overwriting an existing JSON file
     * @param parentSpan Optional parent span for tracing
     * @return StatusDto with the result or error details
     */
    oatpp::Object<creatures::ws::StatusDto> generateLipSync(const oatpp::String &soundFile, bool allowOverwrite,
                                                            std::shared_ptr<RequestSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
