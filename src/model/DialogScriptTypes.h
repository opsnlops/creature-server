#pragma once

#include <cstddef>
#include <string>

namespace creatures {

inline constexpr std::size_t MAX_DIALOG_SCRIPT_TURNS = 200;
inline constexpr std::size_t MAX_DIALOG_SCRIPT_TURN_TEXT = 4096;
inline constexpr std::size_t MAX_DIALOG_SCRIPT_TITLE = 256;
inline constexpr std::size_t MAX_DIALOG_SCRIPT_NOTES = 16384;
inline constexpr std::size_t MAX_DIALOG_MUSIC_PROMPT = 4100;
inline constexpr std::size_t MAX_DIALOG_MUSIC_SOUND_FILE = 1024;

struct DialogScriptTurn {
    std::string creature_id;
    std::string text;
};

} // namespace creatures
