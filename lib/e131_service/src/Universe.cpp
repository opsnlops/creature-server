
#include <array>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "Universe.h"

namespace creatures::e131 {

Universe::Universe(std::shared_ptr<spdlog::logger> _logger)
    : logger(_logger) { // NOLINT(*-unnecessary-value-param, *-pass-by-value)
    logger->debug("new Universe ✨");
}

void Universe::setFragment(uint16_t firstSlot, std::vector<uint8_t> &values) {

    // Only one thread can touch this at a time
    std::lock_guard<std::mutex> lock(valuesMutex);

    // Get the number of elements in the vector
    size_t N = values.size();

    // Useful for debugging sometimes
    logger->trace("firstSlot + N = {}, UNIVERSE_SLOT_COUNT = {}, firstSlot = {}", (firstSlot + N), UNIVERSE_SLOT_COUNT,
                  firstSlot);

    // Ensure the firstSlot plus the size of values doesn't exceed UNIVERSE_SLOT_COUNT and firstSlot isn't zero
    // Also protect against integer overflow in the addition
    if (firstSlot > 0 && firstSlot <= UNIVERSE_SLOT_COUNT && N <= UNIVERSE_SLOT_COUNT &&
        (UNIVERSE_SLOT_COUNT - firstSlot) >= N) {

        // Copy the values into the state
        std::copy(values.begin(), values.end(), state.begin() + firstSlot);
        logger->debug("wrote {} bytes to slot {}", N, firstSlot);

    } else {

        // Oh dear 𐂂
        logger->error("setFragment: Attempt to write beyond universe bounds.");
    }
}

std::array<uint8_t, UNIVERSE_SLOT_COUNT> Universe::getState() {

    // Don't let other people touch the state while we're copying it
    std::lock_guard<std::mutex> lock(valuesMutex);
    auto stateCopy = state;

    return stateCopy;
}

uint8_t Universe::getNextSequenceNumber() { return this->sequenceNumber++; }

} // namespace creatures::e131