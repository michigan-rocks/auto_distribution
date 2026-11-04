#pragma once

#include "queue_names.hpp"

#include <stdexcept>
#include <string>

namespace producer {

inline std::string queueForMode(const std::string& mode) {
    if (mode == "adaptive" || mode == "round-robin") {
        return queue_names::incoming_tasks;
    }
    if (mode == "baseline") {
        return queue_names::shared_tasks;
    }
    throw std::invalid_argument("invalid PRODUCER_MODE: " + mode);
}

} // namespace producer
