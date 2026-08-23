

#include <base64.hpp>

#include "model/JsonCodec.h"
#include "model/StreamFrame.h"
#include "util/helpers.h"

namespace creatures {

namespace {

template <typename T> Result<StreamFrame> forwardStreamFrameError(const Result<T> &result) {
    return Result<StreamFrame>{result.getError().value()};
}

} // namespace

nlohmann::json streamFrameToJson(const StreamFrame &streamFrame) {
    return {{"creature_id", streamFrame.creature_id}, {"universe", streamFrame.universe}, {"data", streamFrame.data}};
}

Result<StreamFrame> streamFrameFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(json, path, {"creature_id", "universe", "data"});
    if (!fields.isSuccess())
        return forwardStreamFrameError(fields);
    auto creatureId = json_codec::requiredString(json, path, "creature_id", 36);
    auto universe = json_codec::requiredUnsigned<uint32_t>(json, path, "universe", MAX_E131_UNIVERSE);
    auto data = json_codec::requiredString(json, path, "data", MAX_STREAM_FRAME_ENCODED_BYTES);
    if (!creatureId.isSuccess())
        return forwardStreamFrameError(creatureId);
    if (!universe.isSuccess())
        return forwardStreamFrameError(universe);
    if (!data.isSuccess())
        return forwardStreamFrameError(data);
    if (!isUuidShape(creatureId.getValue().value()))
        return json_codec::invalid<StreamFrame>(fmt::format("{}.creature_id must be a UUID", path));
    if (universe.getValue().value() == 0)
        return json_codec::invalid<StreamFrame>(fmt::format("{}.universe must be in [1, {}]", path, MAX_E131_UNIVERSE));
    try {
        if (base64::from_base64(data.getValue().value()).size() > MAX_STREAM_FRAME_DECODED_BYTES) {
            return json_codec::invalid<StreamFrame>(
                fmt::format("{}.data decodes to more than {} bytes", path, MAX_STREAM_FRAME_DECODED_BYTES));
        }
    } catch (const std::exception &error) {
        return json_codec::invalid<StreamFrame>(fmt::format("{}.data is not valid base64: {}", path, error.what()));
    }

    return Result<StreamFrame>{
        StreamFrame{creatureId.getValue().value(), universe.getValue().value(), data.getValue().value()}};
}

} // namespace creatures
