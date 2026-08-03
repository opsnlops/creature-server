#include "util/Sha256.h"

#include <array>
#include <iomanip>
#include <memory>
#include <sstream>

#include <openssl/evp.h>

namespace creatures::util {

std::string sha256Hex(std::span<const uint8_t> bytes) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context.get(), hash.data(), &length) != 1) {
        return {};
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) {
        output << std::setw(2) << static_cast<unsigned int>(hash[i]);
    }
    return output.str();
}

std::string sha256Hex(const std::string &text) {
    return sha256Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}

} // namespace creatures::util
