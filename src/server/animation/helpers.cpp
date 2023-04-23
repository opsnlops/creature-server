

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

namespace creatures {

    bsoncxx::document::value Database::animationToBson(const server::Animation *animation, bool assignNewId) {

        trace("converting an animation to BSON");

        bsoncxx::oid id;

        if(!assignNewId) {

            trace("attempting to re-use the old id");

            // If the _id is empty, don't go any further
            if (animation->_id().empty()) {
                error("Unable to save animation in database, _id is empty");
                throw DataFormatException("`_id` is empty");
            }
            trace("incoming id size: {}", animation->_id().size());
            id = bsoncxx::oid(animation->_id().data(), animation->_id().size());
        }

        bsoncxx::builder::stream::document doc{};
        try {

            // The `_id` is the top level, as Mongo expects
            doc << "_id" << id;
            trace("_id set");

            // Metadata
            doc << "metadata" << metadataToBson(animation);
            trace("metadata created, title: {}", animation->metadata().title());

            // Frames
            uint32_t frameCount = framesToBson(doc, animation);
            trace("done adding frames, added {} total", frameCount);

            // If we didn't save the number of frames we thought we should, yell
            if(frameCount != animation->metadata().number_of_frames()) {
                throw DataFormatException(
                        fmt::format("The number of frames in the metadata and in the file did not agree! (meta: {}, actual: {})",
                                    animation->metadata().number_of_frames(),
                                    frameCount)
                                    );
            }

            return doc.extract();
        }
        catch (const bsoncxx::exception& e) {
            error("Error encoding the animation to BSON: {}", e.what());
            throw DataFormatException(fmt::format("Error encoding the animation to BSON: {} ({})",
                                                  e.what(),
                                                  e.code().message()));
        }
    }

    /**
     * Convert the metadata on animation into BSON
     *
     * @param animation to extract the metadata from
     * @return a `bsoncxx::document::value` with the metadata
     */
    bsoncxx::document::value Database::metadataToBson(const Animation *animation) {

        try {
            auto metadata = bsoncxx::builder::stream::document{}
                    << "title" << animation->metadata().title()
                    << "millisecond_per_frame"
                    << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().milliseconds_per_frame())}
                    << "number_of_frames"
                    << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().number_of_frames())}
                    << "creature_type"
                    << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().creature_type())}
                    << "number_of_motors"
                    << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().number_of_motors())}
                    << "notes" << animation->metadata().notes()
                    << bsoncxx::builder::stream::finalize;

            return metadata;
        }
        catch (const bsoncxx::exception& e) {
            error("Error encoding the animation metadata to BSON: {}", e.what());
            throw e;
        }
    }

    uint32_t Database::framesToBson(bsoncxx::builder::stream::document &doc, const server::Animation *animation) {

        try {
            auto frames = doc << "frames" << bsoncxx::builder::stream::open_array;

            trace("starting to add frames");
            uint32_t frameCount = 0;

            for (const auto &f: animation->frames()) {
                const auto *byteData = reinterpret_cast<const uint8_t *>(f.bytes().data());
                size_t byteSize = f.bytes().size();
                std::vector<uint8_t> byteVector(byteData, byteData + byteSize);
                frames << bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                                   static_cast<uint32_t>(byteVector.size()), byteVector.data()};
                frameCount++;
            }

            frames << bsoncxx::builder::stream::close_array;

            return frameCount;
        }
        catch (const bsoncxx::exception& e) {
            error("Error encoding the animation frames to BSON: {}", e.what());
            throw e;
        }
    }

}
