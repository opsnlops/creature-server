
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

using grpc::ServerContext;


namespace creatures {

    extern std::shared_ptr<Database> db;

    Status CreatureServerImpl::ListAnimations(ServerContext *context,
                                              const AnimationFilter *request,
                                              ListAnimationsResponse *response) {

        info("Listing the animations in the database");
        try {
            db->listAnimations(request, response);
            debug("animations listed");
            return grpc::Status(grpc::StatusCode::OK, "✅ Got all animations");

        } catch (const creatures::DataFormatException &e) {
            error("Data format exception while listing animations: {}", e.what());
            return grpc::Status(grpc::StatusCode::INTERNAL, e.what());

        } catch (const creatures::InternalError &e) {
            error("Internal error while listing animations: {}", e.what());
            return grpc::Status(grpc::StatusCode::INTERNAL, e.what());

        } catch (const creatures::NotFoundException &e) {
            error("Not found error while listing animations: {}", e.what());
            return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());

        } catch (...) {

            error("Unknown error while listing animations");
            return grpc::Status(grpc::StatusCode::INTERNAL, "Unknown error");
        }
    }

    /**
     * List animations in the database for a given creature type
     *
     * @param filter what CreatureType to look for
     * @param animationList the list to fill out
     * @return the status of this request
     */
    void Database::listAnimations(const AnimationFilter *filter, ListAnimationsResponse *animationList) {

        trace("attempting to list all of the animation for a filter ({})", toascii(filter->type()));

        grpc::Status status;

        uint32_t numberOfAnimationsFound = 0;

        try {
            auto collection = getCollection(ANIMATIONS_COLLECTION);
            trace("collection obtained");

            document query_doc{};
            document projection_doc{};
            document sort_doc{};

            // We only want to sort by name at this point
            sort_doc << "metadata.title" << 1;

            // We want the idea and metadata only (no frames)
            projection_doc << "_id" << 1 << "metadata" << 1;

            // We only want documents of the given creature type
            query_doc << "metadata.creature_type" << bsoncxx::types::b_int32{static_cast<int32_t>(filter->type())};

            mongocxx::options::find findOptions{};
            findOptions.projection(projection_doc.view());
            findOptions.sort(sort_doc.view());

            mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);

            // Go Mongo, go! 🎉
            for (auto &&doc: cursor) {

                auto animationId = animationList->add_animations();

                // Extract the ID
                bsoncxx::document::element element = doc["_id"];
                if (element && element.type() != bsoncxx::type::k_oid) {
                    error("Field `_id` was not an OID in the database");
                    throw DataFormatException("Field '_id' was not a bsoncxx::oid in the database");
                }
                const bsoncxx::oid &oid = element.get_oid().value;
                const char *oid_data = oid.bytes();
                animationId->set__id(oid_data, bsoncxx::oid::k_oid_length);
                trace("set the _id to {}", oid.to_string());

                // Grab the metadata
                element = doc["metadata"];

                Animation_Metadata metadata = Animation_Metadata();

                bsonToAnimationMetaData(element.get_document().view(), &metadata);
                *animationId->mutable_metadata() = metadata;

                debug("loaded {}", animationId->metadata().title());
                numberOfAnimationsFound++;
            }
        }
        catch(const DataFormatException& e) {
            std::string errorMessage = fmt::format("Failed to get all animations: {}", e.what());
            warn(errorMessage);
            throw creatures::DataFormatException(errorMessage);
        }
        catch(const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("MongoDB Exception while loading animation: {}", e.what());
            critical(errorMessage);
            throw creatures::InternalError(errorMessage);
        }
        catch(const bsoncxx::exception &e) {
            std::string errorMessage = fmt::format("BSON error while attempting to load animations: {}", e.what());
            critical(errorMessage);
            throw creatures::InternalError(errorMessage);
        }

        // Return a 404 if nothing as found
        if(numberOfAnimationsFound == 0) {
            std::string errorMessage = fmt::format("No animations for that creature type found");
            warn(errorMessage);
            throw creatures::NotFoundException(errorMessage);
        }

        std::string okayMessage = fmt::format("✅ Found {} animations", numberOfAnimationsFound);
        info(okayMessage);
    }
}