#pragma once

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <vector>

class AdaptiveScheduler {
public:
    using Clock = std::chrono::steady_clock;

    struct Decision {
        int workerId;       // Выбранный Worker
        double executionMs; // Прогноз времени выполнения
        double finishMs;    // Прогноз с учётом очереди
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

        if (!std::isfinite(alpha) ||
            alpha <= 0.0 || alpha > 1.0) {
            throw std::invalid_argument(
                "alpha must be in (0, 1]"
            );
        }
        if (dispatchWindow < 1 || dispatchWindow > 256) {
            throw std::invalid_argument("dispatchWindow must be in [1, 256]");
        }
    }

    // Выбираем Worker с минимальным временем завершения
    Decision chooseWorker(int matrixSize) const {
        const double factor = sizeFactor(matrixSize);
        if (!hasCapacity()) {
            throw std::logic_error("dispatch window is full or no calibrated worker is available");
        }

        Decision best{0, 0.0, 0.0, 0.0};
        bool first = true;
        const auto now = Clock::now();

        for (const auto& worker : workers_) {
            if (!worker.calibrated) {
                continue;
            }
            const double execution = worker.msAt400 * factor;
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

    // Получаем статистику всех Worker
    std::vector<WorkerStatus> workerStatus() const {
        const auto now = Clock::now();
        std::vector<WorkerStatus> status;
        for (const auto& worker : workers_) {
            status.push_back({worker.id, worker.msAt400, remainingWork(worker, now),
                              static_cast<int>(worker.queue.size())});
        }
        return status;
    }
 
    // Регистрируем задачу после успешной публикации
    void onAssigned(
        std::uint64_t taskId,
        int matrixSize,
        int workerId
    ) {
        if (workerId < 1 || workerId > 3) {
            throw std::invalid_argument(
                "invalid workerId"
            );
        }

        if (pending_.count(taskId) != 0) {
            throw std::invalid_argument(
                "duplicate taskId"
            );
        }

        auto& w = workers_[workerId - 1];
        if (!w.calibrated || !hasCapacity()) {
            throw std::logic_error("dispatch window is full or worker is not calibrated");
        }

        sizeFactor(matrixSize);
        if (w.queue.empty()) w.headStartedAt = Clock::now();
        w.queue.push_back({taskId, matrixSize});
        pending_.emplace(taskId, workerId);
    }

    // Worker завершил задачу: обновляем оценку
    bool onCompleted(
        std::uint64_t taskId,
        double elapsedMs
    ) {
        if (!std::isfinite(elapsedMs) ||
            elapsedMs <= 0.0) {
            throw std::invalid_argument(
                "invalid elapsedMs"
            );
        }

        auto it = pending_.find(taskId);

        if (it == pending_.end()) {
            return false;
        }

        auto& w = workers_[it->second - 1];
        if (w.queue.empty() || w.queue.front().id != taskId) {
            throw std::invalid_argument("completion is out of worker FIFO order");
        }
        const auto task = w.queue.front();

        // Приводим измерение к матрице 400x400
        double observedAt400 =
            elapsedMs / sizeFactor(task.matrixSize);

        // EWMA: обновляем оценку скорости Worker
        w.msAt400 =
            (1.0 - alpha_) * w.msAt400 +
            alpha_ * observedAt400;

        w.queue.pop_front();
        // With prefetch=1 the worker starts its next assigned task after
        // finishing this one. Receipt time is our local start approximation.
        w.headStartedAt = Clock::now();
        pending_.erase(it);

        return true;
    }

    bool onCompleted(
        std::uint64_t taskId,
        int workerId,
        double elapsedMs
    ) {
        auto it = pending_.find(taskId);
        if (it == pending_.end()) {
            return false;
        }
        if (it->second != workerId) {
            throw std::invalid_argument(
                "completion worker does not match assignment"
            );
        }
        return onCompleted(taskId, elapsedMs);
    }

    void calibrateWorker(int workerId, double measuredMs) {
        if (workerId < 1 || workerId > 3 ||
            !std::isfinite(measuredMs) || measuredMs <= 0.0) {
            throw std::invalid_argument("Invalid calibration");
        }

        auto& worker = workers_[workerId - 1];

        worker.msAt400 = measuredMs;
        worker.calibrated = true;
    }

private:
    struct Pending {
        std::uint64_t id;
        int matrixSize;
    };

    struct Worker {
        int id;
        double msAt400 = 0.0;
        bool calibrated = false;
        std::deque<Pending> queue;
        Clock::time_point headStartedAt{};
    };

    static double remainingWork(const Worker& worker, Clock::time_point now) {
        if (worker.queue.empty()) return 0.0;
        const double elapsed = std::chrono::duration<double, std::milli>(
            now - worker.headStartedAt).count();
        double remaining = std::max(0.0, worker.msAt400 *
            sizeFactor(worker.queue.front().matrixSize) - elapsed);
        // Queued tasks have not started. Reprice them from the current
        // speed estimate on every decision rather than keeping old costs.
        for (std::size_t index = 1; index < worker.queue.size(); ++index) {
            remaining += worker.msAt400 * sizeFactor(worker.queue[index].matrixSize);
        }
        return remaining;
    }

    // Для классического умножения матриц O(n^3)
    static double sizeFactor(int n) {
        if (n <= 0) {
            throw std::invalid_argument(
                "matrixSize must be positive"
            );
        }

        double x = static_cast<double>(n) / 400.0;
        return x * x * x;
    }

    // Временные характеристики для тестирования
    std::vector<Worker> workers_{Worker{1}, Worker{2}, Worker{3}};

    // Задачи, назначенные Worker, но ещё не завершённые
    std::unordered_map<std::uint64_t, int> pending_;

    
    double alpha_;
    int dispatch_window_;
};
