#include "matrix.hpp"

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
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

double envNonnegativeDouble(const char* name, double fallback) {
    const std::string value = env(name, std::to_string(fallback));
    std::size_t parsed = 0;
    const double number = std::stod(value, &parsed);
    if (parsed != value.size() || !std::isfinite(number) || number < 0.0) {
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
        return Task{id, size, std::move(a), std::move(b)};
    } catch (const Json::exception& e) {
        throw BadTask(std::string("invalid JSON task: ") + e.what());
    }
}

double msBetween(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

WorkResult execute(const worker::Matrix& a, const worker::Matrix& b,
                   double delay_ms_at_400) {
    const auto start = Clock::now();
    auto result = worker::multiply(a, b);
    const auto after_compute = Clock::now();

    const double n = static_cast<double>(a.size());
    const double factor = (n / 400.0) * (n / 400.0) * (n / 400.0);
    const double artificial_delay = delay_ms_at_400 * factor;
    if (artificial_delay > 0) {
        std::this_thread::sleep_for(
            std::chrono::duration<double, std::milli>(artificial_delay));
    }
    const auto done = Clock::now();
    return WorkResult{std::move(result), msBetween(start, after_compute),
                      msBetween(start, done)};
}

double calibrate(double delay_ms_at_400) {
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
    constexpr int repeats = 3;
    for (int i = 0; i < repeats; ++i) {
        auto result = execute(a, b, delay_ms_at_400);
        total_ms += result.elapsed_ms;
        guard ^= worker::checksum(result.matrix);
    }
    std::cout << "Calibration guard=" << guard << '\n';
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
        const double delay_ms_at_400 =
            envNonnegativeDouble("SIMULATED_DELAY_MS_AT_400", 0.0);
        const bool send_full_result = envBool("WORKER_SEND_FULL_RESULT", true);
        const std::string mode = env("WORKER_MODE", "adaptive");
        if (mode != "adaptive" && mode != "baseline") {
            throw std::invalid_argument("WORKER_MODE must be adaptive or baseline");
        }

        const std::string input_queue =
            mode == "baseline" ? "tasks.shared" :
            "worker." + std::to_string(worker_id);
        const std::string host = env("RABBITMQ_HOST", "localhost");
        const std::string user = env("RABBITMQ_USER", "user");
        const std::string password = env("RABBITMQ_PASSWORD", "user");

        std::cout << "Worker " << worker_id << " connecting to " << host
                  << ':' << port << " (queue=" << input_queue << ")\n";
        auto channel = AmqpClient::Channel::Create(host, port, user, password);

        declareDurableQueue(channel, input_queue);
        declareDurableQueue(channel, "workers.calibration");
        declareDurableQueue(channel, "tasks.completed");
        declareDurableQueue(channel, "tasks.results");

        const double average_ms = calibrate(delay_ms_at_400);
        publishJson(channel, "workers.calibration", Json{
            {"type", "calibration"}, {"worker_id", worker_id},
            {"matrix_size", 400}, {"average_ms", average_ms}
        });
        std::cout << "Calibration sent: " << average_ms << " ms\n";

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
            WorkResult result = execute(task.a, task.b, delay_ms_at_400);
            const auto sum = worker::checksum(result.matrix);
            Json result_event = {
                {"type", "result"}, {"task_id", task.id},
                {"worker_id", worker_id}, {"matrix_size", task.size},
                {"checksum", sum}
            };
            if (send_full_result) {
                result_event["matrix_result"] = std::move(result.matrix);
            }
            publishJson(channel, "tasks.results", result_event);
            publishJson(channel, "tasks.completed", Json{
                {"type", "completed"}, {"task_id", task.id},
                {"worker_id", worker_id}, {"matrix_size", task.size},
                {"elapsed_ms", result.elapsed_ms},
                {"compute_ms", result.compute_ms}, {"checksum", sum}
            });
            channel->BasicAck(envelope);
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
