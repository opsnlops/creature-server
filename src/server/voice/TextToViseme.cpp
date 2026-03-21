#include "TextToViseme.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "server/namespace-stuffs.h"

namespace creatures::voice {

bool TextToViseme::loadCmuDict(const std::filesystem::path &dictPath) {
    if (!std::filesystem::exists(dictPath)) {
        error("CMU dictionary file not found: {}", dictPath.string());
        return false;
    }

    std::ifstream file(dictPath);
    if (!file.is_open()) {
        error("Failed to open CMU dictionary: {}", dictPath.string());
        return false;
    }

    cmuDict_.clear();
    std::string line;
    size_t lineCount = 0;

    while (std::getline(file, line)) {
        // Skip comment lines (start with ;;; in cmudict)
        if (line.empty() || line[0] == ';') {
            continue;
        }

        // Format: WORD  PH1 PH2 PH3 ...
        // or:     WORD(2)  PH1 PH2 PH3 ... (alternate pronunciations)
        std::istringstream iss(line);
        std::string word;
        iss >> word;

        if (word.empty()) {
            continue;
        }

        // Strip alternate pronunciation markers like "(2)", "(3)"
        auto parenPos = word.find('(');
        if (parenPos != std::string::npos) {
            word = word.substr(0, parenPos);
        }

        // Convert to uppercase for consistent lookup
        std::transform(word.begin(), word.end(), word.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        // Read phonemes, stripping stress markers (0, 1, 2)
        std::vector<std::string> phonemes;
        std::string phoneme;
        while (iss >> phoneme) {
            // Remove trailing stress digits (e.g., "AA1" -> "AA")
            while (!phoneme.empty() && std::isdigit(static_cast<unsigned char>(phoneme.back()))) {
                phoneme.pop_back();
            }
            if (!phoneme.empty()) {
                phonemes.push_back(phoneme);
            }
        }

        if (!phonemes.empty()) {
            // Only store the first pronunciation (skip alternates)
            if (cmuDict_.find(word) == cmuDict_.end()) {
                cmuDict_[word] = std::move(phonemes);
            }
        }

        lineCount++;
    }

    info("Loaded CMU dictionary: {} entries from {}", cmuDict_.size(), dictPath.filename().string());
    return !cmuDict_.empty();
}

bool TextToViseme::isLoaded() const { return !cmuDict_.empty(); }

size_t TextToViseme::wordCount() const { return cmuDict_.size(); }

std::string TextToViseme::arpabetToViseme(const std::string &phoneme) {
    // Bilabial stops/nasals → A (nearly closed, 5)
    if (phoneme == "M" || phoneme == "B" || phoneme == "P") {
        return "A";
    }

    // Open vowels → B (wide open, 180)
    if (phoneme == "AA" || phoneme == "AE" || phoneme == "AH" || phoneme == "AO" || phoneme == "AW" ||
        phoneme == "AY" || phoneme == "IH" || phoneme == "IY" || phoneme == "OW" || phoneme == "OY" ||
        phoneme == "UH" || phoneme == "UW") {
        return "B";
    }

    // Postalveolar affricates/fricatives → C (very wide, 240)
    if (phoneme == "CH" || phoneme == "JH" || phoneme == "SH" || phoneme == "ZH") {
        return "C";
    }

    // Velar/glottal/semivowels → D (maximum open, 255)
    if (phoneme == "K" || phoneme == "G" || phoneme == "NG" || phoneme == "HH" || phoneme == "W" ||
        phoneme == "Y") {
        return "D";
    }

    // Alveolar consonants + mid vowels → E (slightly open, 50)
    if (phoneme == "EH" || phoneme == "ER" || phoneme == "EY" || phoneme == "S" || phoneme == "Z" ||
        phoneme == "T" || phoneme == "D" || phoneme == "N" || phoneme == "L" || phoneme == "R" ||
        phoneme == "TH" || phoneme == "DH") {
        return "E";
    }

    // Labiodental fricatives → F (mostly closed, 20)
    if (phoneme == "F" || phoneme == "V") {
        return "F";
    }

    // Silence or unknown → X (rest, 0)
    return "X";
}

std::string TextToViseme::normalizeWord(const std::string &word) {
    std::string normalized;
    normalized.reserve(word.size());

    for (char c : word) {
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'') {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }

    return normalized;
}

std::vector<std::string> TextToViseme::lookupPhonemes(const std::string &word) const {
    auto normalized = normalizeWord(word);
    if (normalized.empty()) {
        return {};
    }

    auto it = cmuDict_.find(normalized);
    if (it != cmuDict_.end()) {
        return it->second;
    }

    // Try without apostrophe (e.g., "DON'T" might be stored as "DON'T" or "DONT")
    std::string noApostrophe;
    for (char c : normalized) {
        if (c != '\'') {
            noApostrophe.push_back(c);
        }
    }
    it = cmuDict_.find(noApostrophe);
    if (it != cmuDict_.end()) {
        return it->second;
    }

    // Fall back to character heuristics
    return approximatePhonemes(normalized);
}

std::vector<std::string> TextToViseme::approximatePhonemes(const std::string &word) {
    std::vector<std::string> phonemes;

    for (size_t i = 0; i < word.size(); ++i) {
        char c = std::toupper(static_cast<unsigned char>(word[i]));
        char next = (i + 1 < word.size()) ? std::toupper(static_cast<unsigned char>(word[i + 1])) : '\0';

        // Handle common digraphs
        if (c == 'S' && next == 'H') {
            phonemes.push_back("SH");
            ++i;
        } else if (c == 'C' && next == 'H') {
            phonemes.push_back("CH");
            ++i;
        } else if (c == 'T' && next == 'H') {
            phonemes.push_back("TH");
            ++i;
        } else if (c == 'N' && next == 'G') {
            phonemes.push_back("NG");
            ++i;
        } else {
            // Single character approximations
            switch (c) {
            case 'A':
                phonemes.push_back("AE");
                break;
            case 'B':
                phonemes.push_back("B");
                break;
            case 'C':
                phonemes.push_back("K");
                break;
            case 'D':
                phonemes.push_back("D");
                break;
            case 'E':
                phonemes.push_back("EH");
                break;
            case 'F':
                phonemes.push_back("F");
                break;
            case 'G':
                phonemes.push_back("G");
                break;
            case 'H':
                phonemes.push_back("HH");
                break;
            case 'I':
                phonemes.push_back("IH");
                break;
            case 'J':
                phonemes.push_back("JH");
                break;
            case 'K':
                phonemes.push_back("K");
                break;
            case 'L':
                phonemes.push_back("L");
                break;
            case 'M':
                phonemes.push_back("M");
                break;
            case 'N':
                phonemes.push_back("N");
                break;
            case 'O':
                phonemes.push_back("OW");
                break;
            case 'P':
                phonemes.push_back("P");
                break;
            case 'Q':
                phonemes.push_back("K");
                break;
            case 'R':
                phonemes.push_back("R");
                break;
            case 'S':
                phonemes.push_back("S");
                break;
            case 'T':
                phonemes.push_back("T");
                break;
            case 'U':
                phonemes.push_back("UW");
                break;
            case 'V':
                phonemes.push_back("V");
                break;
            case 'W':
                phonemes.push_back("W");
                break;
            case 'X':
                phonemes.push_back("K");
                phonemes.push_back("S");
                break;
            case 'Y':
                phonemes.push_back("Y");
                break;
            case 'Z':
                phonemes.push_back("Z");
                break;
            default:
                break;
            }
        }
    }

    return phonemes;
}

std::vector<RhubarbMouthCue> TextToViseme::wordsToMouthCues(const std::vector<WordTiming> &words) const {
    std::vector<RhubarbMouthCue> cues;

    if (words.empty()) {
        return cues;
    }

    // Add initial silence if first word doesn't start at 0
    if (words.front().startTime > 0.01) {
        RhubarbMouthCue silence;
        silence.start = 0.0;
        silence.end = words.front().startTime;
        silence.value = "X";
        cues.push_back(silence);
    }

    for (size_t wordIdx = 0; wordIdx < words.size(); ++wordIdx) {
        const auto &word = words[wordIdx];
        double wordDuration = word.endTime - word.startTime;

        if (wordDuration <= 0.0) {
            continue;
        }

        auto phonemes = lookupPhonemes(word.word);
        if (phonemes.empty()) {
            // Unknown word with no phonemes - use a generic open mouth
            RhubarbMouthCue cue;
            cue.start = word.startTime;
            cue.end = word.endTime;
            cue.value = "B";
            cues.push_back(cue);
            continue;
        }

        // Distribute phonemes evenly across the word duration
        double phonemeDuration = wordDuration / static_cast<double>(phonemes.size());

        for (size_t i = 0; i < phonemes.size(); ++i) {
            RhubarbMouthCue cue;
            cue.start = word.startTime + static_cast<double>(i) * phonemeDuration;
            cue.end = word.startTime + static_cast<double>(i + 1) * phonemeDuration;
            cue.value = arpabetToViseme(phonemes[i]);
            cues.push_back(cue);
        }

        // Add silence between words if there's a gap
        if (wordIdx + 1 < words.size()) {
            double gap = words[wordIdx + 1].startTime - word.endTime;
            if (gap > 0.02) { // Only insert silence for gaps > 20ms
                RhubarbMouthCue silence;
                silence.start = word.endTime;
                silence.end = words[wordIdx + 1].startTime;
                silence.value = "X";
                cues.push_back(silence);
            }
        }
    }

    // Merge consecutive cues with the same viseme to reduce output size
    if (cues.size() > 1) {
        std::vector<RhubarbMouthCue> merged;
        merged.push_back(cues.front());

        for (size_t i = 1; i < cues.size(); ++i) {
            if (cues[i].value == merged.back().value) {
                // Extend previous cue
                merged.back().end = cues[i].end;
            } else {
                merged.push_back(cues[i]);
            }
        }

        cues = std::move(merged);
    }

    return cues;
}

std::vector<RhubarbMouthCue> TextToViseme::charTimingsToMouthCues(const std::vector<CharTiming> &chars) const {
    if (chars.empty()) {
        return {};
    }

    // Reconstruct words from character sequence and their timing
    std::vector<WordTiming> words;
    std::string currentWord;
    double wordStart = 0.0;
    double wordEnd = 0.0;

    for (size_t i = 0; i < chars.size(); ++i) {
        char c = chars[i].character;
        double charStartSec = chars[i].startTimeMs / 1000.0;
        double charEndSec = (chars[i].startTimeMs + chars[i].durationMs) / 1000.0;

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'') {
            if (currentWord.empty()) {
                wordStart = charStartSec;
            }
            currentWord.push_back(c);
            wordEnd = charEndSec;
        } else {
            // Non-alpha character (space, punctuation) = word boundary
            if (!currentWord.empty()) {
                WordTiming wt;
                wt.word = currentWord;
                wt.startTime = wordStart;
                wt.endTime = wordEnd;
                words.push_back(wt);
                currentWord.clear();
            }
        }
    }

    // Don't forget the last word
    if (!currentWord.empty()) {
        WordTiming wt;
        wt.word = currentWord;
        wt.startTime = wordStart;
        wt.endTime = wordEnd;
        words.push_back(wt);
    }

    return wordsToMouthCues(words);
}

} // namespace creatures::voice
