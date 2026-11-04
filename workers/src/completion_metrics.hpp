#pragma once

#include <prometheus/counter.h>
#include <prometheus/registry.h>

#include <memory>
#include <string>

class CompletionMetrics {
public:
    explicit CompletionMetrics(int worker_id)
        : registry_(std::make_shared<prometheus::Registry>()),
          completed_(prometheus::BuildCounter()
                         .Name("optotech_worker_tasks_completed_total")
                         .Help("Tasks completed and acknowledged by this worker")
                         .Register(*registry_)
                         .Add({{"worker_id", std::to_string(worker_id)}})) {}

    void recordCompletion() {
        completed_.Increment();
    }

    std::shared_ptr<prometheus::Registry> registry() const {
        return registry_;
    }

private:
    std::shared_ptr<prometheus::Registry> registry_;
    prometheus::Counter& completed_;
};
