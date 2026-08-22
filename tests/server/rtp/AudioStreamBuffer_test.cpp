// In-memory sharing and bounds for AudioStreamBuffer (issue #93).
//
// Real Opus encoding runs here (fast at these sizes: ~500 ms of 17-channel
// audio). The disk cache is deliberately NOT enabled — AudioCache has its own
// suite — so these tests exercise the encode path plus the memo.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "server/config.h"
#include "server/rtp/AudioStreamBuffer.h"

namespace creatures::rtp {
namespace {

namespace fs = std::filesystem;

void writeLe(std::ofstream &out, uint32_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

// Minimal canonical 17-channel / 48 kHz / s16 PCM WAV.
void write17ChannelWav(const fs::path &path, std::size_t sampleFrames, int16_t sampleValue = 1000) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const uint32_t channels = RTP_STREAMING_CHANNELS;
    const uint32_t sampleRate = RTP_SRATE;
    const uint32_t bytesPerFrame = channels * 2;
    const uint32_t dataBytes = static_cast<uint32_t>(sampleFrames) * bytesPerFrame;

    out.write("RIFF", 4);
    writeLe(out, 36 + dataBytes, 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe(out, 16, 4);
    writeLe(out, 1, 2); // PCM
    writeLe(out, channels, 2);
    writeLe(out, sampleRate, 4);
    writeLe(out, sampleRate * bytesPerFrame, 4);
    writeLe(out, bytesPerFrame, 2);
    writeLe(out, 16, 2); // bits per sample
    out.write("data", 4);
    writeLe(out, dataBytes, 4);
    for (std::size_t frame = 0; frame < sampleFrames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            writeLe(out, static_cast<uint16_t>(sampleValue), 2);
        }
    }
}

class AudioStreamBufferTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("audio-buffer-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::create_directories(root_);
        // Fresh memo per test, generous budget.
        AudioStreamBuffer::clearMemo();
        AudioStreamBuffer::setMemoRetainBytes(64ULL * 1024ULL * 1024ULL);
    }

    void TearDown() override {
        AudioStreamBuffer::clearMemo();
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path wavPath(const std::string &name) { return root_ / name; }

    fs::path root_;
};

// 500 ms = 24,000 sample frames = 50 ten-ms Opus frames per channel.
constexpr std::size_t HALF_SECOND_FRAMES = 24000;

TEST_F(AudioStreamBufferTest, LoadsAndEncodesA17ChannelWav) {
    const auto path = wavPath("clip.wav");
    write17ChannelWav(path, HALF_SECOND_FRAMES);

    auto buffer = AudioStreamBuffer::loadFromWavFile(path.string());
    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(buffer->getFrameCount(), 50U);
    EXPECT_GT(buffer->approximateBytes(), 0U);
}

TEST_F(AudioStreamBufferTest, ConcurrentLoadsShareOneImmutableBuffer) {
    const auto path = wavPath("shared.wav");
    write17ChannelWav(path, HALF_SECOND_FRAMES);

    constexpr std::size_t loaderCount = 8;
    std::vector<std::shared_ptr<AudioStreamBuffer>> buffers(loaderCount);
    std::vector<std::thread> loaders;
    loaders.reserve(loaderCount);
    for (std::size_t i = 0; i < loaderCount; ++i) {
        loaders.emplace_back([&, i] { buffers[i] = AudioStreamBuffer::loadFromWavFile(path.string()); });
    }
    for (auto &loader : loaders) {
        loader.join();
    }

    ASSERT_NE(buffers[0], nullptr);
    for (std::size_t i = 1; i < loaderCount; ++i) {
        EXPECT_EQ(buffers[i].get(), buffers[0].get()) << "every concurrent playback must share ONE buffer";
    }
}

TEST_F(AudioStreamBufferTest, ChangedFileIsReloadedNotServedFromTheMemo) {
    const auto path = wavPath("mutating.wav");
    write17ChannelWav(path, HALF_SECOND_FRAMES);
    auto first = AudioStreamBuffer::loadFromWavFile(path.string());
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->getFrameCount(), 50U);

    // Replace with a longer file (different size ⇒ memo fingerprint miss).
    write17ChannelWav(path, HALF_SECOND_FRAMES * 2);
    auto second = AudioStreamBuffer::loadFromWavFile(path.string());
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second.get(), first.get());
    EXPECT_EQ(second->getFrameCount(), 100U);
    // The first buffer is immutable and still valid for its holders.
    EXPECT_EQ(first->getFrameCount(), 50U);
}

TEST_F(AudioStreamBufferTest, MemoRetentionIsBoundedByTheByteBudget) {
    // A budget of one byte forces the LRU to hold only the newest buffer:
    // reloading an older file must produce a NEW buffer (its strong ref was
    // evicted and no playback held it).
    AudioStreamBuffer::setMemoRetainBytes(1);

    const auto pathA = wavPath("a.wav");
    const auto pathB = wavPath("b.wav");
    write17ChannelWav(pathA, HALF_SECOND_FRAMES);
    write17ChannelWav(pathB, HALF_SECOND_FRAMES);

    auto firstA = AudioStreamBuffer::loadFromWavFile(pathA.string());
    ASSERT_NE(firstA, nullptr);
    std::weak_ptr<AudioStreamBuffer> weakA = firstA;
    firstA.reset(); // drop the only strong ref outside the LRU

    // Loading B evicts A from the tiny LRU, releasing its last strong ref.
    auto loadedB = AudioStreamBuffer::loadFromWavFile(pathB.string());
    ASSERT_NE(loadedB, nullptr);
    EXPECT_TRUE(weakA.expired()) << "the evicted buffer must have been released, not retained";
}

TEST_F(AudioStreamBufferTest, HeldBuffersSurviveEvictionWhileInUse) {
    AudioStreamBuffer::setMemoRetainBytes(1);

    const auto pathA = wavPath("held.wav");
    const auto pathB = wavPath("other.wav");
    write17ChannelWav(pathA, HALF_SECOND_FRAMES);
    write17ChannelWav(pathB, HALF_SECOND_FRAMES);

    auto held = AudioStreamBuffer::loadFromWavFile(pathA.string()); // playback holds it
    ASSERT_NE(held, nullptr);
    auto other = AudioStreamBuffer::loadFromWavFile(pathB.string()); // evicts A's LRU slot

    // The weak memo still knows the live buffer: a concurrent playback of the
    // same unchanged file shares it even though the LRU dropped its strong ref.
    auto again = AudioStreamBuffer::loadFromWavFile(pathA.string());
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again.get(), held.get());
}

TEST_F(AudioStreamBufferTest, OneShotLoadsAreSharedButNotRetained) {
    // Prewarms and per-sentence streaming speech must not spend the retention
    // budget that keeps show audio warm (issue #93 review).
    const auto path = wavPath("oneshot.wav");
    write17ChannelWav(path, HALF_SECOND_FRAMES);

    const auto before = AudioStreamBuffer::memoRetainedBytes();
    auto oneShot =
        AudioStreamBuffer::loadFromWavFile(path.string(), nullptr, AudioStreamBuffer::RetentionIntent::OneShot);
    ASSERT_NE(oneShot, nullptr);
    EXPECT_EQ(AudioStreamBuffer::memoRetainedBytes(), before) << "a one-shot load must not be charged to the budget";

    // Still weakly memoized: a concurrent load shares the same buffer.
    auto shared =
        AudioStreamBuffer::loadFromWavFile(path.string(), nullptr, AudioStreamBuffer::RetentionIntent::OneShot);
    EXPECT_EQ(shared.get(), oneShot.get());

    // A real playback load of the same file DOES retain it.
    auto retained = AudioStreamBuffer::loadFromWavFile(path.string());
    ASSERT_NE(retained, nullptr);
    EXPECT_GT(AudioStreamBuffer::memoRetainedBytes(), before);
}

TEST_F(AudioStreamBufferTest, InvalidateMemoForcesAReload) {
    const auto path = wavPath("invalidated.wav");
    write17ChannelWav(path, HALF_SECOND_FRAMES);

    auto first = AudioStreamBuffer::loadFromWavFile(path.string());
    ASSERT_NE(first, nullptr);
    std::weak_ptr<AudioStreamBuffer> weakFirst = first;
    first.reset();

    // The operator/storage lever for content that changed without the
    // fingerprint moving (issue #93).
    AudioStreamBuffer::invalidateMemo(path.string());
    EXPECT_TRUE(weakFirst.expired()) << "invalidate must release the retained buffer too";

    auto second = AudioStreamBuffer::loadFromWavFile(path.string());
    ASSERT_NE(second, nullptr);
}

TEST_F(AudioStreamBufferTest, RejectsWrongChannelCount) {
    // A 2-channel file must be rejected by the 17-channel contract.
    const auto path = wavPath("stereo.wav");
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    writeLe(out, 36 + 400, 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe(out, 16, 4);
    writeLe(out, 1, 2);
    writeLe(out, 2, 2); // stereo
    writeLe(out, RTP_SRATE, 4);
    writeLe(out, RTP_SRATE * 4, 4);
    writeLe(out, 4, 2);
    writeLe(out, 16, 2);
    out.write("data", 4);
    writeLe(out, 400, 4);
    for (int i = 0; i < 100; ++i) {
        writeLe(out, 0, 4);
    }
    out.close();

    EXPECT_EQ(AudioStreamBuffer::loadFromWavFile(path.string()), nullptr);
}

} // namespace
} // namespace creatures::rtp
