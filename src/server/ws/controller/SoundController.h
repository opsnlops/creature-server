
#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>


#include "server/database.h"


#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/SoundService.h"


#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures :: ws {

    class SoundController : public oatpp::web::server::api::ApiController {
    public:
        SoundController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        SoundService m_soundService; // Create the sound service
    public:

        static std::shared_ptr<SoundController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper) // Inject objectMapper component here as default parameter
        ) {
            return std::make_shared<SoundController>(objectMapper);
        }


        ENDPOINT_INFO(getAllSounds) {
            info->summary = "Lists all of the sound files";

            info->addResponse<Object<SoundsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/sound", getAllSounds)
        {
            return createDtoResponse(Status::CODE_200, m_soundService.getAllSounds());
        }



        ENDPOINT_INFO(playSound) {
            info->summary = "Queue up a sound to play on the next frame";

            info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/sound/play/{fileName}", playSound,
                 PATH(String, fileName))
        {
            return createDtoResponse(Status::CODE_200, m_soundService.playSound(fileName));
        }


    };

}

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen