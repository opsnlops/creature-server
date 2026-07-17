
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/// Completion `result` payload for a DialogPreviewExport job. The 17-channel
/// WAV is written into the ad-hoc sound bucket, so `file_name` is downloadable
/// through GET /api/v1/sound/ad-hoc/{filename}. Shareable/playable renditions are
/// addressed by the source *stem* plus a rendition extension, not the raw `.wav`
/// basename: GET /api/v1/sound/mp3/{stem}.mp3 and /api/v1/sound/shareable/{stem}.ogg
/// (issue #57).
class DialogPreviewExportResultDto : public oatpp::DTO {

    DTO_INIT(DialogPreviewExportResultDto, DTO)

    DTO_FIELD_INFO(file_name) {
        info->description = "Basename of the assembled 17-channel WAV in the ad-hoc sound bucket.";
    }
    DTO_FIELD(String, file_name);

    DTO_FIELD_INFO(generation_id) { info->description = "UUID of the dialog generation the WAV was assembled from."; }
    DTO_FIELD(String, generation_id);

    DTO_FIELD_INFO(cache_key) { info->description = "sha256(turns) — stable identifier for this exact input."; }
    DTO_FIELD(String, cache_key);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
