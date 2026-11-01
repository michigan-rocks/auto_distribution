#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <cstdlib>
#include <string>
#include "RoundRobinScheduler.h"
#include "AdaptiveScheduler.h"

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
        
        // RoundRobinScheduler scheduler;
        AdaptiveScheduler adaptiveScheduler;

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

        channel->DeclareQueue(
            "workers.calibration",
            false,
            true,
            false,
            false
        );

        string calibrationTag = channel->BasicConsume(
        "workers.calibration",
        "",
        false,
        false, // manual ACK
        false,
        1
        );  

        std::array<bool, 3> ready{false, false, false};
        int calibratedCount = 0;


        
        while (calibratedCount < 3) {
            AmqpClient::Envelope::ptr_t envelope;

            bool received = channel->BasicConsumeMessage(
                calibrationTag,
                envelope
            );

            if (!received) {
                continue;
            }

            try {
                auto data = nlohmann::json::parse(
                    envelope->Message()->Body()
                );

                std::string type =
                    data.at("type").get<std::string>();

                int workerId =
                    data.at("worker_id").get<int>();

                int matrixSize =
                    data.at("matrix_size").get<int>();

                double averageMs =
                    data.at("average_ms").get<double>();

                if (type != "calibration" ||
                    workerId < 1 || workerId > 3 ||
                    matrixSize != 400 ||
                    !std::isfinite(averageMs) ||
                    averageMs <= 0) {
                    throw std::runtime_error("Invalid calibration");
                }

                // Обновляем статистику Scheduler
                scheduler.calibrateWorker(workerId, averageMs);

                if (!ready[workerId - 1]) {
                    ready[workerId - 1] = true;
                    ++calibratedCount;
                }

                std::cout << "Worker " << workerId
                        << " calibrated: " << averageMs
                        << " ms\n";

            } catch (const std::exception& e) {
                std::cerr << "Invalid calibration message: "
                        << e.what() << '\n';
            }

            channel->BasicAck(envelope);
        }

        channel->BasicCancel(calibrationTag);

        std::cout << "All workers calibrated!\n";


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

            
        
            // int workerId = scheduler.selectWorker();

            auto decision = adaptiveScheduler.chooseWorker();

            std::cout << "Selected worker: "
                    << decision.workerId << '\n';

            std::cout << "Predicted finish: "
                    << decision.finishMs << " ms\n";
                    
            adaptiveScheduler.onAssigned(1, 400, decision.workerId);

            string targetQueue =
                "worker." + to_string(decision.workerId);

            
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