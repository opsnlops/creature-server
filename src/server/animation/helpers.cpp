
#include "spdlog/spdlog.h"

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
     * This function is responsible serializing an animation to BSON
     *
     * @param Animation* animation  // The animation to be handled.
     * @param int animationId // The unique identifier for the animation.
     *
     * @return bool // Returns true if the animation is successfully handled, false otherwise.
     */
    bsoncxx::document::value Database::animationToBson(const server::Animation *animation, bsoncxx::oid animationId) {

        trace("converting an animation to BSON");

        bsoncxx::builder::stream::document doc{};
        try {

            // The `_id` is the top level, as Mongo expects
            doc << "_id" << animationId;
            trace("_id set");

            // Metadata
            doc << "metadata" << metadataToBson(animation, animationId);
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
    bsoncxx::document::value Database::metadataToBson(const Animation *animation, bsoncxx::oid animationId) {

        try {

            // Grab the ID of the parent animation
            //bsoncxx::oid animation_id = bsoncxx::oid(animation->_id().data(), animation->_id().size());

            auto metadata = bsoncxx::builder::stream::document{}
                    << "title" << animation->metadata().title()
                    << "animation_id" << animationId
                    << "milliseconds_per_frame"
                    << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().milliseconds_per_frame())}
                    << "number_of_frames"
                    << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().number_of_frames())}
                    << "note" << animation->metadata().note()
                    << "sound_file" << animation->metadata().sound_file()
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

            for (const auto &f : animation->frames()) {
                std::vector<uint8_t> byteVector;
                for (const auto &byteBlock : f.bytes()) {
                    byteVector.insert(byteVector.end(), byteBlock.begin(), byteBlock.end());
                }
                frames << bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                                   static_cast<uint32_t>(byteVector.size()), byteVector.data()};
                frameCount++;
            }

            frames << bsoncxx::builder::stream::close_array;

            return frameCount;
        }
        catch (const bsoncxx::exception &e) {
            error("Error encoding the animation frames to BSON: {}", e.what());
            throw e;
        }
    }

    /**
     * Creates an Animation.Metadata from a BSON doc
     *
     * @param doc the doc to look at
     * @param metadata a Animation_Metadata to fill out
     */
    void Database::bsonToAnimationMetaData(const bsoncxx::document::view &doc, AnimationMetaData *metadata) {

        trace("attempting to build an Animation.Metadata from BSON");

        bsoncxx::document::element element = doc["title"];
        if (!element) {
            error("Animation.Metadata value 'title' is not found");
            throw DataFormatException("Animation.Metadata value 'title' is not found");
        }

        if (element.type() != bsoncxx::type::k_utf8) {
            error("Animation.Metadata value 'title' is not a string");
            throw creatures::DataFormatException("Animation.Metadata value 'title' is not a string");
        }
        bsoncxx::stdx::string_view string_value = element.get_string().value;
        metadata->set_title(std::string{string_value});
        trace("set the title to {}", metadata->title());


        // Extract the animation ID
        element = doc["animation_id"];
        if (element && element.type() != bsoncxx::type::k_oid) {
            error("Field `animation_id` was not an OID in the database while loading an animation metadata");
            throw DataFormatException("Field 'animation_id' was not a bsoncxx::oid in the database");
        }
        const bsoncxx::oid &oid = element.get_oid().value;
        const char *oid_data = oid.bytes();
        metadata->set_animationid(oid_data, bsoncxx::oid::k_oid_length);
        trace("set the animation_id to {}", oid.to_string());


        element = doc["milliseconds_per_frame"];
        if (!element) {
            error("Animation.Metadata value 'milliseconds_per_frame' is not found");
            throw creatures::DataFormatException("Animation.Metadata value 'milliseconds_per_frame' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'milliseconds_per_frame' is not an int");
            throw DataFormatException("Animation.Metadata value 'milliseconds_per_frame' is not an int");
        }
        metadata->set_milliseconds_per_frame(element.get_int32().value);
        trace("set the milliseconds_per_frame to {}", metadata->milliseconds_per_frame());


        element = doc["number_of_frames"];
        if (!element) {
            error("Animation.Metadata value 'number_of_frames' is not found");
            throw DataFormatException("Animation.Metadata value 'number_of_frames' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'number_of_frames' is not an int");
            throw DataFormatException("Animation.Metadata value 'number_of_frames' is not an int");
        }
        metadata->set_number_of_frames(element.get_int32().value);
        trace("set the number_of_frames to {}", metadata->number_of_frames());


        element = doc["number_of_motors"];
        if (!element) {
            error("Animation.Metadata value 'number_of_motors' is not found");
            throw DataFormatException("Animation.Metadata value 'number_of_motors' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'number_of_motors' is not an int");
            throw DataFormatException("Animation.Metadata value 'number_of_motors' is not an int");
        }
        metadata->set_number_of_motors(element.get_int32().value);
        trace("set the number_of_motors to {}", metadata->number_of_motors());




        // Creature type, which is an enum, so there's extra work to do
        element = doc["creature_type"];
        if (!element) {
            error("Animation.Metadata value 'creature_type' is not found");
            throw DataFormatException("Animation.Metadata value 'creature_type' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'creature_type' is not an int");
            throw DataFormatException("Animation.Metadata value 'creature_type' is not an int");
        }
        int32_t creature_type = element.get_int32().value;

        // Check if the integer matches up to CreatureType
        if (!server::CreatureType_IsValid(creature_type)) {
            error("Animation.Metadata field 'creature_type' does not map to our enum: {}", creature_type);
            throw DataFormatException(
                    fmt::format("Animation.Metadata field 'creature_type' does not map to our enum: {}", creature_type));
        }
        metadata->set_creature_type(static_cast<server::CreatureType>(creature_type));
        trace("set the creature type to {}", toascii(metadata->creature_type()));


        element = doc["sound_file"];
        if (!element) {
            error("Animation.Metadata value 'sound_file' is not found");
            throw DataFormatException("Animation.Metadata value 'sound_file' is not found");
        }
        if (element.type() != bsoncxx::type::k_utf8) {
            error("Animation.Metadata value 'sound_file' is not an int");
            throw DataFormatException("Animation.Metadata value 'sound_file' is not a string");
        }
        string_value = element.get_string().value;
        metadata->set_sound_file(std::string{string_value});
        trace("set the sound_file to {}", metadata->sound_file());


        // And finally, the notes!
        element = doc["notes"];
        if (!element) {
            error("Animation.Metadata value 'notes' is not found");
            throw DataFormatException("Animation.Metadata value 'notes' is not found");
        }
        if (element.type() != bsoncxx::type::k_utf8) {
            error("Animation.Metadata value 'notes' is not an int");
            throw DataFormatException("Animation.Metadata value 'notes' is not a string");
        }
        string_value = element.get_string().value;
        metadata->set_note(std::string{string_value});
        trace("set the notes to {}", metadata->note());


        trace("all done creating a metadata from BSON!");

    }


    /**
     * Loads frame data from MongoDB
     *
     * @param doc a bsoncxx::document::view with an element called "frames" to read
     * @param animation the animation to populate
     */
    void Database::populateFramesFromBson(const bsoncxx::document::view &doc, Animation *animation) {

        trace("trying to populate the frames from BSON");

        try {
            auto framesView = doc["frames"].get_array().value;

            for (const auto &frameElem : framesView) {
                const bsoncxx::types::b_binary frameBinary = frameElem.get_binary();

                // Ensure the binary data is of the expected subtype
                if (frameBinary.sub_type != bsoncxx::binary_sub_type::k_binary) {
                    error("Frame binary data has an unexpected sub-type");
                    throw DataFormatException("Frame binary data has an unexpected sub-type");
                }

                // Create a new Frame protobuf message
                server::Animation::Frame* frame = animation->add_frames();

                // Add the binary data to the bytes field
                frame->add_bytes(reinterpret_cast<const char*>(frameBinary.bytes), frameBinary.size);

            }
            trace("loaded {} byte arrays", animation->frames().size());
        }
        catch (const bsoncxx::exception& e) {
            error("Error decoding the animation frames from BSON: {}", e.what());
            throw DataFormatException(fmt::format("Error decoding the animation frames from BSON: {}", e.what()));
        }
    }



}
