
#pragma once

#include <atomic>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


/**
 * A helper class to keep track of some counters for system usage
 */
namespace creatures {

    /*
     * This one is weird. The object is UNDER the DTO.
     */


#include OATPP_CODEGEN_BEGIN(DTO)

    class SystemCountersDto : public oatpp::DTO {

        DTO_INIT(SystemCountersDto, DTO /* extends */)

        DTO_FIELD_INFO(totalFrames) {
            info->description = "Number of frames that have been processed";
        }
        DTO_FIELD(UInt64, totalFrames);

        DTO_FIELD_INFO(eventsProcessed) {
            info->description = "Number of events that have been processed by the event loop";
        }
        DTO_FIELD(UInt64, eventsProcessed);

        DTO_FIELD_INFO(framesStreamed) {
            info->description = "Number of streaming frames that have been received from clients";
        }
        DTO_FIELD(UInt64, framesStreamed);

        DTO_FIELD_INFO(dmxEventsProcessed) {
            info->description = "Number of DMX events that have been processed";
        }
        DTO_FIELD(UInt64, dmxEventsProcessed);

        DTO_FIELD_INFO(animationsPlayed) {
            info->description = "Number of animations that have been played";
        }
        DTO_FIELD(UInt64, animationsPlayed);

        DTO_FIELD_INFO(soundsPlayed) {
            info->description = "Number of sounds that have been played";
        }
        DTO_FIELD(UInt64, soundsPlayed);

        DTO_FIELD_INFO(playlistsStarted) {
            info->description = "Number of playlists that have been started";
        }
        DTO_FIELD(UInt64, playlistsStarted);

        DTO_FIELD_INFO(playlistsStopped) {
            info->description = "Number of playlists that have been stopped";
        }
        DTO_FIELD(UInt64, playlistsStopped);

        DTO_FIELD_INFO(playlistsEventsProcessed) {
            info->description = "Number of events that have been processed by the playlist system";
        }
        DTO_FIELD(UInt64, playlistsEventsProcessed);

        DTO_FIELD_INFO(playlistStatusRequests) {
            info->description = "Number of requests for playlist status";
        }
        DTO_FIELD(UInt64, playlistStatusRequests);

        DTO_FIELD_INFO(restRequestsProcessed) {
            info->description = "Number of RESTful requests that have been processed";
        }
        DTO_FIELD(UInt64, restRequestsProcessed);

    };

#include OATPP_CODEGEN_END(DTO)



    class SystemCounters {

    public:
        SystemCounters();
        ~SystemCounters() = default;

        void incrementTotalFrames();
        void incrementEventsProcessed();
        void incrementFramesStreamed();
        void incrementDMXEventsProcessed();
        void incrementAnimationsPlayed();
        void incrementSoundsPlayed();
        void incrementPlaylistsStarted();
        void incrementPlaylistsStopped();
        void incrementPlaylistsEventsProcessed();
        void incrementPlaylistStatusRequests();
        void incrementRestRequestsProcessed();

        uint64_t getTotalFrames();
        uint64_t getEventsProcessed();
        uint64_t getFramesStreamed();
        uint64_t getDMXEventsProcessed();
        uint64_t getAnimationsPlayed();
        uint64_t getSoundsPlayed();
        uint64_t getPlaylistsStarted();
        uint64_t getPlaylistsStopped();
        uint64_t getPlaylistsEventsProcessed();
        uint64_t getPlaylistStatusRequests();
        uint64_t getRestRequestsProcessed();

        // This one is different for how it gets to a DTO since it's not a normal type of object
        std::shared_ptr<SystemCountersDto> convertToDto();

    private:
        std::atomic<uint64_t> totalFrames;
        std::atomic<uint64_t> eventsProcessed;
        std::atomic<uint64_t> framesStreamed;
        std::atomic<uint64_t> dmxEventsProcessed;
        std::atomic<uint64_t> animationsPlayed;
        std::atomic<uint64_t> soundsPlayed;
        std::atomic<uint64_t> playlistsStarted;
        std::atomic<uint64_t> playlistsStopped;
        std::atomic<uint64_t> playlistsEventsProcessed;
        std::atomic<uint64_t> playlistStatusRequests;
        std::atomic<uint64_t> restRequestsProcessed;
    };





}