#pragma once

#include <string>
#include <vector>

#include "server/voice/IxmlWriter.h"

namespace creatures {

struct Sound {
    std::string fileName;
    uint32_t size{0};
    std::string transcript;
    std::string lipsync;
    std::string title;
    std::string sourceScriptId;
    std::string script;
    std::string generationIds;
    bool hasEmbeddedScript{false};
    bool hasEmbeddedLipsync{false};

    std::vector<voice::DialogScriptLine> scriptTurns;
    std::vector<voice::DialogTrackInfo> tracks;
    std::vector<voice::DialogLipsyncTrack> lipsyncTracks;
    std::vector<voice::DialogWordTrack> wordTracks;
};

} // namespace creatures
