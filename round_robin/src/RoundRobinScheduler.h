#pragma once

#include "queue_names.hpp"

#include <string>

class RoundRobinScheduler {
public:
    int selectWorker() {
        const int worker = nextWorker_;
        nextWorker_ = nextWorker_ % queue_names::worker_count + 1;
        return worker;
    }

    std::string selectQueue() {
        return queue_names::worker_tasks(selectWorker());
    }

private:
    int nextWorker_ = 1;
};
