#include "AdaptiveScheduler.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {

bool closeTo(double actual, double expected, double tolerance = 1e-6) {
    return std::abs(actual - expected) <= tolerance;
}

void predictsCubicQuadraticAndConstantWork() {
    AdaptiveScheduler scheduler;
    scheduler.calibrateWorker(1, 40.0, 0.0, 20.0);
    scheduler.calibrateWorker(2, 80.0, 0.0, 0.0);
    scheduler.calibrateWorker(3, 160.0, 0.0, 0.0);

    // Constant overhead dominates a very small task, so worker 2 wins.
    const auto small = scheduler.chooseWorker(100);
    assert(small.workerId == 2);
    assert(closeTo(small.executionMs, 1.25));

    // Cubic work dominates at 400x400, so worker 1 wins.
    const auto large = scheduler.chooseWorker(400);
    assert(large.workerId == 1);
    assert(closeTo(large.executionMs, 60.0));
}

void updatesCompleteModelWithEwma() {
    AdaptiveScheduler scheduler(0.5, 2);
    scheduler.calibrateWorker(1, 70.0, 20.0, 10.0);

    assert(closeTo(scheduler.chooseWorker(400).executionMs, 100.0));
    scheduler.onAssigned(1, 400, 1);
    assert(scheduler.onCompleted(1, 1, 200.0));
    assert(closeTo(scheduler.chooseWorker(400).executionMs, 150.0));
}

void rejectsInvalidCalibration() {
    AdaptiveScheduler scheduler;
    bool rejected = false;
    try {
        scheduler.calibrateWorker(1, 0.0, 0.0, 0.0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

} // namespace

int main() {
    predictsCubicQuadraticAndConstantWork();
    updatesCompleteModelWithEwma();
    rejectsInvalidCalibration();
}
