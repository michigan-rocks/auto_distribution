#include "AdaptiveScheduler.h"
#include "queue_names.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {

bool near(double left, double right) {
    return std::abs(left - right) < 1e-9;
}

} // namespace

int main() {
    assert(queue_names::worker_tasks(1) == "worker.1");
    assert(queue_names::worker_tasks(3) == "worker.3");
    bool invalid_queue_worker_rejected = false;
    try {
        (void)queue_names::worker_tasks(4);
    } catch (const std::invalid_argument&) {
        invalid_queue_worker_rejected = true;
    }
    assert(invalid_queue_worker_rejected);

    AdaptiveScheduler scheduler;
    scheduler.calibrateWorker(1, 100.0);
    scheduler.calibrateWorker(2, 200.0);
    scheduler.calibrateWorker(3, 300.0);

    const auto initial = scheduler.chooseWorker(400);
    assert(initial.workerId == 1);
    assert(near(initial.executionMs, 100.0));
    assert(near(initial.finishMs, 100.0));

    scheduler.onAssigned(10, 400, 1);
    scheduler.onAssigned(11, 400, 1);
    const auto loaded = scheduler.chooseWorker(400);
    assert(loaded.workerId == 2);
    assert(near(loaded.finishMs, 200.0));

    bool mismatched_worker_rejected = false;
    try {
        scheduler.onCompleted(10, 2, 150.0);
    } catch (const std::invalid_argument&) {
        mismatched_worker_rejected = true;
    }
    assert(mismatched_worker_rejected);

    assert(scheduler.onCompleted(10, 1, 150.0));
    assert(!scheduler.onCompleted(999, 1, 150.0));

    const auto status = scheduler.workerStatus();
    assert(status.size() == 3);
    assert(near(status[0].msAt400, 110.0));
    assert(near(status[0].pendingMs, 100.0));

    const auto half_size = scheduler.chooseWorker(200);
    assert(half_size.workerId == 2);
    assert(near(half_size.executionMs, 25.0));
}
