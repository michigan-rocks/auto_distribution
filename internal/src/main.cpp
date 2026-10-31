#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <iostream>
#include <cstdlib>
#include <string>
#include "RoundRobinScheduler.h"

using namespace std;

string getEnv(
    const char* name,
    const string& fallback
) {
    const char* value = getenv(name);
    return value ? value : fallback;
}

int main() {
    try {

        RoundRobinScheduler scheduler;

        auto channel = AmqpClient::Channel::Create(
            "rabbitmq",
            5672,
            "user",
            "user"
        );


        for (int i = 1; i <= 3; ++i) {
            string queueName = "worker." + to_string(i);

            channel->DeclareQueue(
                queueName,
                false, // passive
                true,  // durable
                false, // exclusive
                false  // auto_delete
            );
        }


        // Подписываемся на существующую очередь
        string consumerTag = channel->BasicConsume(
            "tasks.incoming",
            "",
            false,  // no_local
            false, // no_ack
            false  // exclusive
        );

        
        while (true) {
            AmqpClient::Envelope::ptr_t envelope;

            // Ждём следующее сообщение
            bool received = channel->BasicConsumeMessage(
                consumerTag,
                envelope
            );

            if (!received) {
                continue;
            }

            
            

            int workerId = scheduler.selectWorker();

            string targetQueue =
                "worker." + to_string(workerId);

            
            channel->BasicPublish(
                "",                   // Default Exchange
                targetQueue,          // worker.1 / worker.2 / worker.3
                envelope->Message(),
                true                 // mandatory
            );

            std::cout << "Task routed to " << targetQueue
                << " | payload bytes: "
                << envelope->Message()->Body().size()
                << std::endl;

            // Подтверждаем обработку
            channel->BasicAck(envelope);
        }

    } catch (const exception& e) {
        cerr << "RabbitMQ error: " << e.what() << '\n';
        return 1;
    }
}