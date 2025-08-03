
#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// Disable shadow warnings for MongoDB C++ driver headers (third-party code)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

#pragma GCC diagnostic pop

#include "model/Animation.h"

#include "server/namespace-stuffs.h"

namespace creatures {

std::string bytesToString(const std::string &id_bytes);
//    std::string animationIdToString(const AnimationId &animationId);
//    std::string creatureIdToString(const CreatureId &creature_id);
//    std::string playlistIdentifierToString(const PlaylistIdentifier
//    &playlistIdentifier); CreatureId stringToCreatureId(const std::string
//    &creatureIdString); PlaylistIdentifier stringToPlaylistIdentifier(const
//    std::string &playlistIdString); AnimationId stringToAnimationId(const
//    std::string &animationIdString);

/**
 * Converts the string version of an OID as seen in the MongoDB Compass
 * application, such as `6611dfb4e98d776bb0025304` to a `bsoncxx::oid`
 *
 * @param id_string the OID in string format
 * @return a `bsoncxx::oid` object
 * @throws creatures::InvalidArgumentException if the string is not the correct
 * length or invalid
 */
bsoncxx::oid stringToOid(const std::string &id_string);

/**
 * Convert an oid into the string representation of it
 *
 * @param oid the OID to convert
 * @return
 */
std::string oidToString(const bsoncxx::oid &oid);

/**
 * Convert a vector of strings to a BSON array
 *
 * @param vector
 * @return
 */
bsoncxx::document::value stringVectorToBson(const std::vector<std::string> &vector);

/**
 * Convert a BSON array to a vector of strings
 *
 * @param doc
 * @return
 */
std::vector<std::string> stringVectorFromBson(const bsoncxx::document::view &doc);

/*
 * Animations
 */

/**
 * Check if a file is readable
 *
 * @param path the file path to check
 * @return true if it exists and is readable, false otherwise
 */
bool fileIsReadable(const std::string &path);

bsoncxx::oid generateNewOid();

void displayFrames(const creatures::Animation &animation);

/**
 * Get the current time in ISO8601 format (JavaScript format)
 *
 * @return the current time in ISO8601 format
 */
std::string getCurrentTimeISO8601();

/**
 * Decode a string of bases64 data into a `std::vector<uint8_t>`. (ie, the type
 * that we normally use for sending data to the creature.)
 *
 * @param base64Data the string to decode
 * @return the decided data
 */
std::vector<uint8_t> decodeBase64(const std::string &base64Data);

/**
 * Generate a std::string out of a std::vector<uint8_t>. Useful for
 * debugging frames.
 *
 * @param byteVector the data to look at
 * @return A std::string in the format of "[ 0x01, 0x02, 0x03 ]"
 */
std::string vectorToHexString(const std::vector<uint8_t> &byteVector);
} // namespace creatures
