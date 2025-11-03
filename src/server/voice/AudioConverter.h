#pragma once

#include <filesystem>
#include <string>

#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * @brief Utility class for audio format conversions
 *
 * Provides functions to convert between audio formats using external tools like ffmpeg.
 * All operations include OpenTelemetry spans for observability.
 */
class AudioConverter {
  public:
    /**
     * @brief Convert a mono MP3 file to a 17-channel WAV format using ffmpeg
     *
     * Converts a mono MP3 file (typically from ElevenLabs) to a 17-channel WAV file
     * with the audio placed on a specific target channel. All other channels will be silent.
     * This is used for creature-specific audio where each creature listens to a dedicated channel.
     *
     * @param mp3FilePath Path to the input MP3 file (mono)
     * @param wavFilePath Path for the output WAV file (17 channels)
     * @param ffmpegBinaryPath Path to the ffmpeg binary
     * @param targetChannel Which channel (1-17) to place the audio on (1=creature 1, 17=BGM)
     * @param sampleRate Sample rate for the output WAV file (default: 48000 Hz)
     * @param parentSpan Optional parent span for tracing
     * @return Result<std::uintmax_t> containing the WAV file size on success, or an error
     */
    static Result<std::uintmax_t> convertMp3ToWav(const std::filesystem::path &mp3FilePath,
                                                  const std::filesystem::path &wavFilePath,
                                                  const std::string &ffmpegBinaryPath,
                                                  int targetChannel,
                                                  int sampleRate = 48000,
                                                  std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * @brief Determine the channel count of an audio file using ffmpeg.
     *
     * @param audioFilePath Path to the audio file to inspect.
     * @param ffmpegBinaryPath Path to the ffmpeg binary.
     * @param parentSpan Optional parent span for tracing.
     * @return Result<int> containing the detected channel count on success.
     */
    static Result<int> getChannelCount(const std::filesystem::path &audioFilePath,
                                       const std::string &ffmpegBinaryPath,
                                       std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * @brief Extract a single channel from a multichannel audio file into a mono WAV.
     *
     * @param sourcePath Path to the multichannel audio file.
     * @param outputPath Destination path for the mono WAV file.
     * @param ffmpegBinaryPath Path to the ffmpeg binary.
     * @param channelIndex One-based channel index to extract (1 = first channel).
     * @param parentSpan Optional parent span for tracing.
     * @return Result<void> indicating success or failure.
     */
    static Result<void> extractChannelToMono(const std::filesystem::path &sourcePath,
                                             const std::filesystem::path &outputPath,
                                             const std::string &ffmpegBinaryPath,
                                             int channelIndex,
                                             std::shared_ptr<OperationSpan> parentSpan = nullptr);
};

} // namespace creatures::voice
