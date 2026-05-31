#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "RhubarbData.h"
#include "SpeechGenerationManager.h"
#include "StreamingTTSClient.h"
#include "TextToViseme.h"
#include "model/Creature.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures {
class Database;
class Configuration;
class ObservabilityManager;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::voice {

/**
 * Result from streaming speech generation.
 *
 * Contains all the assets needed to build and play back an ad-hoc animation:
 * - The 17-channel WAV file (written to disk)
 * - Lip sync data (generated from ElevenLabs alignment, no Rhubarb needed)
 * - Creature metadata for animation construction
 */
struct StreamingSpeechResult {
    /// Path to the generated 17-channel WAV file
    std::filesystem::path wavPath;

    /// Path to the transcript text file
    std::filesystem::path transcriptPath;

    /// Lip sync data in Rhubarb-compatible format
    RhubarbSoundData lipSyncData;

    /// JSON string of lip sync data (Rhubarb-compatible format)
    std::string lipSyncJson;

    /// Audio duration in seconds
    double audioDurationSeconds = 0.0;

    /// Audio channel the creature's audio is placed on (1-16)
    uint16_t audioChannel = 1;

    /// The creature model
    creatures::Creature creature;

    /// The creature JSON for voice config
    nlohmann::json creatureJson;
};

/**
 * StreamingSpeechGenerationManager
 *
 * Replaces the REST TTS + Rhubarb pipeline for ad-hoc speech with a single
 * ElevenLabs WebSocket streaming call that provides both audio and alignment
 * data simultaneously.
 *
 * Pipeline comparison:
 *   OLD: ElevenLabs REST (2.8s) → ffmpeg MP3→WAV (52ms) → Rhubarb (3.8s) = ~6.7s
 *   NEW: ElevenLabs WebSocket (2-3s, audio+alignment in one call) = ~3s
 */
class StreamingSpeechGenerationManager {
  public:
    /**
     * Generate speech and lip sync data using ElevenLabs streaming.
     *
     * @param request Speech generation request (same as SpeechGenerationManager)
     * @return StreamingSpeechResult with WAV file + lip sync data
     */
    static Result<StreamingSpeechResult> generate(const SpeechGenerationRequest &request);

    // writePcmToMultichannelWav lives in voice::PcmWavWriter as a free
    // function (extracted #12). decodeMp3ToMultichannelWav was retired with
    // the MP3-via-ffmpeg pipeline.
};

} // namespace creatures::voice
