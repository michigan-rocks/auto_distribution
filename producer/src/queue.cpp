#include <chrono>
#include <cstdlib>
#include <iostream>
#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <string>
#include <thread>

std::string CreateRandomTask();

auto connectRabbitMQ() {
    const std::string host = std::getenv("RABBITMQ_HOST")
        ? std::getenv("RABBITMQ_HOST")
        : "localhost";
    const std::string user = std::getenv("RABBITMQ_USER")
        ? std::getenv("RABBITMQ_USER")
        : "user";
    const std::string password = std::getenv("RABBITMQ_PASSWORD")
        ? std::getenv("RABBITMQ_PASSWORD")
        : "user";

    auto channel = AmqpClient::Channel::Create(
        host,
        5672,
        user,
        password
    );

    std::cout << "RabbitMQ connected\n";
    return channel;
}

void createQueue(AmqpClient::Channel::ptr_t &channel,
                 const std::string& target_queue) {
    channel->DeclareQueue(
        target_queue,
        false, // passive
        true,  // durable
        false, // exclusive
        false  // auto_delete
    );
}

void publish(const std::string& body, AmqpClient::Channel::ptr_t& channel,
             const std::string& target_queue) {

    auto message = AmqpClient::BasicMessage::Create(body);
    message->ContentType("application/json");

    channel->BasicPublish(
        "",               // Default Exchange
        target_queue,     // Routing Key
        message
    );
}

void publishRandomTasks(AmqpClient::Channel::ptr_t& channel,
                        const std::string& target_queue) {

    while (true) {
        const std::string body = CreateRandomTask();
        publish(body, channel, target_queue);

        std::cout << "Task published\n";

        const int delay_ms = 100 + std::rand() % 401;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}
