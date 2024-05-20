

#include <string>
#include <cstdlib>

#include <spdlog/spdlog.h>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/array/element.hpp>
#include <mongocxx/cursor.hpp>

#include <bsoncxx/builder/stream/document.hpp>

#include "server/database.h"
#include "exception/exception.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    Result<creatures::Creature> Database::creatureFromJson(json creatureJson) {

        debug("attempting to create a creature from JSON via creatureFromJson()");

        try {

            auto creature = Creature();
            creature.id = creatureJson["id"];
            debug("id: {}", creature.id);

            creature.name = creatureJson["name"];
            debug("name: {}", creature.name);

            creature.audio_channel = creatureJson["audio_channel"];
            debug("audio_channel: {}", creature.audio_channel);

            creature.channel_offset = creatureJson["channel_offset"];
            debug("channel_offset: {}", creature.channel_offset);

            if(creature.id.empty()) {
                std::string errorMessage = "Creature ID is empty";
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            if(creature.name.empty()) {
                std::string errorMessage = "Creature name is empty";
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }


            // Check and parse inputs
            if (creatureJson.contains("inputs")) {
                for (const auto& inputJson : creatureJson["inputs"]) {
                    auto input = Input();
                    input.slot = inputJson.value("slot", std::numeric_limits<uint16_t>::max());
                    input.width = inputJson.value("width", std::numeric_limits<uint8_t>::max());
                    input.name = inputJson.value("name", "-?-");
                    input.joystick_axis = inputJson.value("joystick_axis", std::numeric_limits<uint8_t>::max());

                    // Let's check this before going on
                    if (input.slot == std::numeric_limits<uint16_t>::max()) {
                        std::string errorMessage = "Input slot is missing or invalid";
                        warn(errorMessage);
                        return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                    }

                    if (input.width == std::numeric_limits<uint8_t>::max()) {
                        std::string errorMessage = "Input width is missing or invalid";
                        warn(errorMessage);
                        return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                    }

                    if (input.name.empty() || input.name == "-?-") {
                        std::string errorMessage = "Input name is missing";
                        warn(errorMessage);
                        return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                    }

                    if (input.joystick_axis == std::numeric_limits<uint8_t>::max()) {
                        std::string errorMessage = "Input joystick_axis is missing or invalid";
                        warn(errorMessage);
                        return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                    }

                    debug("adding input: {}, slot: {}, width: {}, axis: {}",
                          input.name, input.slot, input.width, input.joystick_axis);
                    creature.inputs.emplace_back(input);
                }
            } else {
                warn("No inputs for {} found in JSON", creature.name);
                // Don't fail, this isn't fatal
            }


            debug("✅ Looks good, I was able to build a creature from JSON");
            return Result<creatures::Creature>{creature};

        } catch ( const nlohmann::json::exception& e ) {
            std::string errorMessage = fmt::format("Error while converting JSON to Creature: {}", e.what());
            warn(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
    }
}