
#include <string>

#include "HttpMethod.h"

namespace creatures :: voice {

    std::string httpMethodToString(HttpMethod method) {
        switch (method) {
            case HttpMethod::GET: return{"GET"};
            case HttpMethod::POST: return {"POST"};
            case HttpMethod::PUT: return {"PUT"};
            case HttpMethod::DELETE: return {"DELETE"};
            default: return {"UNKNOWN"};
        }
    }


} // namespace creatures :: voice