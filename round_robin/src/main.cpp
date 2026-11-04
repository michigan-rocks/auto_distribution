#include "RoundRobinScheduler.h"
#include "queue_names.hpp"

#include <SimpleAmqpClient/SimpleAmqpClient.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::string env(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

int portFromEnv() {
    const std::string value = env("RABBITMQ_PORT", "5672");
    std::size_t parsed = 0;
    const int port = std::stoi(value, &parsed);
    if (parsed != value.size() || port < 1 || port > 65535) {
        throw std::invalid_argument("invalid RABBITMQ_PORT");
    }
    return port;
}

void declareDurableQueue(const AmqpClient::Channel::ptr_t& channel,
                         const std::string& name) {
    channel->DeclareQueue(name, false, true, false, false);
}

} // namespace

int main() {
    try {
        const std::string host = env("RABBITMQ_HOST", "localhost");
        const int port = portFromEnv();
        const std::string user = env("RABBITMQ_USER", "user");
        const std::string password = env("RABBITMQ_PASSWORD", "user");
        const auto channel = AmqpClient::Channel::Create(
            host, port, user, password
        );

        declareDurableQueue(channel, queue_names::incoming_tasks);
        for (int id = 1; id <= queue_names::worker_count; ++id) {
            declareDurableQueue(channel, queue_names::worker_tasks(id));
        }

        RoundRobinScheduler scheduler;
        const std::string consumer_tag = channel->BasicConsume(
            queue_names::incoming_tasks, "", false, false, false, 20
        );
        std::cout << "Round Robin dispatcher connected to " << host
                  << ':' << port << std::endl;

        while (true) {
            const auto envelope = channel->BasicConsumeMessage(consumer_tag);
            const std::string target_queue = scheduler.selectQueue();
            channel->BasicPublish(
                "", target_queue, envelope->Message(), true
            );
            channel->BasicAck(envelope);
            std::cout << "Task routed to " << target_queue << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "Round Robin dispatcher stopped: "
                  << error.what() << '\n';
        return 1;
    }
}
