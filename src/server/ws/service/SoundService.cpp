

#include <iostream>
#include <filesystem>


#include "exception/exception.h"


#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"

#include "server/config/Configuration.h"

#include "model/Sound.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

#include "util/helpers.h"


#include "SoundService.h"


namespace creatures {
    extern std::shared_ptr<creatures::Configuration> config;
    extern std::shared_ptr<EventLoop> eventLoop;
}

namespace fs = std::filesystem;

namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;


    oatpp::Object<ListDto<oatpp::Object<creatures::SoundDto>>> SoundService::getAllSounds() {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->info("Request to return a list of the sound files");

        // Copy the path locally
        std::string path = config->getSoundFileLocation();

        // Create the response to return
        auto soundList = oatpp::Vector<oatpp::Object<creatures::SoundDto>>::createShared();

        bool error = false;
        Status status = Status::CODE_200;
        oatpp::String message;

        try {
            if (fs::exists(path) && fs::is_directory(path)) {
                for (const auto& entry : fs::directory_iterator(path)) {
                    const auto& filepath = entry.path();
                    if (fs::is_regular_file(entry.status())) {
                        auto filename = filepath.filename();  // Get the filename
                        auto size = fs::file_size(filepath);  // Get the file size

                        Sound sound{filename, (uint32_t)size};

                        appLogger->debug("Adding sound file: {} ({})", sound.fileName, sound.size);
                        soundList->emplace_back(creatures::convertSoundToDto(sound));
                    }
                }

                debug("found {} sound files", soundList->size());

            } else {
                warn("Sound file location not found: {}", path);

                status = Status::CODE_404;
                message = fmt::format("No files found in {}", path);
                error = true;
            }
        } catch (const fs::filesystem_error& e) {

            appLogger->error("Error reading sound file location: {}", e.what());

            status = Status::CODE_500;
            message = fmt::format("Error reading sound file location: {}", e.what());
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, message);

        // All done!
        auto list = ListDto<oatpp::Object<creatures::SoundDto>>::createShared();
        list->count = soundList->size();
        list->items = soundList;

        appLogger->debug("Returning {} sound files", list->count);
        return list;
    }



    /**
     * Schedule a sound to play on the next frame
     *
     * @param inSoundFile
     * @return a message telling which frame it will be played on
     */
    oatpp::Object<creatures::ws::StatusDto> SoundService::playSound(const oatpp::String &inSoundFile) {

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        std::string soundFile = std::string(inSoundFile);

        appLogger->info("Request to play sound file: {}", soundFile);


        // Fill out the full path to the file
        std::string fullFilePath = config->getSoundFileLocation() + "/" + inSoundFile;
        debug("using sound file name: {}", fullFilePath);


        // Make sure the file exists and is readable
        OATPP_ASSERT_HTTP(fileIsReadable(fullFilePath),
                          Status::CODE_404,
                          fmt::format("Sound file not found: {}", soundFile));

        bool error = false;
        oatpp::String message;

        try {
            uint64_t frameNumber = eventLoop->getNextFrameNumber();

            // Create the event and schedule it
            auto playEvent = std::make_shared<MusicEvent>(frameNumber, fullFilePath);
            eventLoop->scheduleEvent(playEvent);

            debug("scheduled sound to play on frame {}", frameNumber);

            message = fmt::format("Scheduled {} for frame {}", soundFile, frameNumber);

        }
        catch (const creatures::InternalError &e) {
            message = fmt::format("Internal error: {}", e.what());
            appLogger->error(std::string(message));
            error = true;
        }
        catch (const creatures::DataFormatException &e) {
            message = fmt::format("Data format error: {}", e.what());
            appLogger->error(std::string(message));
            error = true;
        }
        catch (...) {
            message = fmt::format("Unknown error");
            appLogger->error(std::string(message));
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, Status::CODE_500, message)



        auto response = StatusDto::createShared();
        response->code = 200;
        response->message = message;
        response->status = "OK";

        debug("returning a 200");
        return response;

    }


} // creatures :: ws