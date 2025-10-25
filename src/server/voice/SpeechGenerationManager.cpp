#include "SpeechGenerationManager.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "exception/exception.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/voice/AudioConverter.h"
#include "spdlog/spdlog.h"
#include <fmt/format.h>

namespace creatures {
extern std::shared_ptr<creatures::voice::CreatureVoices> voiceService;
}

namespace creatures::voice {

namespace {

std::filesystem::path resolveOutputDirectory(const SpeechGenerationRequest &request) {
    if (!request.outputDirectory.empty()) {
        return request.outputDirectory;
    }
    return std::filesystem::path(creatures::config->getSoundFileLocation());
}

std::shared_ptr<CreatureVoices> resolveVoiceClient(const SpeechGenerationRequest &request) {
    if (request.voiceClient) {
        return request.voiceClient;
    }
    return std::make_shared<CreatureVoices>(creatures::config->getVoiceApiKey());
}

} // namespace

Result<SpeechGenerationResult> SpeechGenerationManager::generate(const SpeechGenerationRequest &request) {
    if (request.creatureId.empty()) {
        return Result<SpeechGenerationResult>{
            ServerError(ServerError::InvalidData, "Speech generation requires a creature_id")};
    }

    if (request.text.empty()) {
        return Result<SpeechGenerationResult>{
            ServerError(ServerError::InvalidData, "Speech generation text may not be empty")};
    }

    auto span =
        creatures::observability->createChildOperationSpan("SpeechGenerationManager.generate", request.parentSpan);
    if (span) {
        span->setAttribute("creature.id", request.creatureId);
        span->setAttribute("text.length", static_cast<int64_t>(request.text.size()));
    }

    try {
        auto outputDir = resolveOutputDirectory(request);
        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec) {
            std::string errorMessage =
                fmt::format("Unable to create output directory {}: {}", outputDir.string(), ec.message());
            if (span) {
                span->setError(errorMessage);
            }
            return Result<SpeechGenerationResult>{ServerError(ServerError::InternalError, errorMessage)};
        }

        if (span) {
            span->setAttribute("output.directory", outputDir.string());
        }

        // Fetch creature JSON for voice config
        auto creatureJsonResult = creatures::db->getCreatureJson(request.creatureId, span);
        if (!creatureJsonResult.isSuccess()) {
            auto error = creatureJsonResult.getError().value();
            if (span) {
                span->setError(error.getMessage());
            }
            return Result<SpeechGenerationResult>{error};
        }
        auto creatureJson = creatureJsonResult.getValue().value();

        auto creatureResult = creatures::db->getCreature(request.creatureId, span);
        if (!creatureResult.isSuccess()) {
            auto error = creatureResult.getError().value();
            if (span) {
                span->setError(error.getMessage());
            }
            return Result<SpeechGenerationResult>{error};
        }
        auto creatureModel = creatureResult.getValue().value();

        if (span) {
            span->setAttribute("creature.name", creatureModel.name);
        }

        if (!creatureJson.contains("voice") || creatureJson["voice"].is_null()) {
            std::string errorMessage = fmt::format("No voice configuration found for creature {}", request.creatureId);
            if (span) {
                span->setError(errorMessage);
            }
            return Result<SpeechGenerationResult>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        uint16_t audioChannel = 1;
        try {
            if (creatureJson.contains("audio_channel") && !creatureJson["audio_channel"].is_null()) {
                audioChannel = creatureJson["audio_channel"].get<uint16_t>();
            }
        } catch (const std::exception &e) {
            warn("Failed to parse audio_channel for creature {}: {}", request.creatureId, e.what());
        }

        auto voiceConfig = creatureJson["voice"];
        CreatureSpeechRequest speechRequest;
        speechRequest.creature_name = creatureJson.value("name", request.creatureId);
        speechRequest.title = request.title.empty() ? fmt::format("adhoc-{}", request.creatureId) : request.title;
        speechRequest.text = request.text;

        try {
            speechRequest.voice_id = voiceConfig["voice_id"].get<std::string>();
            speechRequest.model_id = voiceConfig["model_id"].get<std::string>();
            speechRequest.stability = voiceConfig["stability"].get<float>();
            speechRequest.similarity_boost = voiceConfig["similarity_boost"].get<float>();
        } catch (const std::exception &e) {
            std::string errorMessage =
                fmt::format("Failed to parse voice configuration for creature {}: {}", request.creatureId, e.what());
            if (span) {
                span->setError(errorMessage);
            }
            return Result<SpeechGenerationResult>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        if (span) {
            span->setAttribute("voice.id", speechRequest.voice_id);
            span->setAttribute("voice.model", speechRequest.model_id);
            span->setAttribute("audio.channel", static_cast<int64_t>(audioChannel));
        }

        auto voiceClient = resolveVoiceClient(request);
        auto mp3Result = voiceClient->generateCreatureSpeech(outputDir, speechRequest);
        if (!mp3Result.isSuccess()) {
            auto error = mp3Result.getError().value();
            if (span) {
                span->setError(error.getMessage());
            }
            ServerError::Code serverCode = ServerError::InternalError;
            switch (error.getCode()) {
            case creatures::voice::VoiceError::InvalidData:
                serverCode = ServerError::InvalidData;
                break;
            case creatures::voice::VoiceError::NotFound:
                serverCode = ServerError::NotFound;
                break;
            case creatures::voice::VoiceError::Forbidden:
            case creatures::voice::VoiceError::InvalidApiKey:
                serverCode = ServerError::Forbidden;
                break;
            case creatures::voice::VoiceError::InternalError:
            default:
                serverCode = ServerError::InternalError;
                break;
            }
            return Result<SpeechGenerationResult>{ServerError(serverCode, error.getMessage())};
        }
        auto mp3Data = mp3Result.getValue().value();

        auto mp3Path = outputDir / mp3Data.sound_file_name;
        auto wavPath = mp3Path;
        wavPath.replace_extension(".wav");
        auto transcriptPath = outputDir / mp3Data.transcript_file_name;

        auto convertSpan =
            creatures::observability->createChildOperationSpan("SpeechGenerationManager.convertToWav", span);
        if (convertSpan) {
            convertSpan->setAttribute("audio.target_channel", static_cast<int64_t>(audioChannel));
        }
        auto conversion = AudioConverter::convertMp3ToWav(mp3Path, wavPath, creatures::config->getFfmpegBinaryPath(),
                                                          audioChannel, 48000, convertSpan);
        if (!conversion.isSuccess()) {
            auto error = conversion.getError().value();
            if (span) {
                span->setError(error.getMessage());
            }
            return Result<SpeechGenerationResult>{error};
        }

        SpeechGenerationResult result;
        result.response.success = true;
        result.response.sound_file_name = wavPath.filename().string();
        result.response.transcript_file_name = mp3Data.transcript_file_name;
        result.response.sound_file_size = conversion.getValue().value();
        result.mp3Path = mp3Path;
        result.wavPath = wavPath;
        result.transcriptPath = transcriptPath;
        result.audioChannel = audioChannel;
        result.creature = creatureModel;
        result.creatureJson = creatureJson;

        if (span) {
            span->setSuccess();
        }

        return Result<SpeechGenerationResult>{result};

    } catch (const std::exception &e) {
        if (span) {
            span->recordException(e);
            span->setError(e.what());
        }
        return Result<SpeechGenerationResult>{ServerError(ServerError::InternalError, e.what())};
    }
}

} // namespace creatures::voice
