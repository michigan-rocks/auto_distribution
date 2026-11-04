#include "RoundRobinScheduler.h"

#include <iostream>
#include <string>

int main() {
    RoundRobinScheduler scheduler;
    const std::string expected[] = {
        "worker.1", "worker.2", "worker.3",
        "worker.1", "worker.2", "worker.3"
    };
    for (const auto& queue : expected) {
        if (scheduler.selectQueue() != queue) {
            std::cerr << "Round Robin queue order is incorrect\n";
            return 1;
        }
    }

    RoundRobinScheduler another;
    if (another.selectQueue() != "worker.1") {
        std::cerr << "A new scheduler did not start at worker 1\n";
        return 1;
    }
}
