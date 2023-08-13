#include "server/config.h"

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
}