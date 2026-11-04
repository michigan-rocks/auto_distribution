#pragma once

#include "queue_names.hpp"

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace producer {

using Json = nlohmann::json;
using BenchmarkClock = std::chrono::steady_clock;

inline std::string setting(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

inline int integerSetting(const char* name, int fallback, int minimum, int maximum) {
    const auto value = setting(name, std::to_string(fallback));
    std::size_t parsed = 0;
    const int number = std::stoi(value, &parsed);
    if (parsed != value.size() || number < minimum || number > maximum) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return number;
}

inline std::uint64_t unsignedField(const Json& object, const char* name,
                                   std::uint64_t maximum) {
    const auto& value = object.at(name);
    if (!value.is_number_integer() || value < 0 || value > maximum) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return value.get<std::uint64_t>();
}

struct Workload {
    std::vector<Json> tasks;
    std::set<std::uint64_t> ids;
    std::string fingerprint;
};

inline Workload loadWorkload(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read workload: " + path);
    Workload workload;
    std::uint64_t hash = 14695981039346656037ULL;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        for (unsigned char byte : line + '\n') {
            hash = (hash ^ byte) * 1099511628211ULL;
        }
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            const auto task = Json::parse(line);
            const auto id = unsignedField(task, "task_id",
                                         std::numeric_limits<std::uint64_t>::max());
            const auto size = unsignedField(task, "matrix_size", 1024);
            const auto seed = unsignedField(task, "seed",
                                           std::numeric_limits<std::uint32_t>::max());
            if (size == 0 || !workload.ids.insert(id).second) {
                throw std::invalid_argument("zero matrix_size or duplicate task_id");
            }
            workload.tasks.push_back({{"task_id", id}, {"matrix_size", size},
                                      {"seed", seed}});
        } catch (const std::exception& error) {
            throw std::runtime_error(path + ":" + std::to_string(line_number) +
                                     ": " + error.what());
        }
    }
    if (input.bad()) throw std::runtime_error("Error reading workload: " + path);
    if (workload.tasks.empty()) throw std::runtime_error("Workload is empty: " + path);
    std::ostringstream fingerprint;
    fingerprint << std::hex << std::setfill('0') << std::setw(16) << hash;
    workload.fingerprint = fingerprint.str();
    return workload;
}

inline void waitUntilReady(const AmqpClient::Channel::ptr_t& channel,
                           const std::string& mode, int timeout_seconds) {
    std::vector<std::pair<std::string, unsigned>> expected;
    if (mode == "baseline") {
        expected.emplace_back(queue_names::shared_tasks, queue_names::worker_count);
    } else {
        expected.emplace_back(queue_names::incoming_tasks, 1);
        for (int id = 1; id <= queue_names::worker_count; ++id) {
            expected.emplace_back(queue_names::worker_tasks(id), 1);
        }
    }
    std::cout << "Waiting for workers and dispatcher to be ready..." << std::endl;
    const auto deadline = BenchmarkClock::now() + std::chrono::seconds(timeout_seconds);
    while (true) {
        bool ready = true;
        for (const auto& [queue, count] : expected) {
            boost::uint32_t messages = 0;
            boost::uint32_t consumers = 0;
            channel->DeclareQueueWithCounts(queue, messages, consumers,
                                            false, true, false, false);
            if (messages != 0 || consumers > count) {
                throw std::runtime_error("Queue " + queue +
                    " contains old tasks or extra consumers; use a fresh EXPERIMENT_ID");
            }
            ready = ready && consumers == count;
        }
        if (ready) return;
        if (BenchmarkClock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for workers/dispatcher readiness");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

inline double secondsSince(BenchmarkClock::time_point start) {
    return std::chrono::duration<double>(BenchmarkClock::now() - start).count();
}

inline void runWorkload(const AmqpClient::Channel::ptr_t& channel,
                        const std::string& target_queue, const std::string& mode,
                        const std::string& path) {
    const auto workload = loadWorkload(path);
    const int timeout_seconds = integerSetting("BENCHMARK_TIMEOUT_SECONDS", 3600, 1, 86400);
    const int ready_timeout = integerSetting("BENCHMARK_READY_TIMEOUT_SECONDS", 300, 1, 3600);
    const int interval_ms = integerSetting("PRODUCER_INTERVAL_MS", 0, 0, 60000);
    const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string experiment = setting("EXPERIMENT_ID", "");
    if (experiment.empty()) {
        experiment = "run-" + std::to_string(epoch_ms) + "-" + std::to_string(std::random_device{}());
    }
    if (experiment.empty() || experiment.size() > 100 ||
        experiment.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
            != std::string::npos) {
        throw std::invalid_argument("EXPERIMENT_ID must contain 1..100 letters, digits, '-' or '_'");
    }
    const auto result_path = std::filesystem::path(setting("BENCHMARK_RESULTS_DIR", "results")) /
                             (experiment + ".json");
    std::filesystem::create_directories(result_path.parent_path());
    if (std::filesystem::exists(result_path)) {
        throw std::runtime_error("Report already exists: " + result_path.string());
    }
    std::ofstream output(result_path);
    if (!output) throw std::runtime_error("Cannot write report: " + result_path.string());

    Json report = {{"experiment_id", experiment}, {"mode", setting("SCHEDULER_MODE", mode)},
                   {"workload", path}, {"workload_fingerprint_fnv1a64", workload.fingerprint},
                   {"expected", workload.tasks.size()}, {"completed", 0}, {"published", 0},
                   {"interval_ms", interval_ms}, {"status", "failed"}};
    if (report["mode"] == "adaptive") {
        report["scheduler"] = {
            {"policy", "fifo-remaining-work-window-v3"},
            {"max_in_flight_total",
             integerSetting("ADAPTIVE_DISPATCH_WINDOW", 6, 1, 256)}
        };
    }
    std::map<std::string, unsigned> workers{{"1", 0}, {"2", 0}, {"3", 0}};
    Json completions = Json::array();
    std::set<std::uint64_t> completed;
    auto start = BenchmarkClock::time_point{};
    auto save = [&] {
        report["completed"] = completed.size();
        report["workers"] = workers;
        report["tasks"] = completions;
        output << report.dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("Cannot save report: " + result_path.string());
    };
    try {
        waitUntilReady(channel, mode, ready_timeout);
        const std::string replies = channel->DeclareQueue("", false, false, true, true);
        const std::string consumer = channel->BasicConsume(replies, "", false, true, true);
        std::size_t published = 0;
        start = BenchmarkClock::now();
        report["started_at_unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto next_publish = start;
        const auto deadline = start + std::chrono::seconds(timeout_seconds);
        std::cout << "Experiment " << experiment << ": " << workload.tasks.size()
                  << " tasks, fingerprint=" << workload.fingerprint << std::endl;
        while (completed.size() < workload.tasks.size()) {
            if (BenchmarkClock::now() >= deadline) {
                throw std::runtime_error("Benchmark timed out before all tasks completed");
            }
            if (published < workload.tasks.size() && BenchmarkClock::now() >= next_publish) {
                auto task = workload.tasks[published];
                task["experiment_id"] = experiment;
                auto message = AmqpClient::BasicMessage::Create(task.dump());
                message->ContentType("application/json");
                message->DeliveryMode(AmqpClient::BasicMessage::dm_persistent);
                message->ReplyTo(replies);
                channel->BasicPublish("", target_queue, message, true);
                ++published;
                report["published"] = published;
                next_publish += std::chrono::milliseconds(interval_ms);
                if (published == workload.tasks.size()) {
                    report["publish_seconds"] = secondsSince(start);
                    std::cout << "All " << published << " tasks published" << std::endl;
                }
            }
            AmqpClient::Envelope::ptr_t envelope;
            // Drain replies while publishing too, so completion observation
            // is not delayed until the entire pool has been sent.
            const int wait_ms = published == workload.tasks.size() ? 100 : 0;
            if (channel->BasicConsumeMessage(consumer, envelope, wait_ms)) {
                const double received_after = secondsSince(start);
                auto event = Json::parse(envelope->Message()->Body());
                const auto id = unsignedField(event, "task_id",
                                               std::numeric_limits<std::uint64_t>::max());
                const auto worker_id = unsignedField(event, "worker_id", queue_names::worker_count);
                if (event.at("experiment_id") != experiment ||
                    event.at("type") != "completed" || !workload.ids.count(id) || worker_id == 0) {
                    throw std::runtime_error("Unexpected completion received for this experiment");
                }
                if (completed.insert(id).second) {
                    ++workers.at(std::to_string(worker_id));
                    event["received_after_seconds"] = received_after;
                    completions.push_back(std::move(event));
                    if (completed.size() % 100 == 0) {
                        std::cout << "Completed " << completed.size() << '/'
                                  << workload.tasks.size() << std::endl;
                    }
                }
            } else if (published < workload.tasks.size() && interval_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        report["elapsed_seconds"] = secondsSince(start);
        report["tasks_per_second"] = workload.tasks.size() / report["elapsed_seconds"].get<double>();
        report["status"] = "completed";
        save();
        std::cout << "BENCHMARK COMPLETE: " << completed.size() << " tasks in "
                  << report["elapsed_seconds"] << " seconds; workers=" << Json(workers).dump()
                  << "; report=" << result_path.string() << std::endl;
    } catch (const std::exception& error) {
        report["status"] = "failed";
        report["error"] = error.what();
        if (start != BenchmarkClock::time_point{}) report["elapsed_seconds"] = secondsSince(start);
        save();
        throw;
    }
}

} // namespace producer
