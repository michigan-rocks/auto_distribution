#pragma once

#include <stdexcept>
#include <string>

namespace queue_names {

inline constexpr int worker_count = 3;

inline constexpr const char* incoming_tasks = "tasks.incoming";
inline constexpr const char* shared_tasks = "tasks.shared";
inline constexpr const char* worker_calibration = "workers.calibration";
inline constexpr const char* completed_tasks = "tasks.completed";
inline constexpr const char* task_results = "tasks.results";

inline std::string worker_tasks(int worker_id) {
    if (worker_id < 1 || worker_id > worker_count) {
        throw std::invalid_argument("invalid worker id");
    }
    return "worker." + std::to_string(worker_id);
}

} // namespace queue_names
