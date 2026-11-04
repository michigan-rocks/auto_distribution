#include "AdaptiveScheduler.h"
#include "queue_names.hpp"

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using Json = nlohmann::json;

namespace {

struct InvalidMessage : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct RabbitConfig {
    std::string host;
    int port;
    std::string user;
    std::string password;
};

struct TaskDescriptor {
    std::uint64_t id;
    int matrix_size;
};

struct Completion {
    std::uint64_t task_id;
    int worker_id;
    double elapsed_ms;
};

std::string env(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

int envInt(const char* name, int fallback, int min, int max) {
    const std::string value = env(name, std::to_string(fallback));
    std::size_t parsed = 0;
    const int number = std::stoi(value, &parsed);
    if (parsed != value.size() || number < min || number > max) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return number;
}

RabbitConfig rabbitConfig() {
    return RabbitConfig{
        env("RABBITMQ_HOST", "localhost"),
        envInt("RABBITMQ_PORT", 5672, 1, 65535),
        env("RABBITMQ_USER", "user"),
        env("RABBITMQ_PASSWORD", "user")
    };
}

AmqpClient::Channel::ptr_t connect(const RabbitConfig& config) {
    return AmqpClient::Channel::Create(
        config.host, config.port, config.user, config.password
    );
}

void declareDurableQueue(
    const AmqpClient::Channel::ptr_t& channel,
    const std::string& name
) {
    channel->DeclareQueue(name, false, true, false, false);
}

std::uint64_t positiveId(const Json& value, const char* field) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto id = value.get<std::int64_t>();
        if (id >= 0) {
            return static_cast<std::uint64_t>(id);
        }
    }
    throw InvalidMessage(std::string("invalid ") + field);
}

TaskDescriptor parseTask(const std::string& body) {
    try {
        const Json data = Json::parse(body);
        if (!data.is_object()) {
            throw InvalidMessage("task must be a JSON object");
        }
        const auto task_id = positiveId(data.at("task_id"), "task_id");
        const int matrix_size = data.at("matrix_size").get<int>();
        if (matrix_size < 1 || matrix_size > 1024) {
            throw InvalidMessage("unsupported matrix_size");
        }
        return TaskDescriptor{task_id, matrix_size};
    } catch (const Json::exception& error) {
        throw InvalidMessage(std::string("invalid task: ") + error.what());
    }
}

Completion parseCompletion(const std::string& body) {
    try {
        const Json data = Json::parse(body);
        if (!data.is_object() ||
            data.at("type").get<std::string>() != "completed") {
            throw InvalidMessage("not a completion event");
        }
        const auto task_id = positiveId(data.at("task_id"), "task_id");
        const int worker_id = data.at("worker_id").get<int>();
        const double elapsed_ms = data.at("elapsed_ms").get<double>();
        if (worker_id < 1 || worker_id > queue_names::worker_count) {
            throw InvalidMessage("invalid worker_id");
        }
        if (!std::isfinite(elapsed_ms) || elapsed_ms <= 0.0) {
            throw InvalidMessage("invalid elapsed_ms");
        }
        return Completion{task_id, worker_id, elapsed_ms};
    } catch (const Json::exception& error) {
        throw InvalidMessage(
            std::string("invalid completion event: ") + error.what()
        );
    }
}

void calibrate(
    const AmqpClient::Channel::ptr_t& channel,
    AdaptiveScheduler& scheduler
) {
    const std::string consumer_tag = channel->BasicConsume(
        queue_names::worker_calibration, "", false, false, false, 1
    );
    std::array<bool, queue_names::worker_count> ready{};
    int calibrated_count = 0;

    while (calibrated_count < queue_names::worker_count) {
        const auto envelope = channel->BasicConsumeMessage(consumer_tag);
        try {
            const Json data = Json::parse(envelope->Message()->Body());
            const std::string type = data.at("type").get<std::string>();
            const int worker_id = data.at("worker_id").get<int>();
            const int matrix_size = data.at("matrix_size").get<int>();
            const double average_ms = data.at("average_ms").get<double>();

            if (type != "calibration" ||
                worker_id < 1 || worker_id > queue_names::worker_count ||
                matrix_size != 400 || !std::isfinite(average_ms) ||
                average_ms <= 0.0) {
                throw InvalidMessage("invalid calibration values");
            }

            scheduler.calibrateWorker(worker_id, average_ms);
            if (!ready[worker_id - 1]) {
                ready[worker_id - 1] = true;
                ++calibrated_count;
            }
            std::cout << "Worker " << worker_id
                      << " calibrated: " << average_ms << " ms\n";
        } catch (const std::exception& error) {
            std::cerr << "Discarding invalid calibration: "
                      << error.what() << '\n';
        }
        channel->BasicAck(envelope);
    }

    channel->BasicCancel(consumer_tag);
}

void handleCompletion(
    const AmqpClient::Channel::ptr_t& channel,
    const AmqpClient::Envelope::ptr_t& envelope,
    AdaptiveScheduler& scheduler
) {
    try {
        const Completion completed = parseCompletion(
            envelope->Message()->Body()
        );
        const bool known_task = scheduler.onCompleted(
            completed.task_id,
            completed.worker_id,
            completed.elapsed_ms
        );
        if (known_task) {
            std::cout << "Task " << completed.task_id
                      << " completed by worker "
                      << completed.worker_id << " in "
                      << completed.elapsed_ms << " ms" << std::endl;
        } else {
            std::cerr << "Ignoring completion for unknown task "
                      << completed.task_id << '\n';
        }
        channel->BasicAck(envelope);
    } catch (const InvalidMessage& error) {
        std::cerr << "Discarding invalid completion: "
                  << error.what() << '\n';
        channel->BasicReject(envelope, false);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Discarding inconsistent completion: "
                  << error.what() << '\n';
        channel->BasicReject(envelope, false);
    }
}

} // namespace

int main() {
    try {
        const RabbitConfig config = rabbitConfig();
        const auto channel = connect(config);
        AdaptiveScheduler scheduler(0.2,
            envInt("ADAPTIVE_DISPATCH_WINDOW", 6, 1, 256));

        declareDurableQueue(channel, queue_names::incoming_tasks);
        declareDurableQueue(channel, queue_names::worker_calibration);
        declareDurableQueue(channel, queue_names::completed_tasks);
        for (int worker_id = 1;
             worker_id <= queue_names::worker_count;
             ++worker_id) {
            declareDurableQueue(channel, queue_names::worker_tasks(worker_id));
        }

        std::cout << "Dispatcher connected to " << config.host
                  << ':' << config.port << '\n';
        calibrate(channel, scheduler);
        std::cout << "All workers calibrated; total dispatch window="
                  << scheduler.dispatchWindow() << std::endl;

        // Independent connections let us keep receiving completions while
        // task consumption is paused because the dispatch window is full.
        const auto task_channel = connect(config);
        const std::string task_consumer_tag = task_channel->BasicConsume(
            queue_names::incoming_tasks, "", false, false, false, 1
        );
        const std::string completion_consumer_tag = channel->BasicConsume(
            queue_names::completed_tasks, "", false, false, false,
            queue_names::worker_count
        );

        while (true) {
            AmqpClient::Envelope::ptr_t completion;
            while (channel->BasicConsumeMessage(completion_consumer_tag, completion, 0)) {
                handleCompletion(channel, completion, scheduler);
            }
            if (!scheduler.hasCapacity()) {
                handleCompletion(channel,
                    channel->BasicConsumeMessage(completion_consumer_tag), scheduler);
                continue;
            }

            // Prefetch holds at most one unassigned task at the dispatcher;
            // the rest of the pool stays in the shared incoming queue.
            AmqpClient::Envelope::ptr_t envelope;
            if (!task_channel->BasicConsumeMessage(task_consumer_tag, envelope, 10)) {
                continue;
            }
            TaskDescriptor task;
            try {
                task = parseTask(envelope->Message()->Body());
            } catch (const InvalidMessage& error) {
                std::cerr << "Discarding invalid task: "
                          << error.what() << '\n';
                task_channel->BasicReject(envelope, false);
                continue;
            }

            const auto decision = scheduler.chooseWorker(task.matrix_size);
            const std::string target_queue =
                queue_names::worker_tasks(decision.workerId);
            task_channel->BasicPublish(
                "", target_queue, envelope->Message(), true
            );
            scheduler.onAssigned(
                task.id, task.matrix_size, decision.workerId
            );

            task_channel->BasicAck(envelope);
            std::cout << "Task " << task.id << " routed to "
                      << target_queue << "; predicted execution="
                      << decision.executionMs << " ms, waiting="
                      << decision.waitingMs << " ms, finish="
                      << decision.finishMs << " ms" << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "Dispatcher stopped: " << error.what() << '\n';
        return 1;
    }
}
