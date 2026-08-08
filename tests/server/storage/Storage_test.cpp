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

// ===========================================================================
// Accepted voice take promote / demote (#131)
//
// The invariant under test: the permanent sounds directory holds at most one
// take per script, because acceptance MOVES the audio rather than copying it.
// ===========================================================================

namespace {

/// Put a stand-in take WAV where a generated take's export would be.
std::filesystem::path writeAdHocTake(const std::string &generationId, const std::string &contents) {
    auto path = voiceTakeAdHocPath(generationId);
    EXPECT_TRUE(path.isSuccess()) << "ad-hoc root should always resolve";
    std::filesystem::create_directories(path.getValue().value().parent_path());
    std::ofstream out(path.getValue().value(), std::ios::binary);
    out << contents;
    return path.getValue().value();
}

} // namespace

TEST_F(StorageTest, VoiceTakeAdHocPathEndsWithTheSharedFilename) {
    // promote, demote and the preview export all have to agree on where a
    // take's WAV lives; they agree by going through this one function.
    auto path = voiceTakeAdHocPath("abc123");
    ASSERT_TRUE(path.isSuccess());
    EXPECT_EQ(path.getValue().value().filename().string(), voiceTakeAdHocFilename("abc123"));
}

TEST_F(StorageTest, PromoteVoiceTakeMovesRatherThanCopies) {
    const std::string generationId = "0f7c2a11-promote";
    const auto source = writeAdHocTake(generationId, "take audio");

    auto promoted = promoteVoiceTake(generationId, "coffee-0f7c2a11.wav");
    ASSERT_TRUE(promoted.isSuccess()) << promoted.getError()->getMessage();
    EXPECT_EQ(promoted.getValue().value().forMetadata, std::string("dialog/voice/coffee-0f7c2a11.wav"));
    EXPECT_TRUE(std::filesystem::exists(promoted.getValue().value().absolute));

    // Moved: nothing is left behind in ad-hoc to be swept or re-promoted.
    EXPECT_FALSE(std::filesystem::exists(source));
}

TEST_F(StorageTest, PromoteVoiceTakeWithoutAdHocAudioSaysWhatIsActuallyWrong) {
    // A take generated seconds ago hits this path too when its 17-channel WAV
    // was never written, so the message must not blame the 24h TTL.
    auto promoted = promoteVoiceTake("2b8e44c9-never-exported", "scene-2b8e44c9.wav");
    ASSERT_FALSE(promoted.isSuccess());
    EXPECT_EQ(promoted.getError()->getCode(), ServerError::NotFound);
    const std::string message = promoted.getError()->getMessage();
    EXPECT_NE(message.find("never exported"), std::string::npos) << message;
}

TEST_F(StorageTest, DemoteReturnsTheTakeToWherePromoteWouldFindItAgain) {
    const std::string generationId = "9d31ff02-roundtrip";
    writeAdHocTake(generationId, "take audio");

    auto promoted = promoteVoiceTake(generationId, "coffee-9d31ff02.wav");
    ASSERT_TRUE(promoted.isSuccess()) << promoted.getError()->getMessage();

    ASSERT_TRUE(demoteVoiceTake(promoted.getValue().value().forMetadata, generationId).isSuccess());
    EXPECT_FALSE(std::filesystem::exists(promoted.getValue().value().absolute));

    // Un-accepting has to be reversible: the same take can be accepted again
    // without regenerating audio.
    auto again = promoteVoiceTake(generationId, "coffee-9d31ff02.wav");
    ASSERT_TRUE(again.isSuccess()) << again.getError()->getMessage();
    EXPECT_TRUE(std::filesystem::exists(again.getValue().value().absolute));
}

TEST_F(StorageTest, ReplacingATakeLeavesExactlyOneInThePermanentTree) {
    writeAdHocTake("take-a", "A");
    writeAdHocTake("take-b", "B");

    auto a = promoteVoiceTake("take-a", "coffee-take-a.wav");
    ASSERT_TRUE(a.isSuccess()) << a.getError()->getMessage();

    // What the accept endpoint does when replacing: demote the outgoing take,
    // then promote the incoming one.
    ASSERT_TRUE(demoteVoiceTake(a.getValue().value().forMetadata, "take-a").isSuccess());
    auto b = promoteVoiceTake("take-b", "coffee-take-b.wav");
    ASSERT_TRUE(b.isSuccess()) << b.getError()->getMessage();

    std::size_t takesOnDisk = 0;
    for (const auto &entry : std::filesystem::directory_iterator(permanentRoot_ / "dialog" / "voice")) {
        takesOnDisk += entry.is_regular_file() ? 1 : 0;
    }
    EXPECT_EQ(takesOnDisk, 1u) << "the sounds directory must hold at most one take per script";
}

} // namespace creatures::storage

// ===========================================================================
// Superseded dialog audio cleanup (issue #128)
//
// Re-rendering a dialog script writes new audio and repoints the animation,
// orphaning the previous file. These are large — a long scene runs to
// hundreds of MB — so the guards on what may be deleted matter more than
// usual. A wrong delete here destroys show audio.
// ===========================================================================

TEST(DeleteSupersededDialogSoundTest, RefusesAnythingOutsideTheDialogSubdirectory) {
    // Hand-uploaded sounds live at the root of the tree. Only this pipeline's
    // own generated audio is ever a candidate.
    EXPECT_TRUE(creatures::storage::deleteSupersededDialogSound("nerdAlert.wav").isSuccess());
    EXPECT_TRUE(creatures::storage::deleteSupersededDialogSound("sounds/thing.wav").isSuccess());
    EXPECT_TRUE(creatures::storage::deleteSupersededDialogSound("").isSuccess());
}

TEST(DeleteSupersededDialogSoundTest, RefusesPathTraversalOutOfTheSoundRoot) {
    // sound_file is client-writable through the animation upsert, so a
    // reference that climbs out of the tree must never be followed.
    for (const auto *evil :
         {"dialog/../../etc/passwd", "dialog/../../../tmp/x.wav", "dialog/subdir/../../../outside.wav"}) {
        EXPECT_TRUE(creatures::storage::deleteSupersededDialogSound(evil).isSuccess())
            << evil << " should decline, not error";
    }
}

TEST(DeleteSupersededDialogSoundTest, MissingFileIsNotAnError) {
    // Cleanup must never fail a render that already succeeded.
    EXPECT_TRUE(creatures::storage::deleteSupersededDialogSound("dialog/definitely-not-here-9f1fd726.wav").isSuccess());
}
