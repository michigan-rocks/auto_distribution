#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

class AdaptiveScheduler {
public:
    struct Decision {
        int workerId;       // Выбранный Worker
        double executionMs; // Прогноз времени выполнения
        double finishMs;    // Прогноз с учётом очереди
    };

    struct WorkerStatus {
        int id;
        double msAt400;
        double pendingMs;
    };

    AdaptiveScheduler(double alpha = 0.2)
        : alpha_(alpha) {

        if (!std::isfinite(alpha) ||
            alpha <= 0.0 || alpha > 1.0) {
            throw std::invalid_argument(
                "alpha must be in (0, 1]"
            );
        }
    }

    // Выбираем Worker с минимальным временем завершения
    Decision chooseWorker(int matrixSize) const {
        const double factor = sizeFactor(matrixSize);

        Decision best{0, 0.0, 0.0};
        bool first = true;

        for (const auto& worker : workers_) {
            const double execution = worker.msAt400 * factor;
            const double finish = worker.pendingMs + execution;

            if (first || finish < best.finishMs) {
                best = {worker.id, execution, finish};
                first = false;
            }
        }

        return best;
    }

    // Получаем статистику всех Worker
    std::vector<WorkerStatus> workerStatus() const {
        return {
            {workers_[0].id, workers_[0].msAt400,
             workers_[0].pendingMs},
            {workers_[1].id, workers_[1].msAt400,
             workers_[1].pendingMs},
            {workers_[2].id, workers_[2].msAt400,
             workers_[2].pendingMs}
        };
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

        double predicted =
            w.msAt400 * sizeFactor(matrixSize);

        pending_.emplace(
            taskId,
            Pending{workerId, matrixSize, predicted}
        );

        w.pendingMs += predicted;
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

        Pending task = it->second;

        auto& w = workers_[task.workerId - 1];

        // Удаляем завершённую работу из нагрузки
        w.pendingMs = std::max(
            0.0,
            w.pendingMs - task.predictedMs
        );

        // Приводим измерение к матрице 400x400
        double observedAt400 =
            elapsedMs / sizeFactor(task.matrixSize);

        // EWMA: обновляем оценку скорости Worker
        w.msAt400 =
            (1.0 - alpha_) * w.msAt400 +
            alpha_ * observedAt400;

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
        if (it->second.workerId != workerId) {
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
    struct Worker {
        int id;
        double msAt400;
        double pendingMs;
        bool calibrated;
    };

    struct Pending {
        int workerId;
        int matrixSize;
        double predictedMs;
    };

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
    std::vector<Worker> workers_{{
        {1, 0.0, 0.0, false},
        {2, 0.0, 0.0, false},
        {3, 0.0, 0.0, false}
    }};

    // Задачи, назначенные Worker, но ещё не завершённые
    std::unordered_map<std::uint64_t, Pending> pending_;

    
    double alpha_;
};
