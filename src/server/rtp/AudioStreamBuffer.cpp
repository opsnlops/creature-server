//
// AudioStreamBuffer.cpp
//

#include <cstring>
#include <filesystem>
#include <arpa/inet.h>  // For htons/ntohs

#include <SDL2/SDL.h>

#include "AudioChunk.h"
#include "AudioStreamBuffer.h"

#include "server/config.h"
#include "util/ObservabilityManager.h"

namespace creatures {
    extern std::shared_ptr<ObservabilityManager> observability;
}

namespace {
    // Utility functions for byte order conversion
    int16_t hostToNetworkInt16(int16_t hostValue) {
        // Convert to network byte order (big-endian)
        return static_cast<int16_t>(htons(static_cast<uint16_t>(hostValue)));
    }

    int16_t networkToHostInt16(int16_t networkValue) {
        // Convert from network byte order
        return static_cast<int16_t>(ntohs(static_cast<uint16_t>(networkValue)));
    }
}

namespace creatures :: rtp {

    AudioStreamBuffer::~AudioStreamBuffer() {
        chunks.clear();
    }

    bool AudioStreamBuffer::loadFile(const std::string &filePath, std::shared_ptr<OperationSpan> parentSpan) {

        if(!parentSpan) {
            warn("No parent span provided for AudioStreamBuffer.loadFile, creating a root span");
            parentSpan = observability->createOperationSpan("audio_stream_buffer.load_file");
        }

        auto span = observability->createChildOperationSpan("audio_stream_buffer.load_file", parentSpan);
        span->setAttribute("file_path", filePath);

        debug("Loading 16-bit PCM audio file for RTP streaming: {}", filePath);

        // Check if file exists and is readable
        if (!std::filesystem::exists(filePath)) {
            std::string errorMessage = fmt::format("Audio file does not exist: {}", filePath);
            error(errorMessage);
            span->setError(errorMessage);
            return false;
        }

        SDL_AudioSpec desiredSpec{};
        desiredSpec.freq = RTP_SRATE;        // 48000 Hz
        desiredSpec.format = AUDIO_S16;      // Load as 16-bit signed int (no conversion needed!)
        desiredSpec.channels = RTP_STREAMING_CHANNELS; // 17 channels
        desiredSpec.samples = 1024;          // Buffer size (not critical for loading)

        Uint8 *audioBuffer = nullptr;
        Uint32 audioLength = 0;

        // Load the WAV file directly as 16-bit
        auto loadSpan = observability->createChildOperationSpan("audio_stream_buffer.load_wav", span);
        loadSpan->setAttribute("file_path", filePath);
        if (!SDL_LoadWAV(filePath.c_str(), &audioSpec, &audioBuffer, &audioLength)) {
            std::string errorMessage = fmt::format("Failed to load WAV file {}: {}", filePath, SDL_GetError());
            error(errorMessage);
            loadSpan->setError(errorMessage);
            return false;
        }
        loadSpan->setSuccess();

        // === ENDIANNESS AND FORMAT DEBUGGING ===
        debug("=== ENDIANNESS DEBUG ===");
        debug("System byte order: {}", (SDL_BYTEORDER == SDL_LIL_ENDIAN) ? "Little Endian" : "Big Endian");
        debug("Audio format from file: 0x{:04X}", audioSpec.format);
        debug("SDL AUDIO_S16LSB: 0x{:04X}", AUDIO_S16LSB);
        debug("SDL AUDIO_S16MSB: 0x{:04X}", AUDIO_S16MSB);
        debug("SDL AUDIO_S16SYS: 0x{:04X}", AUDIO_S16SYS);

        // Check first few samples for sanity
        const int16_t* samples = reinterpret_cast<const int16_t*>(audioBuffer);
        debug("First 3 samples from file (each sample = {} channels):", audioSpec.channels);
        for (int i = 0; i < 3; i++) {
            debug("  Sample {} - Raw: 0x{:04X}, Value: {}", i,
                  static_cast<uint16_t>(samples[i]), samples[i]);
        }
        debug("========================");

        // Verify we got what we expected for RTP streaming
        if (audioSpec.channels != RTP_STREAMING_CHANNELS) {
            std::string errorMessage = fmt::format("Channel count mismatch: expected {} for RTP streaming, got {} in file {}",
                                                  RTP_STREAMING_CHANNELS, audioSpec.channels, filePath);
            error(errorMessage);
            span->setError(errorMessage);
            SDL_FreeWAV(audioBuffer);
            return false;
        }

        if (audioSpec.format != AUDIO_S16) {
            std::string errorMessage = fmt::format("Format mismatch: expected AUDIO_S16 for RTP streaming, got {} in file {}",
                                                  audioSpec.format, filePath);
            error(errorMessage);
            span->setError(errorMessage);
            SDL_FreeWAV(audioBuffer);
            return false;
        }

        // Store format info
        sampleRate = audioSpec.freq;
        channels = audioSpec.channels;

        // Convert to samples count (audioLength is in bytes, divide by bytes per sample)
        size_t sampleCount = audioLength / (sizeof(int16_t) * channels);

        span->setAttribute("sample_rate", sampleRate);
        span->setAttribute("channels", channels);
        span->setAttribute("total_samples", static_cast<int64_t>(sampleCount));
        span->setAttribute("audio_length_bytes", static_cast<int64_t>(audioLength));
        span->setAttribute("audio_format_detected", audioSpec.format);

        debug("Loaded 16-bit audio for RTP streaming: {} samples, {} channels, {} Hz", sampleCount, channels, sampleRate);

        // Create chunks from the loaded 16-bit data (no conversion needed!)
        createChunks(reinterpret_cast<const int16_t *>(audioBuffer), sampleCount, span);

        // Clean up SDL buffer
        SDL_FreeWAV(audioBuffer);

        span->setAttribute("chunk_count", static_cast<int64_t>(chunks.size()));
        span->setAttribute("chunk_size_ms", RTP_FRAME_MS);
        span->setSuccess();

        info("Successfully loaded 16-bit audio file for RTP streaming with {} chunks of {}ms each", chunks.size(), RTP_FRAME_MS);
        return true;
    }

    void AudioStreamBuffer::createChunks(const int16_t *audioData, size_t totalSamples, std::shared_ptr<OperationSpan> parentSpan) {

        if(!parentSpan) {
            warn("No parent span provided for AudioStreamBuffer.createChunks, creating a root span");
            parentSpan = observability->createOperationSpan("audio_stream_buffer.create_chunks");
        }

        auto span = observability->createChildOperationSpan("audio_stream_buffer.create_chunks", parentSpan);
        span->setAttribute("total_samples", static_cast<int64_t>(totalSamples));
        span->setAttribute("chunk_size_ms", RTP_FRAME_MS);

        // Calculate samples per chunk based on RTP configuration
        const uint32_t chunkSizeMs = RTP_FRAME_MS;  // 5ms chunks for RTP
        const uint32_t samplesPerChunk = RTP_SAMPLES;  // Pre-calculated: 48000 * 5 / 1000 = 240

        span->setAttribute("samples_per_chunk", samplesPerChunk);
        span->setAttribute("sample_rate", sampleRate);

        debug("Creating RTP chunks with {} samples per chunk ({}ms at {}Hz)",
              samplesPerChunk, chunkSizeMs, sampleRate);

        chunks.clear();

        // Process the audio data in 5ms chunks for RTP streaming
        size_t currentSample = 0;
        while (currentSample < totalSamples) {
            auto chunk = std::make_unique<AudioChunk>();
            chunk->channels = channels;
            chunk->sampleRate = sampleRate;

            // Determine how many samples to include in this chunk
            size_t remainingSamples = totalSamples - currentSample;
            chunk->sampleCount = std::min(static_cast<size_t>(samplesPerChunk), remainingSamples);

            // Copy interleaved 16-bit data directly (no conversion!)
            size_t int16sInChunk = chunk->sampleCount * channels;
            chunk->data.resize(int16sInChunk);

            std::memcpy(chunk->data.data(),
                        &audioData[currentSample * channels],
                        int16sInChunk * sizeof(int16_t));

            // Store the sample count BEFORE moving the chunk
            uint32_t currentChunkSampleCount = chunk->sampleCount;

            chunks.push_back(std::move(chunk));

            // Use the stored value instead of accessing the moved chunk
            currentSample += currentChunkSampleCount;
        }

        span->setAttribute("chunks_created", static_cast<int64_t>(chunks.size()));
        span->setAttribute("total_audio_bytes", static_cast<int64_t>(chunks.size() * samplesPerChunk * channels * sizeof(int16_t)));
        span->setSuccess();

        debug("Created {} RTP audio chunks of {}ms each", chunks.size(), chunkSizeMs);
    }

    const AudioChunk *AudioStreamBuffer::getChunk(size_t index) const {
        if (index >= chunks.size()) {
            return nullptr;
        }
        return chunks[index].get();
    }

    std::vector<uint8_t> AudioStreamBuffer::createMultiChannelPayload(const AudioChunk* chunk, framenum_t frameNumber, std::shared_ptr<OperationSpan> parentSpan) {

        if(!parentSpan) {
            warn("No parent span provided for AudioStreamBuffer.createMultiChannelPayload, creating a root span");
            parentSpan = observability->createOperationSpan("audio_stream_buffer.create_multi_channel_payload");
        }
        auto span = observability->createChildOperationSpan("audio_stream_buffer.create_multi_channel_payload", parentSpan);

        // Validate chunk integrity first
        size_t expectedSamples = RTP_SAMPLES;
        if (chunk->sampleCount != expectedSamples) {
            warn("Chunk sample count mismatch: got {}, expected {}", chunk->sampleCount, expectedSamples);
        }

        if (chunk->channels != RTP_STREAMING_CHANNELS) {
            warn("Chunk channel count mismatch: got {}, expected {}", chunk->channels, RTP_STREAMING_CHANNELS);
        }

        // Calculate expected size for validation
        size_t expectedSize = RTP_SAMPLES * RTP_STREAMING_CHANNELS * sizeof(int16_t);
        if (chunk->getSizeInBytes() != expectedSize) {
            warn("Audio chunk size mismatch: got {} bytes, expected {} bytes", chunk->getSizeInBytes(), expectedSize);
            warn("  Expected: {} samples × {} channels × 2 bytes = {} bytes",
                 RTP_SAMPLES, RTP_STREAMING_CHANNELS, expectedSize);
        }

        // Create payload with explicit byte order handling for L16 (network byte order)
        std::vector<uint8_t> payload(chunk->getSizeInBytes());

        const int16_t* sourceData = chunk->getRawData();
        int16_t* destData = reinterpret_cast<int16_t*>(payload.data());

        // Convert to network byte order (L16 format requirement)
        size_t totalSamples = chunk->sampleCount * chunk->channels;
        for (size_t i = 0; i < totalSamples; i++) {
            destData[i] = hostToNetworkInt16(sourceData[i]);
        }

        // Debug logging every 5 seconds (1000 frames at 5ms = 5 seconds)
        if (frameNumber % 1000 == 0) {
            debug("=== PAYLOAD DEBUG (Frame {}) ===", frameNumber);
            debug("Chunk: {} samples, {} channels", chunk->sampleCount, chunk->channels);
            debug("Byte order conversion sample:");
            debug("  Original sample 0: 0x{:04X} ({}) -> Network: 0x{:04X} ({})",
                  static_cast<uint16_t>(sourceData[0]), sourceData[0],
                  static_cast<uint16_t>(destData[0]), destData[0]);
            debug("  Original sample 1: 0x{:04X} ({}) -> Network: 0x{:04X} ({})",
                  static_cast<uint16_t>(sourceData[1]), sourceData[1],
                  static_cast<uint16_t>(destData[1]), destData[1]);
            debug("Sample integrity check - first 2 samples of chunk:");
            for (int i = 0; i < 2 && i < chunk->sampleCount; i++) {
                debug("  Sample {}: ch0={} ch1={} ch2={}", i,
                      destData[i*17], destData[i*17+1], destData[i*17+2]);
            }
            debug("================================");
        }

        span->setAttribute("payload_size", static_cast<int64_t>(payload.size()));
        span->setAttribute("audio_format", "16-bit_pcm_interleaved_network_order");
        span->setAttribute("frame_number", static_cast<int64_t>(frameNumber));
        span->setAttribute("sample_count", chunk->sampleCount);
        span->setAttribute("channels", chunk->channels);
        span->setAttribute("byte_order_conversion", "host_to_network");

        trace("Created RTP payload with network byte order: {}KB of 16-bit interleaved PCM audio ({} samples × {} channels)",
              payload.size() / 1024, chunk->sampleCount, chunk->channels);

        span->setSuccess();
        return payload;
    }

    uint32_t AudioStreamBuffer::getDurationMs() const {
        if (chunks.empty() || sampleRate == 0) {
            return 0;
        }

        // Calculate total samples across all chunks
        size_t totalSamples = 0;
        for (const auto &chunk: chunks) {
            totalSamples += chunk->sampleCount;
        }

        // Convert to milliseconds
        return (totalSamples * 1000) / sampleRate;
    }

    // Updated AudioChunk methods for 16-bit data
    std::vector<int16_t> AudioChunk::getChannelData(uint8_t channel) const {
        if (channel >= channels) {
            return {};
        }

        std::vector<int16_t> channelData;
        channelData.reserve(sampleCount);

        // Extract non-interleaved data for the specified channel
        for (uint32_t sample = 0; sample < sampleCount; ++sample) {
            size_t index = sample * channels + channel;
            channelData.push_back(data[index]);
        }

        return channelData;
    }

} // namespace creatures :: rtp