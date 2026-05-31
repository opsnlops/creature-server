#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/config/Configuration.h"
#include "server/storage/Storage.h"

namespace creatures {

// The Configuration global is declared in TestGlobals.cpp; we point its
// SoundFileLocation at a per-test temp dir so Persistence::Permanent has a
// real root to write into.
extern std::shared_ptr<Configuration> config;

} // namespace creatures

namespace creatures::storage {

namespace {

// Tiny subclass that exposes the protected SoundFileLocation setter so the
// test can point Permanent at a per-test temp dir. Configuration's only friend
// is CommandLine in production; subclassing here avoids polluting the header
// with a testing friend.
class TestConfiguration : public Configuration {
  public:
    using Configuration::setSoundFileLocation;
};

class StorageTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Per-test permanent root under the system temp dir. Reused across
        // tests in the same fixture instance — gtest creates a fresh instance
        // per test so we still get isolation.
        permanentRoot_ = std::filesystem::temp_directory_path() /
                         ("storage-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(permanentRoot_);

        savedConfig_ = creatures::config;
        auto testConfig = std::make_shared<TestConfiguration>();
        testConfig->setSoundFileLocation(permanentRoot_.string());
        creatures::config = testConfig;
    }

    void TearDown() override {
        creatures::config = savedConfig_;
        std::error_code ec;
        std::filesystem::remove_all(permanentRoot_, ec);
    }

    std::filesystem::path permanentRoot_;
    std::shared_ptr<Configuration> savedConfig_;
};

} // namespace

TEST_F(StorageTest, RootForEachPersistenceExists) {
    for (auto p : {Persistence::Permanent, Persistence::AdHoc, Persistence::JobScratch, Persistence::GenerationCache}) {
        auto r = root(p);
        ASSERT_TRUE(r.isSuccess()) << static_cast<int>(p) << ": " << r.getError()->getMessage();
        EXPECT_TRUE(std::filesystem::exists(r.getValue().value()));
        EXPECT_TRUE(std::filesystem::is_directory(r.getValue().value()));
    }
}

TEST_F(StorageTest, RootForPermanentMatchesConfig) {
    auto r = root(Persistence::Permanent);
    ASSERT_TRUE(r.isSuccess());
    EXPECT_EQ(r.getValue().value(), permanentRoot_);
}

TEST_F(StorageTest, AllocateSoundPathFailsOnEmptyFilename) {
    auto r = allocateSoundPath(Persistence::Permanent, "");
    ASSERT_FALSE(r.isSuccess());
}

TEST_F(StorageTest, AllocateSoundPathSubdirCreatesNestedDir) {
    auto r = allocateSoundPath(Persistence::Permanent, "scene.wav", std::string{"dialog"});
    ASSERT_TRUE(r.isSuccess()) << r.getError()->getMessage();
    EXPECT_EQ(r.getValue().value().absolute, permanentRoot_ / "dialog" / "scene.wav");
    EXPECT_TRUE(std::filesystem::exists(permanentRoot_ / "dialog"));
}

TEST_F(StorageTest, ForMetadataIsRelativeForPermanent) {
    auto r = allocateSoundPath(Persistence::Permanent, "scene.wav", std::string{"dialog"});
    ASSERT_TRUE(r.isSuccess());
    // The contract: Permanent's forMetadata is the path relative to the
    // permanent root, so the deployment can move the root without rewriting
    // the DB. This is what gets stamped on Animation.metadata.sound_file.
    EXPECT_EQ(r.getValue().value().forMetadata, std::string("dialog/scene.wav"));
}

TEST_F(StorageTest, ForMetadataIsAbsoluteForNonPermanent) {
    for (auto p : {Persistence::AdHoc, Persistence::JobScratch, Persistence::GenerationCache}) {
        auto r = allocateSoundPath(p, "scene.wav");
        ASSERT_TRUE(r.isSuccess()) << static_cast<int>(p) << ": " << r.getError()->getMessage();
        // Non-Permanent stores absolute because there's no canonical root for
        // a reader to join against later.
        EXPECT_EQ(r.getValue().value().forMetadata, r.getValue().value().absolute.string())
            << "persistence=" << static_cast<int>(p);
        EXPECT_TRUE(std::filesystem::path(r.getValue().value().forMetadata).is_absolute());
    }
}

TEST_F(StorageTest, WriteSoundFilePersistsBytes) {
    const std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03, 0xfe, 0xff};
    auto r = writeSoundFile(Persistence::AdHoc, "test.wav", bytes);
    ASSERT_TRUE(r.isSuccess()) << r.getError()->getMessage();

    std::ifstream in(r.getValue().value().absolute, std::ios::binary);
    ASSERT_TRUE(in);
    std::vector<char> read((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_EQ(read.size(), bytes.size());
    EXPECT_EQ(std::memcmp(read.data(), bytes.data(), bytes.size()), 0);
}

TEST_F(StorageTest, WriteSoundFileAtomicLeavesNoTmpOnSuccess) {
    const std::vector<std::uint8_t> bytes{0xaa, 0xbb};
    auto r = writeSoundFile(Persistence::AdHoc, "atomic.wav", bytes);
    ASSERT_TRUE(r.isSuccess());
    // No orphan .tmp sibling — the rename succeeded and removed the temp.
    EXPECT_FALSE(std::filesystem::exists(r.getValue().value().absolute.string() + ".tmp"));
}

TEST_F(StorageTest, WriteSoundFileEmptyBytesWritesEmptyFile) {
    // Edge case: an empty write is still a file on disk. Real callers won't
    // do this but the contract should be predictable.
    auto r = writeSoundFile(Persistence::JobScratch, "empty.wav", std::span<const std::uint8_t>{});
    ASSERT_TRUE(r.isSuccess());
    EXPECT_TRUE(std::filesystem::exists(r.getValue().value().absolute));
    EXPECT_EQ(std::filesystem::file_size(r.getValue().value().absolute), 0u);
}

TEST_F(StorageTest, ResolveSoundPathAbsolutePassesThrough) {
    const std::filesystem::path abs = std::filesystem::temp_directory_path() / "foo" / "bar.wav";
    EXPECT_EQ(resolveSoundPath(abs.string()), abs);
}

TEST_F(StorageTest, ResolveSoundPathRelativeJoinsUnderPermanent) {
    EXPECT_EQ(resolveSoundPath("dialog/scene.wav"), permanentRoot_ / "dialog" / "scene.wav");
}

TEST_F(StorageTest, ResolveSoundPathEmptyReturnsEmpty) { EXPECT_TRUE(resolveSoundPath("").empty()); }

TEST_F(StorageTest, AllocateAndResolveRoundTripPermanent) {
    // The contract that makes Permanent work: allocateSoundPath → write to
    // .absolute → stamp .forMetadata onto the model → resolveSoundPath returns
    // the same .absolute. If this breaks, sound playback breaks.
    auto r = allocateSoundPath(Persistence::Permanent, "scene.wav", std::string{"dialog"});
    ASSERT_TRUE(r.isSuccess());
    EXPECT_EQ(resolveSoundPath(r.getValue().value().forMetadata), r.getValue().value().absolute);
}

TEST_F(StorageTest, AllocateAndResolveRoundTripAdHoc) {
    auto r = allocateSoundPath(Persistence::AdHoc, "scene.wav");
    ASSERT_TRUE(r.isSuccess());
    EXPECT_EQ(resolveSoundPath(r.getValue().value().forMetadata), r.getValue().value().absolute);
}

} // namespace creatures::storage
