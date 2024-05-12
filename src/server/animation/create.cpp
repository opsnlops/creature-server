
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
#include "model/FrameData.h"

#include "server/namespace-stuffs.h"

#include "util/helpers.h"



namespace creatures {

    extern std::shared_ptr<Database> db;


    /**
     * Create a new animation in the database
     *
     * @param animation the `creatures::Animation` to save
     * @return a status message to return to the client
     */
    std::string Database::createAnimation(creatures::Animation animation) {

        debug("creating a new animation in the database");


        auto collection = getCollection(ANIMATIONS_COLLECTION);
        trace("collection obtained");

        // Create a BSON doc with this animation
        try {

            // Create the new animationId
            bsoncxx::oid animationId = generateNewOid();
            animation.id = oidToString(animationId);

            auto doc = animationToBson(animation);
            //trace("doc_value made: {}", bsoncxx::to_json(doc_view));

            collection.insert_one(doc.view());
            trace("run_command done");

            info("saved new animation in the database 💃🏽");
            return std::string{"✅ Saved new animation in the database"};
        }
        catch (const mongocxx::exception &e) {

            // Code 11000 means id collision
            if (e.code().value() == 11000) {

                std::string errorMessage = fmt::format("Unable to save animation due to duplicate ID: {}", e.what());
                error(errorMessage);
                throw creatures::DuplicateFoundError(errorMessage);


            } else {
                std::string errorMessage = fmt::format("Error updating database: {}", e.what());
                critical(errorMessage);
                throw creatures::InternalError(errorMessage);
            }
        }
        catch (creatures::DataFormatException &e) {
            std::string errorMessage = fmt::format("Data format error while creating an animation: {}", e.what());
            error(errorMessage);
            throw creatures::DataFormatException(errorMessage);

        }
        catch (const bsoncxx::exception &e) {
            std::string errorMessage = fmt::format("BSON error while creating an animation: {}", e.what());
            error(errorMessage);
            throw creatures::DatabaseError(errorMessage);
        }

    }

}