#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "RhubarbData.h"

namespace creatures::voice {

/**
 * TextToViseme
 *
 * Converts word-level timing data into mouth shape (viseme) cues using
 * the CMU Pronouncing Dictionary for accurate phoneme lookup.
 *
 * ARPAbet phonemes are mapped to Rhubarb-compatible mouth shapes (A-F, X):
 *   A (5)   = Nearly closed (M, B, P)
 *   B (180) = Wide open (AA, AE, AH, AO, AW, AY, IH, IY, OW, OY, UH, UW)
 *   C (240) = Very wide (CH, JH, SH, ZH)
 *   D (255) = Maximum open (K, G, NG, HH, W, Y)
 *   E (50)  = Slightly open (EH, ER, EY, S, Z, T, D, N, L, R, TH, DH)
 *   F (20)  = Mostly closed (F, V)
 *   X (0)   = Rest/silence
 *
 * Shared between the whisper.cpp path (Phase 1) and ElevenLabs alignment
 * path (Phase 2).
 */
class TextToViseme {
  public:
    TextToViseme() = default;

    /**
     * Load the CMU Pronouncing Dictionary from a file.
     *
     * @param dictPath Path to the cmudict file (e.g., cmudict-0.7b or cmudict.dict)
     * @return true if loaded successfully
     */
    bool loadCmuDict(const std::filesystem::path &dictPath);

    /**
     * Check if the dictionary has been loaded.
     */
    [[nodiscard]] bool isLoaded() const;

    /**
     * Get the number of words in the dictionary.
     */
    [[nodiscard]] size_t wordCount() const;

    /**
     * Word-level timing information (from whisper.cpp or ElevenLabs alignment).
     */
    struct WordTiming {
        std::string word;
        double startTime; // seconds
        double endTime;   // seconds
    };

    /**
     * Character-level timing information (from ElevenLabs alignment data).
     */
    struct CharTiming {
        char character;
        double startTimeMs; // milliseconds
        double durationMs;  // milliseconds
    };

    /**
     * Convert word-level timing data into mouth cues.
     *
     * @param words Vector of words with start/end times
     * @return Vector of RhubarbMouthCue entries ready for SoundDataProcessor
     */
    std::vector<RhubarbMouthCue> wordsToMouthCues(const std::vector<WordTiming> &words) const;

    /**
     * Convert ElevenLabs character-level alignment data into mouth cues.
     *
     * Reconstructs words from character sequences, looks up phonemes,
     * and distributes them across the character timing.
     *
     * @param chars Vector of character timing data
     * @return Vector of RhubarbMouthCue entries
     */
    std::vector<RhubarbMouthCue> charTimingsToMouthCues(const std::vector<CharTiming> &chars) const;

    /**
     * Map a single ARPAbet phoneme to a Rhubarb viseme letter.
     *
     * The phoneme string should be the base phoneme without stress markers
     * (e.g., "AA" not "AA1").
     *
     * @param phoneme ARPAbet phoneme (e.g., "AA", "M", "SH")
     * @return Viseme letter (A-F, X)
     */
    static std::string arpabetToViseme(const std::string &phoneme);

  private:
    // word (uppercase) -> list of ARPAbet phonemes (without stress markers)
    std::unordered_map<std::string, std::vector<std::string>> cmuDict_;

    /**
     * Look up phonemes for a word in the CMU dictionary.
     * Falls back to character-based heuristics for unknown words.
     *
     * @param word The word to look up (case-insensitive)
     * @return Vector of ARPAbet phoneme strings
     */
    std::vector<std::string> lookupPhonemes(const std::string &word) const;

    /**
     * Generate approximate phonemes from character heuristics
     * for words not found in the CMU dictionary.
     *
     * @param word The word to approximate
     * @return Vector of approximate ARPAbet phoneme strings
     */
    static std::vector<std::string> approximatePhonemes(const std::string &word);

    /**
     * Strip punctuation and normalize a word for dictionary lookup.
     *
     * @param word Raw word text
     * @return Normalized uppercase word
     */
    static std::string normalizeWord(const std::string &word);
};

} // namespace creatures::voice
