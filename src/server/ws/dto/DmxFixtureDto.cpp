#include "server/ws/dto/DmxFixtureDto.h"

namespace creatures {

DmxFixture convertFromDto(const std::shared_ptr<DmxFixtureDto> &fixtureDto) {
    DmxFixture fixture;
    fixture.id = fixtureDto->id;
    fixture.name = fixtureDto->name;
    fixture.type = fixtureTypeFromString(fixtureDto->type);
    fixture.channel_offset = fixtureDto->channel_offset;

    if (fixtureDto->assigned_universe) {
        fixture.assigned_universe = *fixtureDto->assigned_universe;
    }
    if (fixtureDto->channels) {
        for (const auto &channelDto : *fixtureDto->channels) {
            if (!channelDto)
                continue;
            fixture.channels.push_back(
                {channelDto->offset, channelDto->name, channelDto->kind ? std::string(channelDto->kind) : "generic"});
        }
    }
    if (fixtureDto->patterns) {
        for (const auto &patternDto : *fixtureDto->patterns) {
            if (!patternDto)
                continue;
            FixturePattern pattern{patternDto->id,
                                   patternDto->name,
                                   {},
                                   patternDto->fade_in_ms ? static_cast<uint32_t>(*patternDto->fade_in_ms) : 0u,
                                   patternDto->fade_out_ms ? static_cast<uint32_t>(*patternDto->fade_out_ms) : 0u,
                                   patternDto->hold_ms ? static_cast<uint32_t>(*patternDto->hold_ms) : 0u};
            if (patternDto->values) {
                for (const auto &valueDto : *patternDto->values) {
                    if (valueDto)
                        pattern.values.push_back({valueDto->channel, valueDto->value});
                }
            }
            fixture.patterns.push_back(std::move(pattern));
        }
    }
    if (fixtureDto->bindings) {
        for (const auto &bindingDto : *fixtureDto->bindings) {
            if (!bindingDto)
                continue;
            FixtureBinding binding{bindingDto->creature_id, std::nullopt, std::nullopt, bindingDto->pattern_id};
            if (bindingDto->on_reason)
                binding.on_reason = std::string(bindingDto->on_reason);
            if (bindingDto->on_state)
                binding.on_state = std::string(bindingDto->on_state);
            fixture.bindings.push_back(std::move(binding));
        }
    }
    return fixture;
}

oatpp::Object<DmxFixtureDto> convertToDto(const DmxFixture &fixture) {
    auto dto = DmxFixtureDto::createShared();
    dto->id = fixture.id;
    dto->name = fixture.name;
    dto->type = fixtureTypeToString(fixture.type);
    dto->channel_offset = fixture.channel_offset;
    if (fixture.assigned_universe)
        dto->assigned_universe = *fixture.assigned_universe;

    dto->channels = oatpp::List<oatpp::Object<FixtureChannelDto>>::createShared();
    for (const auto &channel : fixture.channels) {
        auto channelDto = FixtureChannelDto::createShared();
        channelDto->offset = channel.offset;
        channelDto->name = channel.name;
        channelDto->kind = channel.kind;
        dto->channels->push_back(channelDto);
    }
    if (!fixture.patterns.empty()) {
        dto->patterns = oatpp::List<oatpp::Object<FixturePatternDto>>::createShared();
        for (const auto &pattern : fixture.patterns) {
            auto patternDto = FixturePatternDto::createShared();
            patternDto->id = pattern.id;
            patternDto->name = pattern.name;
            patternDto->fade_in_ms = pattern.fade_in_ms;
            patternDto->fade_out_ms = pattern.fade_out_ms;
            patternDto->hold_ms = pattern.hold_ms;
            patternDto->values = oatpp::List<oatpp::Object<FixturePatternValueDto>>::createShared();
            for (const auto &value : pattern.values) {
                auto valueDto = FixturePatternValueDto::createShared();
                valueDto->channel = value.channel;
                valueDto->value = value.value;
                patternDto->values->push_back(valueDto);
            }
            dto->patterns->push_back(patternDto);
        }
    }
    if (!fixture.bindings.empty()) {
        dto->bindings = oatpp::List<oatpp::Object<FixtureBindingDto>>::createShared();
        for (const auto &binding : fixture.bindings) {
            auto bindingDto = FixtureBindingDto::createShared();
            bindingDto->creature_id = binding.creature_id;
            bindingDto->pattern_id = binding.pattern_id;
            if (binding.on_reason)
                bindingDto->on_reason = *binding.on_reason;
            if (binding.on_state)
                bindingDto->on_state = *binding.on_state;
            dto->bindings->push_back(bindingDto);
        }
    }
    return dto;
}

} // namespace creatures
