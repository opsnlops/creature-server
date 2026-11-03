#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "server/config/ConfigFile.h"

namespace creatures {
namespace {

namespace fs = std::filesystem;

class ConfigFileTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("config-file-test-" + std::to_string(::getpid()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::string write(const std::string &contents) {
        const auto path = dir_ / "server.json";
        std::ofstream out(path);
        out << contents;
        out.close();
        return path.string();
    }

    fs::path dir_;
};

TEST_F(ConfigFileTest, ReadsTheProductionSettings) {
    const auto path = write(R"({
        "mongodb_uri": "mongodb://127.0.0.1/?serverSelectionTimeoutMS=2000",
        "sounds_location": "/mnt/creatures/sounds",
        "audio_cache_dir": "/var/cache/creature-server",
        "network_device": "enp5s0f0np0",
        "audio_mode": "rtp",
        "sound_channels": 2,
        "expressive_voice_model": "eleven_v4",
        "adhoc_voice_model": "eleven_v4_turbo"
    })");

    const auto loaded = ConfigFile::load(path, /*required=*/true);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.getError()->getMessage();
    const auto config = loaded.getValue().value();
    EXPECT_EQ(config.stringOr("sounds_location", "wrong"), "/mnt/creatures/sounds");
    EXPECT_EQ(config.stringOr("audio_cache_dir", "wrong"), "/var/cache/creature-server");
    EXPECT_EQ(config.stringOr("network_device", "wrong"), "enp5s0f0np0");
    EXPECT_EQ(config.stringOr("audio_mode", "wrong"), "rtp");
    EXPECT_EQ(config.intOr("sound_channels", 99), 2);
    EXPECT_EQ(config.stringOr("expressive_voice_model", "wrong"), "eleven_v4");
    EXPECT_EQ(config.stringOr("adhoc_voice_model", "wrong"), "eleven_v4_turbo");
    EXPECT_EQ(config.sourcePath(), path);
}

TEST_F(ConfigFileTest, AbsentKeysFallThroughToTheirDefaults) {
    const auto path = write(R"({"sounds_location": "/only/this/one"})");

    const auto config = ConfigFile::load(path, true).getValue().value();

    EXPECT_EQ(config.stringOr("sounds_location", "fallback"), "/only/this/one");
    EXPECT_EQ(config.stringOr("network_device", "fallback"), "fallback");
    EXPECT_EQ(config.intOr("sound_channels", 17), 17);
    EXPECT_FALSE(config.has("network_device"));
}

TEST_F(ConfigFileTest, UnderscoreKeysAreCommentsAndAreIgnored) {
    // JSON has no comments, and this is a file people edit by hand, so the
    // packaged one explains itself in a "_comment" key.
    const auto path = write(R"({
        "_comment": ["anything at all", 42, {"nested": true}],
        "_": "even this",
        "sounds_location": "/real/setting"
    })");

    const auto loaded = ConfigFile::load(path, true);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.getError()->getMessage();
    EXPECT_EQ(loaded.getValue()->stringOr("sounds_location", "wrong"), "/real/setting");
}

TEST_F(ConfigFileTest, RejectsAnUnknownSettingAndSaysWhatIsValid) {
    // A typo that silently did nothing would be worse than refusing to start:
    // the operator would believe a setting had taken effect.
    const auto path = write(R"({"sonud_channels": 2})");

    const auto loaded = ConfigFile::load(path, true);

    ASSERT_FALSE(loaded.isSuccess());
    EXPECT_EQ(loaded.getError()->getCode(), ServerError::InvalidData);
    const auto message = loaded.getError()->getMessage();
    EXPECT_NE(message.find("sonud_channels"), std::string::npos) << "must name the offending key";
    EXPECT_NE(message.find("sound_channels"), std::string::npos) << "and list what is actually accepted";
}

TEST_F(ConfigFileTest, RejectsAWronglyTypedSettingAtLoadTime) {
    // Checked on load rather than on first read, so a bad value is reported at
    // startup instead of whenever that setting happens to be used.
    ASSERT_FALSE(ConfigFile::load(write(R"({"sound_channels": "two"})"), true).isSuccess());
    ASSERT_FALSE(ConfigFile::load(write(R"({"sounds_location": 17})"), true).isSuccess());
    ASSERT_FALSE(ConfigFile::load(write(R"({"sound_channels": 2.5})"), true).isSuccess());
}

TEST_F(ConfigFileTest, RejectsMalformedJsonAndNonObjects) {
    EXPECT_FALSE(ConfigFile::load(write("{not json"), true).isSuccess());
    EXPECT_FALSE(ConfigFile::load(write("[1,2,3]"), true).isSuccess());
    EXPECT_FALSE(ConfigFile::load(write("\"a string\""), true).isSuccess());
}

TEST_F(ConfigFileTest, AMissingFileIsFineUnlessItWasAskedForByName) {
    const auto missing = (dir_ / "not-here.json").string();

    // The default path is read opportunistically: a dev build has no
    // /etc/creature and must not need one.
    const auto optional = ConfigFile::load(missing, /*required=*/false);
    ASSERT_TRUE(optional.isSuccess());
    EXPECT_TRUE(optional.getValue()->sourcePath().empty()) << "nothing was read, so there is no source";
    EXPECT_EQ(optional.getValue()->stringOr("sounds_location", "fallback"), "fallback");

    // But --config pointing at a file that isn't there is a mistake.
    const auto required = ConfigFile::load(missing, /*required=*/true);
    ASSERT_FALSE(required.isSuccess());
    EXPECT_EQ(required.getError()->getCode(), ServerError::NotFound);
}

TEST_F(ConfigFileTest, AnEmptyConfigReturnsEveryFallback) {
    const auto config = ConfigFile::empty();
    EXPECT_TRUE(config.sourcePath().empty());
    EXPECT_EQ(config.stringOr("mongodb_uri", "fallback"), "fallback");
    EXPECT_EQ(config.intOr("sound_channels", 17), 17);
    EXPECT_TRUE(config.boolOr("anything", true));
}

TEST_F(ConfigFileTest, EveryKeyInThePackagedConfigIsAKnownKey) {
    // Guards against the packaged /etc/creature/server.json drifting away from
    // what the server accepts, which would make the package refuse to start.
    const auto &keys = ConfigFile::knownKeys();
    for (const auto &expected : {"mongodb_uri", "sounds_location", "audio_cache_dir", "network_device", "audio_mode",
                                 "sound_channels", "expressive_voice_model", "adhoc_voice_model"}) {
        EXPECT_NE(std::find(keys.begin(), keys.end(), expected), keys.end()) << expected << " should be accepted";
    }
}

} // namespace
} // namespace creatures
