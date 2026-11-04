#pragma once

#include <stdexcept>
#include <string>
#include <cstdlib>

namespace queue_names {

inline constexpr int worker_count = 3;

inline std::string scoped(const std::string& name) {
    const char* experiment = std::getenv("EXPERIMENT_ID");
    return experiment && *experiment ? std::string(experiment) + "." + name : name;
}

inline const std::string incoming_tasks = scoped("tasks.incoming");
inline const std::string shared_tasks = scoped("tasks.shared");
inline const std::string worker_calibration = scoped("workers.calibration");
inline const std::string completed_tasks = scoped("tasks.completed");
inline const std::string task_results = scoped("tasks.results");

inline std::string worker_tasks(int worker_id) {
    if (worker_id < 1 || worker_id > worker_count) {
        throw std::invalid_argument("invalid worker id");
    }
    return scoped("worker." + std::to_string(worker_id));
}

} // namespace queue_names
