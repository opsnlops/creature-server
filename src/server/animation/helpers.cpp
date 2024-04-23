
#include "spdlog/spdlog.h"

#include <base64.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/helpers.h"

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
    bsoncxx::document::value Database::animationToBson(const creatures::Animation &animation) {

        debug("converting an animation to BSON");

        bsoncxx::builder::stream::document doc{};
        try {

            // The `_id` is the top level, as Mongo expects
            doc << "_id" << stringToOid(animation.id);
            trace("_id set");

            // Metadata
            doc << "metadata" << animationMetadataToBson(animation.metadata);
            trace("metadata created, title: {}", animation.metadata.title);

            // Go load each track
            for(const auto& track : animation.tracks) {
                doc << "frames" << frameDataToBson(track);
            }
            trace("added the frame data");

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
     * Create the BSON representation of an `AnimationMetadata`
     *
     * @param metadata the `AnimationMetadata` to serialize
     * @return a `bsoncxx::document::value` with the metadata
     */
    bsoncxx::document::value animationMetadataToBson(const creatures::AnimationMetadata &metadata) {

        debug("attempting to serial an AnimationMetadata to BSON");

        // Do some light error checking
        if(metadata.animation_id.empty()) {
            error("Unable to serialize AnimationMetadata because it does not have an animation_id");
            throw DataFormatException("Unable to serialize AnimationMetadata because it does not have an animation_id");
        }

        bsoncxx::builder::stream::document builder{};

        try {
            builder << "animation_id" << metadata.animation_id
                << "title" << metadata.title
                << "milliseconds_per_frame"
                << bsoncxx::types::b_int32{static_cast<int32_t>(metadata.milliseconds_per_frame)}
                << "number_of_frames"
                << bsoncxx::types::b_int32{static_cast<int32_t>(metadata.number_of_frames)}
                << "note" << metadata.note
                << "sound_file" << metadata.sound_file
                << "multitrack_audio" << metadata.multitrack_audio;

            return builder.extract();

        }
        catch (const bsoncxx::exception& e) {
            error("Unable to encode an AnimationMetadata to BSON: {}", e.what());
            throw e;
        }

    }


    bsoncxx::document::value frameDataToBson(const creatures::FrameData &frameData) {

        if(frameData.id.empty()) {
            error("Unable to serialize FrameData because it does not have an id");
            throw DataFormatException("Unable to serialize FrameData because it does not have an id");
        }

        bsoncxx::builder::stream::document doc{};
        try {

            // Include the easy stuff :)
            doc << "_id" << stringToOid(frameData.id)
                << "creature_id" << stringToOid(frameData.creature_id)
                << "animation_id" << stringToOid(frameData.animation_id)
                << "frames" << stringVectorToBson(frameData.frames);

            return doc.extract();

        }
        catch (const bsoncxx::exception& e) {
            error("Error encoding a FrameData to BSON: {}", e.what());
            throw;
        }
    }

    /**
     * Extract an AnimationMetadata object from a BSON document
     *
     * @param doc the doc to look at
     */
    creatures::AnimationMetadata Database::animationMetadataFromBson(const bsoncxx::document::element &doc) {

        debug("attempting to build an AnimationMetadata from BSON");

        auto metadata = creatures::AnimationMetadata();

        bsoncxx::document::element element = doc["title"];
        if (!element) {
            error("AnimationMetadata value 'title' is not found");
            throw DataFormatException("AnimationMetadata value 'title' is not found");
        }

        if (element.type() != bsoncxx::type::k_utf8) {
            error("Animation.Metadata value 'title' is not a string");
            throw creatures::DataFormatException("AnimationMetadata value 'title' is not a string");
        }
        bsoncxx::stdx::string_view string_value = element.get_string().value;
        metadata.title = std::string{string_value};
        trace("set the title to {}", metadata.title);




        // Extract the animation ID
        element = doc["animation_id"];
        if (element && element.type() != bsoncxx::type::k_oid) {
            error("Field `animation_id` was not an OID in the database while loading an animation metadata");
            throw DataFormatException("Field 'animation_id' was not a bsoncxx::oid in the database");
        }
        const bsoncxx::oid &oid = element.get_oid().value;
        metadata.animation_id = oidToString(oid);
        trace("set the animation_id to {}", metadata.animation_id);


        element = doc["milliseconds_per_frame"];
        if (!element) {
            error("Animation.Metadata value 'milliseconds_per_frame' is not found");
            throw creatures::DataFormatException("AnimationMetadata value 'milliseconds_per_frame' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'milliseconds_per_frame' is not an int");
            throw DataFormatException("AnimationMetadata value 'milliseconds_per_frame' is not an int");
        }
        metadata.milliseconds_per_frame = element.get_int32().value;
        trace("set the milliseconds_per_frame to {}", metadata.milliseconds_per_frame);


        element = doc["number_of_frames"];
        if (!element) {
            error("Animation.Metadata value 'number_of_frames' is not found");
            throw DataFormatException("AnimationMetadata value 'number_of_frames' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'number_of_frames' is not an int");
            throw DataFormatException("AnimationMetadata value 'number_of_frames' is not an int");
        }
        metadata.number_of_frames = element.get_int32().value;
        trace("set the number_of_frames to {}", metadata.number_of_frames);



        element = doc["sound_file"];
        if (!element) {
            error("AnimationMetadata value 'sound_file' is not found");
            throw DataFormatException("AnimationMetadata value 'sound_file' is not found");
        }
        if (element.type() != bsoncxx::type::k_utf8) {
            error("AnimationMetadata value 'sound_file' is not an int");
            throw DataFormatException("AnimationMetadata value 'sound_file' is not a string");
        }
        string_value = element.get_string().value;
        metadata.sound_file = std::string{string_value};
        trace("set the sound_file to {}", metadata.sound_file);



        element = doc["multitrack_audio"];
        if (!element) {
            error("AnimationMetadata value 'multitrack_audio' is not found");
            throw DataFormatException("AnimationMetadata value 'multitrack_audio' is not found");
        }
        if (element.type() != bsoncxx::type::k_bool) {
            error("AnimationMetadata value 'multitrack_audio' is not a boolean");
            throw DataFormatException("AnimationMetadata value 'multitrack_audio' is not a boolean");
        }
        metadata.multitrack_audio = element.get_bool().value;
        trace("set the multitrack_audio to {}", metadata.multitrack_audio);


        // And finally, the note!
        element = doc["note"];
        if (!element) {
            error("AnimationMetadata value 'note' is not found");
            throw DataFormatException("AnimationMetadata value 'notes' is not found");
        }
        if (element.type() != bsoncxx::type::k_utf8) {
            error("AnimationMetadata value 'notes' is not an string");
            throw DataFormatException("AnimationMetadata value 'notes' is not a string");
        }
        metadata.note = std::string{element.get_string().value};
        trace("set the notes to {}", metadata.note);

        trace("all done creating a metadata from BSON!");

        return metadata;
    }


    /**
     * Loads frame data from MongoDB
     *
     * @param doc a bsoncxx::document::view with an element called "frames" to read
     */
    FrameData Database::frameDataFromBson(const bsoncxx::document::view &doc) {
        trace("trying to populate the frames from BSON");

        auto frameData = FrameData();


        try {

            frameData.id = oidToString(doc["_id"].get_oid().value);
            frameData.animation_id = oidToString(doc["animation_id"].get_oid().value);
            frameData.creature_id = oidToString(doc["creature_id"].get_oid().value);

            frameData.frames = stringVectorFromBson(doc["frames"].get_array().value);

            debug("loaded frame data, {} tracks found", frameData.frames.size());

        } catch (const bsoncxx::exception& e) {
            error("Error decoding the animation frames from BSON: {}", e.what());
            throw DataFormatException(fmt::format("Error decoding the animation frames from BSON: {}", e.what()));
        }

        return frameData;
    }



}
