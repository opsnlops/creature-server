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

  private:
    /**
     * Write raw PCM audio data into a 17-channel WAV file.
     *
     * Places the audio on the creature's specific channel with silence
     * on all other channels, matching the format produced by AudioConverter.
     *
     * @param pcmData Raw PCM audio data (mono, 48kHz, 16-bit)
     * @param wavPath Output WAV file path
     * @param audioChannel Target channel (1-16, or 17 for BGM)
     * @param sampleRate Sample rate in Hz
     * @return Size of the written file, or error
     */
    static Result<size_t> writePcmToMultichannelWav(const std::vector<uint8_t> &pcmData,
                                                     const std::filesystem::path &wavPath, uint16_t audioChannel,
                                                     uint32_t sampleRate);

    /**
     * Decode MP3 data to PCM using ffmpeg, then write to multichannel WAV.
     *
     * @param mp3Data Raw MP3 audio data
     * @param wavPath Output WAV file path
     * @param audioChannel Target channel
     * @param parentSpan Observability span
     * @return Size of the written file, or error
     */
    static Result<size_t> decodeMp3ToMultichannelWav(const std::vector<uint8_t> &mp3Data,
                                                      const std::filesystem::path &wavPath, uint16_t audioChannel,
                                                      std::shared_ptr<OperationSpan> parentSpan);
};

} // namespace creatures::voice
