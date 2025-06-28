//
// AudioStreamBuffer.h
//

#pragma once

#include <vector>

#include <SDL2/SDL.h>

#include "server/rtp/AudioChunk.h"
#include "util/ObservabilityManager.h"

#include "server/namespace-stuffs.h"

namespace creatures :: rtp {

    /**
     * Loads and manages streaming 16-bit PCM audio data from WAV files
     * Optimized for direct RTP transmission without float conversion
     */
    class AudioStreamBuffer {
    public:
        AudioStreamBuffer() = default;
        ~AudioStreamBuffer();

        /**
         * Load a 16-bit PCM WAV file and prepare it for streaming
         * @param filePath Path to the multi-channel WAV file (must be 16-bit PCM)
         * @return true if successful, false on error
         */
        bool loadFile(const std::string& filePath, std::shared_ptr<OperationSpan> parentSpan = nullptr);

        /**
         * Get the total number of chunks in this audio file
         * @return Number of chunks
         */
        [[nodiscard]] size_t getChunkCount() const { return chunks.size(); }

        /**
         * Get a specific chunk
         * @param index Chunk index (0-based)
         * @return Pointer to chunk, or nullptr if index is invalid
         */
        [[nodiscard]] const AudioChunk* getChunk(size_t index) const;

        /**
         * Get the duration of the audio file in milliseconds
         * @return Duration in ms
         */
        [[nodiscard]] uint32_t getDurationMs() const;

        /**
         * Get audio format information
         */
        [[nodiscard]] uint32_t getSampleRate() const { return sampleRate; }
        [[nodiscard]] uint8_t getChannels() const { return channels; }

         /**
         * Create a pure RTP payload with just the raw 16-bit interleaved PCM audio data
         * No custom headers - uvgRTP will add proper RTP headers automatically! 🐰
         *
         * @param chunk Audio chunk to convert to payload
         * @param frameNumber Frame number for observability (not included in payload)
         * @param parentSpan Optional parent span for tracing
         * @return Raw 16-bit interleaved PCM data ready for RTP transmission
         */
        static std::vector<uint8_t> createMultiChannelPayload(const rtp::AudioChunk* chunk,
                                                              framenum_t frameNumber,
                                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

    private:
        std::vector<std::unique_ptr<AudioChunk>> chunks;
        uint32_t sampleRate = 0;
        uint8_t channels = 0;
        SDL_AudioSpec audioSpec{};

        /**
         * Slice the loaded 16-bit audio data into configurable chunks
         * @param audioData Raw 16-bit PCM data (no conversion needed!)
         * @param sampleCount Number of samples (per channel)
         * @param parentSpan Optional parent span for tracing
         */
        void createChunks(const int16_t* audioData, size_t sampleCount, std::shared_ptr<OperationSpan> parentSpan = nullptr);
    };

} // namespace creatures :: rtp