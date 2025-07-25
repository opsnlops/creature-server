
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace creatures {

std::shared_ptr<spdlog::logger>
makeLogger(std::string name, spdlog::level::level_enum defaultLevel);
}