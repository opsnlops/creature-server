
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
            appendMetadataToAnimationDoc(doc, animation, animationId);
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
     * Append the animation's metadata to an existing BSON document builder
     *
     * @param doc The document to append to
     * @param animation the animation we're serializing
     * @param animationId the animation's unique identifier
     */
    void Database::appendMetadataToAnimationDoc(bsoncxx::builder::stream::document& doc,
                                                const Animation *animation,
                                                bsoncxx::oid animationId) {

        try {
            debug("appending metadata to the animation doc");
            doc << "animation_id" << animationId
                << "title" << animation->metadata().title()
                << "last_updated" << bsoncxx::types::b_date{protobufTimestampToTimePoint(animation->metadata().last_updated())}
                << "milliseconds_per_frame"
                << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().milliseconds_per_frame())}
                << "number_of_frames"
                << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().number_of_frames())}
                << "note" << animation->metadata().note()
                << "sound_file" << animation->metadata().sound_file()
                << "multitrack_audio" << bsoncxx::types::b_bool{animation->metadata().multitrack_audio()};;

        }
        catch (const bsoncxx::exception& e) {
            error("Error encoding the animation metadata to BSON: {}", e.what());
            throw e;
        }
    }

    uint32_t Database::framesToBson(bsoncxx::builder::stream::document& doc, const Animation* animation) {
        try {
            auto framesArrayBuilder = doc << "frames" << bsoncxx::builder::stream::open_array;

            trace("starting to add frames");
            uint32_t frameCount = 0;

            // Iterate over each FrameData object in the animation
            for (const auto& frameData : animation->frames()) {
                auto frameDataBuilder = bsoncxx::builder::stream::document{};

                // Convert the string OIDs to bsoncxx::oid and add them to the BSON document
                frameDataBuilder << "_id" << bsoncxx::oid(frameData._id())
                                 << "creature_id" << bsoncxx::oid(frameData.creature_id())
                                 << "animation_id" << bsoncxx::oid(frameData.animation_id());

                // The nested frames array construction remains the same
                auto nestedFramesArrayBuilder = frameDataBuilder << "frames" << bsoncxx::builder::stream::open_array;
                for (const auto& frameBytes : frameData.frames()) {
                    nestedFramesArrayBuilder << bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                                                         static_cast<uint32_t>(frameBytes.size()),
                                                                         reinterpret_cast<const uint8_t*>(frameBytes.data())};
                }
                nestedFramesArrayBuilder << bsoncxx::builder::stream::close_array;

                // Append the frame data document to the frames array
                framesArrayBuilder << frameDataBuilder.extract();
            }

            framesArrayBuilder << bsoncxx::builder::stream::close_array;

            return frameCount;
        }
        catch (const bsoncxx::exception& e) {
            error("Error encoding the animation frames to BSON: {}", e.what());
            throw;
        }
    }

    /**
     * Extract an AnimationMetadata object from a BSON document
     *
     * @param doc the doc to look at
     * @param metadata a AnimationMetadata to fill out
     */
    void Database::bsonToAnimationMetadata(const bsoncxx::document::view &doc, AnimationMetadata *metadata) {

        trace("attempting to build an AnimationMetadata from BSON");

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
        metadata->set_title(std::string{string_value});
        trace("set the title to {}", metadata->title());



        // Handle last updated
        element = doc["last_updated"];

        // Convert the BSON date to a google::protobuf::Timestamp
        google::protobuf::Timestamp protobufTimestamp = convertMongoDateToProtobufTimestamp(element);

        // use mutable_last_updated to get a pointer to the existing Timestamp object
        google::protobuf::Timestamp* metadataTimestamp = metadata->mutable_last_updated();

        // Manually set the seconds and nanoseconds
        metadataTimestamp->set_seconds(protobufTimestamp.seconds());
        metadataTimestamp->set_nanos(protobufTimestamp.nanos());
        trace("set the last_updated to {}", metadata->last_updated().DebugString());





        // Extract the animation ID
        element = doc["animation_id"];
        if (element && element.type() != bsoncxx::type::k_oid) {
            error("Field `animation_id` was not an OID in the database while loading an animation metadata");
            throw DataFormatException("Field 'animation_id' was not a bsoncxx::oid in the database");
        }
        const bsoncxx::oid &oid = element.get_oid().value;
        const char *oid_data = oid.bytes();
        metadata->set_animation_id(oid_data, bsoncxx::oid::k_oid_length);
        trace("set the animation_id to {}", oid.to_string());


        element = doc["milliseconds_per_frame"];
        if (!element) {
            error("Animation.Metadata value 'milliseconds_per_frame' is not found");
            throw creatures::DataFormatException("AnimationMetadata value 'milliseconds_per_frame' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'milliseconds_per_frame' is not an int");
            throw DataFormatException("AnimationMetadata value 'milliseconds_per_frame' is not an int");
        }
        metadata->set_milliseconds_per_frame(element.get_int32().value);
        trace("set the milliseconds_per_frame to {}", metadata->milliseconds_per_frame());


        element = doc["number_of_frames"];
        if (!element) {
            error("Animation.Metadata value 'number_of_frames' is not found");
            throw DataFormatException("AnimationMetadata value 'number_of_frames' is not found");
        }
        if (element.type() != bsoncxx::type::k_int32) {
            error("Animation.Metadata value 'number_of_frames' is not an int");
            throw DataFormatException("AnimationMetadata value 'number_of_frames' is not an int");
        }
        metadata->set_number_of_frames(element.get_int32().value);
        trace("set the number_of_frames to {}", metadata->number_of_frames());



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
        metadata->set_sound_file(std::string{string_value});
        trace("set the sound_file to {}", metadata->sound_file());



        element = doc["multitrack_audio"];
        if (!element) {
            error("AnimationMetadata value 'multitrack_audio' is not found");
            throw DataFormatException("AnimationMetadata value 'multitrack_audio' is not found");
        }
        if (element.type() != bsoncxx::type::k_bool) {
            error("AnimationMetadata value 'multitrack_audio' is not a boolean");
            throw DataFormatException("AnimationMetadata value 'multitrack_audio' is not a boolean");
        }
        metadata->set_multitrack_audio(element.get_bool().value);
        trace("set the multitrack_audio to {}", metadata->multitrack_audio());


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
            auto framesArrayView = doc["frames"].get_array().value;

            // Loop through each FrameData object
            for (const auto &frameDataElem : framesArrayView) {
                auto frameDataDoc = frameDataElem.get_document().view();

                // Extract data fields
                bsoncxx::oid id = bsoncxx::oid(frameDataDoc["_id"].get_oid().value);
                bsoncxx::oid creatureId = bsoncxx::oid(frameDataDoc["creature_id"].get_oid().value);
                bsoncxx::oid animationId = bsoncxx::oid(frameDataDoc["animation_id"].get_oid().value);

                // Prepare a FrameData protobuf object
                auto frameData = animation->add_frames();
                frameData->set__id(id.bytes(), bsoncxx::oid::k_oid_length);
                frameData->set_creature_id(creatureId.bytes(), bsoncxx::oid::k_oid_length);
                frameData->set_animation_id(animationId.bytes(), bsoncxx::oid::k_oid_length);


                /// Iterate over the nested frames array within each FrameData document
                auto nestedFramesView = frameDataDoc["frames"].get_array().value;
                for (const auto &nestedFrameElem : nestedFramesView) {
                    const bsoncxx::types::b_binary frameBinary = nestedFrameElem.get_binary();

                    // Check the binary data's subtype
                    if (frameBinary.sub_type != bsoncxx::binary_sub_type::k_binary) {
                        error("Nested frame binary data has an unexpected sub-type");
                        throw DataFormatException("Nested frame binary data has an unexpected sub-type");
                    }

                    // Since the nested frame element is a binary object, add it directly to the protobuf object
                    frameData->add_frames(frameBinary.bytes, frameBinary.size);
                }
            }
            trace("loaded frame data for {} creature(s)", animation->frames_size());
        } catch (const bsoncxx::exception& e) {
            error("Error decoding the animation frames from BSON: {}", e.what());
            throw DataFormatException(fmt::format("Error decoding the animation frames from BSON: {}", e.what()));
        }
    }



}
