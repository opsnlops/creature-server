
#include "uuidUtils.h"

#include <uuid/uuid.h>

namespace creatures::util {

std::string generateUUID() {
    uuid_t uuid;
    uuid_generate(uuid);

    char uuidStr[37]; // UUID string is 36 characters + null terminator
    uuid_unparse(uuid, uuidStr);

    return std::string(uuidStr);
}

} // namespace creatures::util
