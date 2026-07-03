
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

#include "model/Sound.h"

#include "server/ws/dto/AdHocSoundEntryDto.h"
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
     * Get all ad-hoc generated sound files.
     */
    oatpp::Object<AdHocSoundListDto> getAdHocSounds(std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Resolve the absolute path for an ad-hoc sound filename.
     */
    std::string resolveAdHocSoundPath(const std::string &filename, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Resolve the absolute path for a permanent-store sound by basename.
     *
     * Tries a top-level file first, then walks the permanent sound tree so that
     * sounds living in subdirectories (e.g. dialog/ renders) resolve too (#46).
     * Throws an HTTP 404 if nothing matches, 400 for an unsafe filename.
     */
    std::string resolvePermanentSoundPath(const std::string &filename,
                                          std::shared_ptr<RequestSpan> parentSpan = nullptr);

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
