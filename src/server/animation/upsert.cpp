
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>

#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"


#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Track.h"

#include "server/namespace-stuffs.h"
#include "util/websocketUtils.h"




namespace creatures {

    extern std::shared_ptr<Database> db;


    /**
     * Create a new animation in the database
     *
     * @param animation the `creatures::Animation` to save
     * @return a status message to return to the client
     */
     Result<creatures::Animation> Database::upsertAnimation(const std::string& animationJson) {

        debug("upserting an animation in the database");


        try {
            auto jsonObject = nlohmann::json::parse(animationJson);

            auto animation = animationFromJson(jsonObject);
            if (!animation.isSuccess()) {
                auto error = animation.getError();
                warn("Error while creating an animation from JSON: {}", error->getMessage());
                return Result<creatures::Animation>{ServerError(ServerError::InvalidData, error->getMessage())};
            }

            // Now go save it in Mongo
            auto collection = getCollection(ANIMATIONS_COLLECTION);
            trace("collection obtained");

            // Convert the JSON string into BSON
            auto bsonDoc = bsoncxx::from_json(animationJson);

            bsoncxx::builder::stream::document filter_builder;
            filter_builder << "id" << animation.getValue().value().id;

            // Upsert options
            mongocxx::options::update update_options;
            update_options.upsert(true);

            // Upsert operation
            collection.update_one(
                    filter_builder.view(),
                    bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view()
                                                         << bsoncxx::builder::stream::finalize,
                    update_options
            );

            info("Animation upserted in the database: {}", animation.getValue().value().id);
            return Result<creatures::Animation>{animation.getValue().value()};

        } catch (const mongocxx::exception &e) {
            error("Error (mongocxx::exception) while upserting an animation in database: {}", e.what());
            return Result<creatures::Animation>{ServerError(ServerError::InternalError, e.what())};
        } catch (const bsoncxx::exception &e) {
            error("Error (bsoncxx::exception) while upserting an animation in database: {}", e.what());
            return Result<creatures::Animation>{ServerError(ServerError::InvalidData, e.what())};
        } catch (...) {
            std::string error_message = "Unknown error while upserting an animation in the database";
            critical(error_message);
            return Result<creatures::Animation>{ServerError(ServerError::InternalError, error_message)};
        }
    }
}