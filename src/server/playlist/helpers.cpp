
#include "spdlog/spdlog.h"

#include <chrono>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"

#include "server/namespace-stuffs.h"

namespace creatures {

    /**
     * Convert a playlist into BSON
     *
     * @param playlist the playlist in question
     * @param playlistId it's id
     * @return A BSON document
     */
    bsoncxx::document::value Database::playlistToBson(const server::Playlist *playlist, bsoncxx::oid playlistId) {

        trace("converting a playlist to BSON");

        using bsoncxx::builder::stream::document;
        using std::chrono::system_clock;

        document doc{};
        try {

            system_clock::time_point last_updated = protobufTimestampToTimePoint(playlist->last_updated());

            // The `_id` is the top level, as Mongo expects
            doc << "_id" << playlistId;
            trace("_id set");

            doc << "name" << playlist->name()
            << "creature_type" << bsoncxx::types::b_int32{static_cast<int32_t>(playlist->creature_type())}
            << "last_updated" << bsoncxx::types::b_date{last_updated};

            // Add the items
            int32_t itemCount = playlistItemsToBson(doc, playlist);
            doc << "item_count" << bsoncxx::types::b_int32{itemCount};

            return doc.extract();
        }
        catch (const bsoncxx::exception &e) {
            error("Error encoding the playlist to BSON: {}", e.what());
            throw DataFormatException(fmt::format("Error encoding the playlist to BSON: {} ({})",
                                                  e.what(),
                                                  e.code().message()));
        }
    }


    int32_t Database::playlistItemsToBson(bsoncxx::builder::stream::document &doc, const server::Playlist *playlist) {

        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;

        try {
            auto items = doc << "items" << bsoncxx::builder::stream::open_array;

            trace("adding items to the playlist BSON");
            int32_t itemCount = 0;

            for (const auto &i : playlist->items()) {

                items << bsoncxx::builder::stream::open_document
                      << "animation_id" << bsoncxx::oid(i.animationid()._id().data(), bsoncxx::oid::k_oid_length)
                      << "weight" << bsoncxx::types::b_int32{static_cast<int32_t>(i.weight())}
                      << bsoncxx::builder::stream::close_document;

                itemCount++;
            }

            items << bsoncxx::builder::stream::close_array;

            debug("added {} items to the playlist BSON", itemCount);

            return itemCount;
        }
        catch (const bsoncxx::exception &e) {
            error("Error encoding the playlist items to BSON: {}", e.what());
            throw e;
        }
    }

    void Database::bsonToPlaylist(const bsoncxx::document::view &doc, server::Playlist *playlist) {
        using bsoncxx::types::b_oid;
        using bsoncxx::types::b_date;

        trace("converting a BSON document to a playlist");

        // Extract the ID
        auto element = doc["_id"];
        if (element && element.type() != bsoncxx::type::k_oid) {
            error("Field `_id` was not an OID in the database while loading an playlist");
            throw DataFormatException("Field '_id' was not a bsoncxx::oid in the database");
        }
        const bsoncxx::oid &oid = element.get_oid().value;
        const char *oid_data = oid.bytes();
        playlist->mutable__id()->set__id(oid_data, bsoncxx::oid::k_oid_length);
        trace("set the _id to {}", oid.to_string());

        // Getting name
        element = doc["name"];
        if (!element) {
            error("Playlist value 'name' is not found");
            throw DataFormatException("Playlist value 'name' is not found");
        }

        if (element.type() != bsoncxx::type::k_utf8) {
            error("Playlist value 'name' is not a string");
            throw creatures::DataFormatException("Playlist value 'name' is not a string");
        }
        bsoncxx::stdx::string_view string_value = element.get_string().value;
        playlist->set_name(std::string{string_value});
        trace("set the name to {}", playlist->name());

        // Creature type, which is an enum, so there's extra work to do
        element = doc["creature_type"];
        if (!element) {
            error("Playlist value 'creature_type' is not found");
            throw DataFormatException("Playlist value 'creature_type' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Playlist value 'creature_type' is not an int");
            throw DataFormatException("Playlist value 'creature_type' is not an int");
        }
        int32_t creature_type = element.get_int32().value;

        // Check if the integer matches up to CreatureType
        if (!server::CreatureType_IsValid(creature_type)) {
            error("Playlist field 'creature_type' does not map to our enum: {}", creature_type);
            throw DataFormatException(
                    fmt::format("Playlist field 'creature_type' does not map to our enum: {}", creature_type));
        }
        playlist->set_creature_type(static_cast<server::CreatureType>(creature_type));
        trace("set the creature type to {}", toascii(playlist->creature_type()));



        // Last updated
        element = doc["last_updated"];
        *playlist->mutable_last_updated() = convertMongoDateToProtobufTimestamp(element);



        // Now go get the playlist itens
        bsonToPlaylistItems(doc, playlist);
    }



    void Database::bsonToPlaylistItems(const bsoncxx::document::view &doc, server::Playlist *playlist) {

        trace("loading the items in a playlist from BSON");

        // Get the array of items
        auto itemsArray = doc["items"].get_array().value;

        for (const auto &itemDoc : itemsArray) {

            auto item = playlist->add_items(); // Create new item in playlist

            // Get the animation id
            auto element = itemDoc["animation_id"];
            if (element && element.type() != bsoncxx::type::k_oid) {
                error("Field `animation_id` was not an OID in the database while loading a playlist item");
                throw DataFormatException("Field 'animation_id' was not a bsoncxx::oid in the database");
            }
            const bsoncxx::oid &oid = element.get_oid().value;
            const char *oid_data = oid.bytes();
            item->mutable_animationid()->set__id(oid_data, bsoncxx::oid::k_oid_length);
            trace("set the animation_id to {}", oid.to_string());


            // Getting weight
            element = itemDoc["weight"];
            if (element && element.type() == bsoncxx::type::k_int32) {
                int32_t int32_value = element.get_int32().value;
                item->set_weight(int32_value);
                trace("set the animation weight to {}", item->weight());
            } else {
                error("playlist item field 'weight' was not an int32 in the database");
                throw creatures::DataFormatException("playlist item field 'weight' was not an int32 in the database");
            }

        }

        trace("done loading playlist items");
    }
}