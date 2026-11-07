#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <vector>

class AdaptiveScheduler {
public:
    using Clock = std::chrono::steady_clock;

    struct Decision {
        int workerId;
        double executionMs;
        double finishMs;
        double waitingMs;
    };

    struct WorkerStatus {
        int id;
        double msAt400;
        double pendingMs;
        int pendingTasks;
    };

    AdaptiveScheduler(double alpha = 0.2, int dispatchWindow = 6)
        : alpha_(alpha), dispatch_window_(dispatchWindow) {
        if (!std::isfinite(alpha) || alpha <= 0.0 || alpha > 1.0) {
            throw std::invalid_argument("alpha must be in (0, 1]");
        }
        if (dispatchWindow < 1 || dispatchWindow > 256) {
            throw std::invalid_argument("dispatchWindow must be in [1, 256]");
        }
    }

    Decision chooseWorker(int matrixSize) const {
        validateMatrixSize(matrixSize);
        if (!hasCapacity()) {
            throw std::logic_error(
                "dispatch window is full or no calibrated worker is available");
        }

        Decision best{0, 0.0, 0.0, 0.0};
        bool first = true;
        const auto now = Clock::now();
        for (const auto& worker : workers_) {
            if (!worker.calibrated) continue;

            const double execution = predictedServiceMs(worker, matrixSize);
            const double waiting = remainingWork(worker, now);
            const double finish = waiting + execution;
            if (first || finish < best.finishMs) {
                best = {worker.id, execution, finish, waiting};
                first = false;
            }
        }

        if (first) {
            throw std::logic_error("no calibrated worker is available");
        }
        return best;
    }

    bool hasCapacity() const {
        return pending_.size() < static_cast<std::size_t>(dispatch_window_) &&
            std::any_of(workers_.begin(), workers_.end(), [](const Worker& worker) {
                return worker.calibrated;
            });
    }

    int dispatchWindow() const {
        return dispatch_window_;
    }

    std::vector<WorkerStatus> workerStatus() const {
        const auto now = Clock::now();
        std::vector<WorkerStatus> status;
        for (const auto& worker : workers_) {
            status.push_back({worker.id, predictedServiceMs(worker, 400),
                              remainingWork(worker, now),
                              static_cast<int>(worker.queue.size())});
        }
        return status;
    }

    void onAssigned(std::uint64_t taskId, int matrixSize, int workerId) {
        if (workerId < 1 || workerId > 3) {
            throw std::invalid_argument("invalid workerId");
        }
        if (pending_.count(taskId) != 0) {
            throw std::invalid_argument("duplicate taskId");
        }

        auto& worker = workers_[workerId - 1];
        if (!worker.calibrated || !hasCapacity()) {
            throw std::logic_error(
                "dispatch window is full or worker is not calibrated");
        }

        validateMatrixSize(matrixSize);
        if (worker.queue.empty()) worker.headStartedAt = Clock::now();
        worker.queue.push_back({taskId, matrixSize});
        pending_.emplace(taskId, workerId);
    }

    bool onCompleted(std::uint64_t taskId, double serviceMs) {
        if (!std::isfinite(serviceMs) || serviceMs <= 0.0) {
            throw std::invalid_argument("invalid serviceMs");
        }

        auto it = pending_.find(taskId);
        if (it == pending_.end()) return false;

        auto& worker = workers_[it->second - 1];
        if (worker.queue.empty() || worker.queue.front().id != taskId) {
            throw std::invalid_argument("completion is out of worker FIFO order");
        }
        const auto task = worker.queue.front();

        // EWMA adjusts the complete calibrated model, not just its cubic term.
        const double predicted = predictedServiceMs(worker, task.matrixSize);
        const double observedRatio = serviceMs / predicted;
        worker.correctionScale *=
            (1.0 - alpha_) + alpha_ * observedRatio;

        worker.queue.pop_front();
        // With prefetch=1, receipt of this completion is the best local
        // approximation for the start of the next task in this worker queue.
        worker.headStartedAt = Clock::now();
        pending_.erase(it);
        return true;
    }

    bool onCompleted(std::uint64_t taskId, int workerId, double serviceMs) {
        auto it = pending_.find(taskId);
        if (it == pending_.end()) return false;
        if (it->second != workerId) {
            throw std::invalid_argument(
                "completion worker does not match assignment");
        }
        return onCompleted(taskId, serviceMs);
    }

    // Compatibility overload for callers that only have a cubic estimate.
    void calibrateWorker(int workerId, double measuredMs) {
        calibrateWorker(workerId, measuredMs, 0.0, 0.0);
    }

    void calibrateWorker(int workerId, double cubicMsAt400,
                         double quadraticMsAt400, double constantMs) {
        if (workerId < 1 || workerId > 3 ||
            !validCoefficient(cubicMsAt400) ||
            !validCoefficient(quadraticMsAt400) ||
            !validCoefficient(constantMs) ||
            cubicMsAt400 + quadraticMsAt400 + constantMs <= 0.0) {
            throw std::invalid_argument("Invalid calibration");
        }

        auto& worker = workers_[workerId - 1];
        worker.cubicMsAt400 = cubicMsAt400;
        worker.quadraticMsAt400 = quadraticMsAt400;
        worker.constantMs = constantMs;
        worker.correctionScale = 1.0;
        worker.calibrated = true;
    }

private:
    struct Pending {
        std::uint64_t id;
        int matrixSize;
    };

    struct Worker {
        explicit Worker(int workerId) : id(workerId) {}

        int id;
        double cubicMsAt400 = 0.0;
        double quadraticMsAt400 = 0.0;
        double constantMs = 0.0;
        double correctionScale = 1.0;
        bool calibrated = false;
        std::deque<Pending> queue;
        Clock::time_point headStartedAt{};
    };

    static double remainingWork(const Worker& worker, Clock::time_point now) {
        if (worker.queue.empty()) return 0.0;

        const double elapsed = std::chrono::duration<double, std::milli>(
            now - worker.headStartedAt).count();
        double remaining = std::max(
            0.0,
            predictedServiceMs(worker, worker.queue.front().matrixSize) - elapsed
        );
        for (std::size_t index = 1; index < worker.queue.size(); ++index) {
            remaining += predictedServiceMs(
                worker, worker.queue[index].matrixSize);
        }
        return remaining;
    }

    static void validateMatrixSize(int n) {
        if (n <= 0) {
            throw std::invalid_argument("matrixSize must be positive");
        }
    }

    static bool validCoefficient(double value) {
        return std::isfinite(value) && value >= 0.0;
    }

    static double predictedServiceMs(const Worker& worker, int n) {
        validateMatrixSize(n);
        const double x = static_cast<double>(n) / 400.0;
        return worker.correctionScale * (
            worker.cubicMsAt400 * x * x * x +
            worker.quadraticMsAt400 * x * x +
            worker.constantMs
        );
    }

    std::vector<Worker> workers_{Worker{1}, Worker{2}, Worker{3}};
    std::unordered_map<std::uint64_t, int> pending_;
    double alpha_;
    int dispatch_window_;
};
