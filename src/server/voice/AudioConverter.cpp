#include "AudioConverter.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "server/config.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

Result<std::uintmax_t> AudioConverter::convertMp3ToWav(const std::filesystem::path &mp3FilePath,
                                                       const std::filesystem::path &wavFilePath,
                                                       const std::string &ffmpegBinaryPath, int targetChannel,
                                                       int sampleRate, std::shared_ptr<OperationSpan> parentSpan) {

    // Create observability span
    auto span = observability->createChildOperationSpan("AudioConverter.convertMp3ToWav", parentSpan);
    if (span) {
        span->setAttribute("mp3_file", mp3FilePath.string());
        span->setAttribute("wav_file", wavFilePath.string());
        span->setAttribute("target_channel", targetChannel);
        span->setAttribute("sample_rate", sampleRate);
        span->setAttribute("total_channels", RTP_STREAMING_CHANNELS);
    }

    // Validate inputs
    if (!std::filesystem::exists(mp3FilePath)) {
        std::string errorMsg = fmt::format("MP3 file does not exist: {}", mp3FilePath.string());
        error(errorMsg);
        if (span)
            span->setError(errorMsg);
        return Result<std::uintmax_t>{ServerError(ServerError::NotFound, errorMsg)};
    }

    if (targetChannel < 1 || targetChannel > RTP_STREAMING_CHANNELS) {
        std::string errorMsg = fmt::format("Invalid target channel {}. Must be between 1 and {}", targetChannel,
                                           RTP_STREAMING_CHANNELS);
        error(errorMsg);
        if (span)
            span->setError(errorMsg);
        return Result<std::uintmax_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Build filter_complex to create 17-channel audio with input on target channel
    // Strategy:
    // 1. Create silent mono streams for each channel (anullsrc)
    // 2. Split the input audio
    // 3. Merge all streams using amerge, placing input audio at the correct position
    // 4. Use -shortest to match the duration of the input

    std::stringstream filterComplex;
    filterComplex.imbue(std::locale::classic()); // Use C locale to avoid comma formatting
    filterComplex << "[0:a]aformat=sample_rates=" << sampleRate << ":channel_layouts=mono[input];";
    filterComplex << "anullsrc=channel_layout=mono:sample_rate=" << sampleRate << "[null];";

    // Split null into enough copies for all silent channels
    int silentChannels = RTP_STREAMING_CHANNELS - 1;
    filterComplex << "[null]asplit=" << silentChannels;
    for (int i = 0; i < silentChannels; i++) {
        filterComplex << "[s" << i << "]";
    }
    filterComplex << ";";

    // Build the amerge inputs, placing [input] at the target channel position
    int silentIndex = 0;
    filterComplex << "["; // Start with opening bracket
    for (int i = 0; i < RTP_STREAMING_CHANNELS; i++) {
        if (i > 0) filterComplex << "][";

        if (i == (targetChannel - 1)) {
            filterComplex << "input";
        } else {
            filterComplex << "s" << silentIndex;
            silentIndex++;
        }
    }
    filterComplex << "]amerge=inputs=" << RTP_STREAMING_CHANNELS << "[out]";

    // Build ffmpeg command
    // -i: input file
    // -filter_complex: complex audio filter to create multi-channel output
    // -map: map the output of the filter
    // -acodec pcm_s16le: 16-bit PCM signed little-endian (not 32-bit float)
    // -shortest: terminate when the shortest input ends (the actual audio)
    // -y: overwrite output file
    std::string ffmpegCommand =
        fmt::format("\"{}\" -i \"{}\" -filter_complex \"{}\" -map \"[out]\" -acodec pcm_s16le -shortest -y \"{}\" 2>&1",
                    ffmpegBinaryPath, mp3FilePath.string(), filterComplex.str(), wavFilePath.string());

    debug("Executing ffmpeg: {}", ffmpegCommand);

    // Execute ffmpeg
    FILE *ffmpegPipe = popen(ffmpegCommand.c_str(), "r");
    if (!ffmpegPipe) {
        std::string errorMsg =
            fmt::format("Failed to execute ffmpeg at {}. Is it installed? errno: {}", ffmpegBinaryPath, errno);
        error(errorMsg);
        if (span)
            span->setError(errorMsg);
        return Result<std::uintmax_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Read ffmpeg output for logging
    std::string ffmpegOutput;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), ffmpegPipe) != nullptr) {
        ffmpegOutput += buffer;
    }

    int ffmpegExitCode = pclose(ffmpegPipe);
    debug("ffmpeg exited with code: {}", ffmpegExitCode);

    if (ffmpegExitCode != 0) {
        std::string errorMsg =
            fmt::format("ffmpeg conversion failed with exit code {}.\n\nOutput:\n{}", ffmpegExitCode, ffmpegOutput);
        error(errorMsg);
        if (span)
            span->setError(errorMsg);
        return Result<std::uintmax_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Verify WAV file was created and get its size
    if (!std::filesystem::exists(wavFilePath)) {
        std::string errorMsg =
            fmt::format("ffmpeg completed but did not create WAV file: {}", wavFilePath.string());
        error(errorMsg);
        if (span)
            span->setError(errorMsg);
        return Result<std::uintmax_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    auto wavFileSize = std::filesystem::file_size(wavFilePath);

    info("Successfully converted MP3 to WAV: {} -> {} ({} bytes, channel {})", mp3FilePath.filename().string(),
         wavFilePath.filename().string(), wavFileSize, targetChannel);

    if (span) {
        span->setAttribute("output_size_bytes", static_cast<int64_t>(wavFileSize));
        span->setSuccess();
    }

    return Result<std::uintmax_t>{wavFileSize};
}

} // namespace creatures::voice
