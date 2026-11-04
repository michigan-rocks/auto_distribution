#include <iostream>
#include <cstdlib>
#include <string>

#include "routing.hpp"
#include "workload.hpp"
#include "queue.cpp"

int main() {
    try {
        const char* mode = std::getenv("PRODUCER_MODE");
        const std::string target_queue = producer::queueForMode(
            mode ? mode : "adaptive"
        );
        auto channel = connectRabbitMQ();
        createQueue(channel, target_queue);
        const char* workload = std::getenv("PRODUCER_WORKLOAD");
        if (workload && *workload) {
            producer::runWorkload(channel, target_queue,
                                 mode ? mode : "adaptive", workload);
        } else {
            publishRandomTasks(channel, target_queue);
        }

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }


    return 0;
}



