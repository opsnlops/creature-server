
#pragma once

#include <fmt/format.h>

#include "Version.h"

#include <oatpp-swagger/Model.hpp>
#include <oatpp-swagger/Resources.hpp>
#include <oatpp/core/macro/component.hpp>

/**
 *  Swagger ui is served at
 *  http://host:port/swagger/ui
 */
class SwaggerComponent {
public:

    /**
     *  General API docs info
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::swagger::DocumentInfo>, swaggerDocumentInfo)([] {

        oatpp::swagger::DocumentInfo::Builder builder;

        // App version
        std::string version = fmt::format("{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR, CREATURE_SERVER_VERSION_MINOR, CREATURE_SERVER_VERSION_PATCH);

        builder
                .setTitle("Creature Server")
                .setDescription("The backend server at April's Creature Workshop")
                .setVersion(version)
                .setContactName("April White")
                .setContactUrl("https://creature.engineering")

                .setLicenseName("MIT")
                .setLicenseUrl("https://opensource.org/license/mit")

                .setTermsOfService("Private")

                .addServer("https://server.prod.chirpchirp.dev", "production")
                .addServer("http://localhost:8000", "localhost");



        return builder.build();

    }());


    /**
     *  Swagger-Ui Resources (<oatpp-examples>/lib/oatpp-swagger/res)
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::swagger::Resources>, swaggerResources)([] {
        return oatpp::swagger::Resources::loadResources(SWAGGER_UI_PATH);
    }());

};
