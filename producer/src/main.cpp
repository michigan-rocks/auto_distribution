#include <iostream>
#include <string>

#include "queue.cpp"

int main() {
    try {
        auto channel = connectRabbitMQ();
        createQueue(channel);
        publishRandomTasks(channel);

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }


    return 0;
}



