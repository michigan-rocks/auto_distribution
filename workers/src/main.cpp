#include "matrix.hpp"
#include "completion_metrics.hpp"
#include "queue_names.hpp"

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <nlohmann/json.hpp>
#include <prometheus/exposer.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

struct BadTask : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Task {
    std::uint64_t id;
    int size;
    worker::Matrix a;
    worker::Matrix b;
    std::string experiment_id;
};

struct WorkResult {
    worker::Matrix matrix;
    double compute_ms;
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

bool envBool(const char* name, bool fallback) {
    const std::string value = env(name, fallback ? "true" : "false");
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    throw std::invalid_argument(std::string("invalid ") + name);
}

Task parseTask(const std::string& body) {
    try {
        const Json data = Json::parse(body);
        if (!data.is_object() || !data.contains("task_id") ||
            !data.at("task_id").is_number_integer()) {
            throw BadTask("missing or invalid task_id");
        }
        std::uint64_t id;
        if (data.at("task_id").is_number_unsigned()) {
            id = data.at("task_id").get<std::uint64_t>();
        } else {
            const auto signed_id = data.at("task_id").get<std::int64_t>();
            if (signed_id < 0) throw BadTask("negative task_id");
            id = static_cast<std::uint64_t>(signed_id);
        }
        const int size = data.at("matrix_size").get<int>();
        // Current producer generates 100..500. A modest upper bound prevents
        // accidental resource exhaustion from malformed messages.
        if (size < 1 || size > 1024) throw BadTask("unsupported matrix_size");
        const auto experiment_id = data.value("experiment_id", std::string{});
        if (data.contains("seed")) {
            const auto& seed = data.at("seed");
            if (!seed.is_number_integer() || seed < 0 ||
                seed > std::numeric_limits<std::uint32_t>::max()) {
                throw BadTask("seed must be an unsigned 32-bit integer");
            }
            auto inputs = worker::generateInputs(size, seed.get<std::uint32_t>());
            return Task{id, size, std::move(inputs.first),
                        std::move(inputs.second), experiment_id};
        }
        auto a = data.at("matrix_a").get<worker::Matrix>();
        auto b = data.at("matrix_b").get<worker::Matrix>();
        if (a.size() != static_cast<std::size_t>(size) ||
            b.size() != static_cast<std::size_t>(size)) {
            throw BadTask("matrix_size does not match matrix dimensions");
        }
        for (int i = 0; i < size; ++i) {
            if (a[i].size() != static_cast<std::size_t>(size) ||
                b[i].size() != static_cast<std::size_t>(size)) {
                throw BadTask("matrices must be square");
            }
            for (int j = 0; j < size; ++j) {
                // Ensures the output elements and checksum fit int64_t.
                if (std::abs(static_cast<double>(a[i][j])) > 1000.0 ||
                    std::abs(static_cast<double>(b[i][j])) > 1000.0) {
                    throw BadTask("matrix element out of supported range");
                }
            }
        }
        return Task{id, size, std::move(a), std::move(b), experiment_id};
    } catch (const Json::exception& e) {
        throw BadTask(std::string("invalid JSON task: ") + e.what());
    }
}

double msBetween(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

WorkResult execute(const worker::Matrix& a, const worker::Matrix& b) {
    const auto start = Clock::now();
    auto result = worker::multiply(a, b);
    const double elapsed = msBetween(start, Clock::now());
    // Wall time includes time spent throttled by the container's CPU quota.
    return WorkResult{std::move(result), elapsed, elapsed};
}

double calibrate() {
    constexpr int n = 400;
    std::mt19937 random(2026);
    std::uniform_int_distribution<int> distribution(0, 9);
    worker::Matrix a(n, std::vector<std::int64_t>(n));
    worker::Matrix b(n, std::vector<std::int64_t>(n));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            a[i][j] = distribution(random);
            b[i][j] = distribution(random);
        }
    }
    double total_ms = 0;
    std::int64_t guard = 0;
    int repeats = 0;
    const auto start = Clock::now();
    // Span many CPU quota periods so the initial available CPU budget
    // does not make a throttled worker look faster than it is.
    do {
        auto result = execute(a, b);
        total_ms += result.elapsed_ms;
        guard ^= worker::checksum(result.matrix);
        ++repeats;
    } while (repeats < 3 || Clock::now() - start < std::chrono::seconds(2));
    std::cout << "Calibration samples=" << repeats << " guard=" << guard << '\n';
    return total_ms / repeats;
}

void declareDurableQueue(const AmqpClient::Channel::ptr_t& channel,
                         const std::string& name) {
    channel->DeclareQueue(name, false, true, false, false);
}

void publishJson(const AmqpClient::Channel::ptr_t& channel,
                 const std::string& queue, const Json& value) {
    auto message = AmqpClient::BasicMessage::Create(value.dump());
    message->ContentType("application/json");
    message->DeliveryMode(AmqpClient::BasicMessage::dm_persistent);
    channel->BasicPublish("", queue, message, true);
}

} // namespace

int main() {
    try {
        const int worker_id = envInt("WORKER_ID", 1, 1, 3);
        const int port = envInt("RABBITMQ_PORT", 5672, 1, 65535);
        const int metrics_port = envInt("METRICS_PORT", 9100, 1, 65535);
        const bool send_full_result = envBool("WORKER_SEND_FULL_RESULT", true);
        const std::string mode = env("WORKER_MODE", "adaptive");
        if (mode != "adaptive" && mode != "baseline") {
            throw std::invalid_argument("WORKER_MODE must be adaptive or baseline");
        }

        const std::string input_queue = mode == "baseline"
            ? queue_names::shared_tasks
            : queue_names::worker_tasks(worker_id);
        const std::string host = env("RABBITMQ_HOST", "localhost");
        const std::string user = env("RABBITMQ_USER", "user");
        const std::string password = env("RABBITMQ_PASSWORD", "user");

        CompletionMetrics metrics(worker_id);
        prometheus::Exposer metrics_server(
            "0.0.0.0:" + std::to_string(metrics_port)
        );
        metrics_server.RegisterCollectable(metrics.registry());

        std::cout << "Worker " << worker_id << " connecting to " << host
                  << ':' << port << " (queue=" << input_queue << ")\n";
        auto channel = AmqpClient::Channel::Create(host, port, user, password);

        declareDurableQueue(channel, input_queue);
        declareDurableQueue(channel, queue_names::worker_calibration);
        declareDurableQueue(channel, queue_names::completed_tasks);
        declareDurableQueue(channel, queue_names::task_results);

        const double average_ms = calibrate();
        if (mode == "adaptive") {
            publishJson(channel, queue_names::worker_calibration, Json{
                {"type", "calibration"}, {"worker_id", worker_id},
                {"matrix_size", 400}, {"average_ms", average_ms}
            });
            std::cout << "Calibration sent: " << average_ms << " ms\n";
        }

        // Manual acknowledgements + prefetch=1: one unfinished task at a time.
        const std::string consumer_tag = channel->BasicConsume(
            input_queue, "", false, false, false, 1);
        while (true) {
            const auto envelope = channel->BasicConsumeMessage(consumer_tag);
            Task task;
            try {
                task = parseTask(envelope->Message()->Body());
            } catch (const BadTask& e) {
                std::cerr << "Discarding malformed task: " << e.what() << '\n';
                channel->BasicReject(envelope, false);
                continue;
            }

            // Publish first, acknowledge last. Transient errors propagate and
            // the broker will redeliver this unacknowledged task on disconnect.
            WorkResult result = execute(task.a, task.b);
            const auto sum = worker::checksum(result.matrix);
            Json result_event = {
                {"type", "result"}, {"task_id", task.id},
                {"worker_id", worker_id}, {"matrix_size", task.size},
                {"experiment_id", task.experiment_id},
                {"checksum", sum}
            };
            if (send_full_result) {
                result_event["matrix_result"] = std::move(result.matrix);
            }
            publishJson(channel, queue_names::task_results, result_event);
            const Json completion_event = {
                {"type", "completed"}, {"task_id", task.id},
                {"worker_id", worker_id}, {"matrix_size", task.size},
                {"experiment_id", task.experiment_id},
                {"elapsed_ms", result.elapsed_ms},
                {"compute_ms", result.compute_ms}, {"checksum", sum}
            };
            publishJson(channel, queue_names::completed_tasks, completion_event);
            channel->BasicAck(envelope);
            metrics.recordCompletion();
            if (envelope->Message()->ReplyToIsSet()) {
                // The temporary reply queue belongs to this producer run.
                // A cancelled run must not stop a worker or requeue its task.
                auto reply = AmqpClient::BasicMessage::Create(completion_event.dump());
                reply->ContentType("application/json");
                channel->BasicPublish("", envelope->Message()->ReplyTo(), reply, false);
            }
            std::cout << "Completed task=" << task.id
                      << " size=" << task.size
                      << " elapsed_ms=" << result.elapsed_ms
                      << " checksum=" << sum << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Worker stopped: " << e.what() << '\n';
        return 1;
    }
}
