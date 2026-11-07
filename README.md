# Adaptive Task Scheduler

An experimental distributed task-processing system built with **C++17** and **RabbitMQ**. The project explores whether an adaptive scheduler can improve task completion times compared with conventional message distribution strategies.

This project is being developed as part of a research paper for the **Optotech 2026** conference (Industrial Programming section).

> **Status:** Work in progress. The producer, workers, adaptive dispatcher, and separate Round Robin dispatcher are implemented. Worker completion counters are exported to Prometheus; the remaining metrics and strategy comparison are still in development.

## Research Goal

A shared queue with RabbitMQ's `prefetch = 1` already provides effective load distribution: workers request new tasks as they finish existing ones. However, it does not predict how long a particular task will take on a particular worker.

Our hypothesis is that **adaptive scheduling based on estimated task cost, worker performance, and current load** may reduce end-to-end latency under heterogeneous workloads. We will test this hypothesis rather than assume the adaptive approach is always better.

## Architecture

The proposed system has three types of C++ applications:

- **Producer:** replays a fixed pool of matrix tasks and records the time to complete the entire pool.
- **Dispatcher:** consumes incoming tasks, selects a worker using a scheduling strategy, and routes tasks through RabbitMQ.
- **Workers:** execute actual matrix multiplications and report completion times and other measurements.

RabbitMQ handles message transport via **AMQP 0-9-1**. Prometheus and Grafana provide observability.

### Adaptive mode

```text
Producer (C++)
      |
      v
RabbitMQ: tasks.incoming
      |
      v
Dispatcher (C++)
      |
      v
RabbitMQ: worker-specific queues
      |
      +----> Worker 1 (C++)
      +----> Worker 2 (C++)
      +----> Worker 3 (C++)
                 |
                 v
        Completion events / metrics
```

### Baseline mode

In the RabbitMQ baseline, the same worker implementations consume from **one shared RabbitMQ queue** using `prefetch = 1`, with no dispatcher in the task path.

## Scheduling Strategies

The experimental comparison is planned to include:

1. **RabbitMQ baseline:** shared queue with `prefetch = 1`.
2. **Round Robin:** a separate dispatcher assigns tasks to worker queues in cyclic order.
3. **Least Loaded:** tasks assigned using the current known worker backlog.
4. **Adaptive Scheduling:** tasks assigned using estimates of task execution cost and worker availability.

The adaptive dispatcher calibrates each worker with complete local service samples. Its model is `T(n) = cubic_ms_at_400 * (n / 400)^3 + quadratic_ms_at_400 * (n / 400)^2 + constant_ms`: multiplication supplies the cubic component, while input generation, checksumming and result serialization supply the quadratic and constant components. Actual `service_ms` observations update a multiplier for the complete model using EWMA with `alpha=0.2`; this also absorbs worker-specific RabbitMQ publication cost.

Tasks are considered strictly in incoming FIFO order. For each task, the dispatcher minimizes `estimated remaining work + predicted task duration`. A busy fast worker can be preferred over an idle slow one. Remaining work includes the estimated unfinished part of the first assigned task plus the full predicted costs of queued tasks. The first task's start is approximated locally from assignment or receipt of the preceding completion; time already elapsed is subtracted only from that task, never from tasks still waiting behind it. Costs are recalculated from the current EWMA estimate on every decision.

The default dispatch window contains at most six assigned unfinished tasks across all workers. Admission is limited globally: every assignment still compares all calibrated workers, including busy ones. There is no separate per-worker queue limit that would force dispatch to whichever worker just freed a slot. Override `ADAPTIVE_DISPATCH_WINDOW` in `.env` (1..256) to compare window sizes; reports record `max_in_flight_total` and the policy name `full-service-model-window-v4`. When the global window is full, the dispatcher waits for a completion. The remaining pool stays in the incoming queue, with at most one extra unassigned message prefetched by the dispatcher. Independent incoming/completion connections keep completion processing available while task dispatch is paused. This remains an online strategy and does not inspect future tasks or reorder the pool. It does not guarantee an improvement over the RabbitMQ baseline.

## Workload

Tasks consist of square-matrix multiplications with randomized dimensions. The producer sends task parameters, not entire matrices; workers generate reproducible input data from a task-specific seed.

Example message:

```json
{
  "task_id": 1024,
  "matrix_size": 743,
  "seed": 12345
}
```

The checked-in `workloads/default.jsonl` contains 1000 tasks with sizes from 100 to 500, generated once with seed 2026. Each line fixes the task ID, matrix size and matrix seed. Workers generate identical matrix values from that seed using `std::mt19937`. The file and task order are reused for every strategy.

## Metrics and Evaluation

We plan to measure:

- **Throughput:** completed tasks per second.
- **End-to-end latency:** time from task submission to completion, including median and p95.
- **Queue waiting time:** time spent waiting before processing starts.
- **Worker utilization:** how effectively available computing resources are used.
- **Queue depth and scheduling overhead:** backlog and additional cost of dispatching.

Experiments will be repeated with comparable task sequences and worker configurations. Both homogeneous and heterogeneous worker capacities will be evaluated. Per-task timestamps will be recorded for precise analysis; Prometheus and Grafana will provide live operational dashboards.

## Technology Stack

| Component | Technology |
| --- | --- |
| Producer, dispatcher, workers | C++17 |
| Message broker | RabbitMQ (AMQP 0-9-1) |
| RabbitMQ C++ client | SimpleAmqpClient |
| Message format | JSON |
| Build system | CMake |
| Metrics collection | Prometheus / prometheus-cpp |
| Dashboards | Grafana |
| Service orchestration | Docker Compose |

## Running the Project

For comparable experiments on Windows, run one mode at a time with the benchmark runner:

```powershell
.\scripts\run_benchmark.ps1 -Mode adaptive
.\scripts\run_benchmark.ps1 -Mode round-robin
.\scripts\run_benchmark.ps1 -Mode baseline
```

Use `-Alpha` to change how strongly the adaptive scheduler weights the newest
complete service-time observation. For example:

```powershell
.\scripts\run_benchmark.ps1 -Mode adaptive -Alpha 0.1
```

The accepted range is `0.001..1.0`; the default is `0.2`. Every adaptive JSON
report records the selected value as `scheduler.ewma_alpha`.

Each invocation builds the required images, removes containers from all three profiles, and starts exactly one strategy with three fresh workers. Runtime data uses the containers' writable disk layers, with no persistent Docker volumes: RabbitMQ stores its database under `/tmp/rabbitmq/mnesia`, Prometheus under `/tmp/prometheus`, and Grafana under `/var/lib/grafana`. Removing containers discards queues, messages, Prometheus history and Grafana users/dashboards/settings; the default Prometheus data source is provisioned again automatically. Small `tmpfs` mounts mask the built-in RabbitMQ/Prometheus image volume paths so Docker does not create anonymous volumes there. The actual message store and TSDB stay on disk. A unique `EXPERIMENT_ID` still identifies the run and its queues. The fixed workload and JSON reports are bind-mounted host files and are retained. Use `-NoBuild` to reuse images after they have been built from the current sources.

For a complete manual shutdown and reset of this project, use:

```powershell
docker compose --profile adaptive --profile round-robin --profile baseline down --remove-orphans
```

The Compose file declares no named data volumes, so `--volumes` is not needed. A restart of the same container keeps its disk layer; `down` followed by `up` creates fresh data. Bind mounts with configuration files, the workload and JSON reports are still needed to connect the containers to the project files. Image layers and build caches are retained by the reset command; they do not contain the runtime RabbitMQ messages or monitoring history. Results from the previous volume-backed setup should be kept separate when comparing benchmark series because the storage layout has changed.

Before starting processing services, the runner waits for RabbitMQ's AMQP listener through the management HTTP health endpoint. This avoids repeatedly launching Erlang CLI VMs during measurements. Producer then waits until all three workers have finished calibration and subscribed to their queues; routed modes also wait for their dispatcher. It publishes the pool once, with no artificial arrival delay, and waits for all unique task IDs to complete. Its restart policy is disabled so a finished experiment is not replayed automatically. In `baseline`, producer publishes directly to the shared queue; neither dispatcher starts.

The producer receives completion notifications through its own temporary reply queue, leaving the adaptive scheduler's completion consumer independent. Each report in `results/<experiment_id>.json` contains the mode, workload fingerprint, expected/completed task counts, elapsed seconds, throughput, counts per worker, and per-task checksums and timings. `elapsed_seconds` measures from the first publish attempt until producer receives the final completion notification, after that worker has acknowledged its task. Calibration and container startup are excluded; routing, input generation, computation, CPU throttling and completion delivery are included. A timeout or missing completion marks the report `failed` and the runner exits with an error.

Worker capacity is limited by Docker CPU quotas: worker 1 gets at most `1.0` CPU, worker 2 `0.5`, and worker 3 `0.25`. These are the same limits in all three modes. Worker computation contains no artificial sleep. CPU quotas cap available processor time; they do not reserve dedicated cores. Because matrix multiplication is single-threaded, increasing a worker's limit above one CPU will not speed up that computation. Docker Desktop must have enough CPU capacity for the workers plus RabbitMQ, the dispatcher and monitoring.

Override the limits in `.env` when needed, keeping them identical between strategy comparisons:

```dotenv
WORKER1_CPUS=1.0
WORKER2_CPUS=0.5
WORKER3_CPUS=0.25
```

Worker calibration runs complete `400x400` local service samples for at least two seconds and at least three repetitions, spanning multiple CPU quota periods. `compute_ms` measures matrix multiplication only. `service_ms` (also written to the legacy `elapsed_ms` field) starts when the worker receives a task and includes JSON parsing, input generation, multiplication, checksumming, result serialization and result publication. Run the benchmark script again to recreate workers with the new quotas; after changing C++ sources, keep image building enabled.

Review completed runs:

```powershell
Get-ChildItem results/*.json | ForEach-Object {
    Get-Content -Raw $_.FullName | ConvertFrom-Json
} | Select-Object mode, status, expected, completed, elapsed_seconds, tasks_per_second |
    Format-Table -AutoSize
```

Compare runs with the same workload fingerprint, worker settings and arrival interval, and repeat each mode several times. The default benchmark stores checksums instead of full result matrices (`WORKER_SEND_FULL_RESULT=false`). Prometheus/Grafana counters remain available for observing progress; the JSON report supplies the final experiment measurement.

To create another pool, Python requires no extra packages:

```powershell
py scripts/generate_workload.py --count 2000 --seed 2026 --min-size 100 --max-size 500 --output workloads/pool-2000.jsonl
.\scripts\run_benchmark.ps1 -Mode baseline -Workload workloads/pool-2000.jsonl
```

The generator refuses to overwrite an existing pool. `-IntervalMs 100` gives a fixed 100 ms arrival interval; the default `0` sends the finite pool as quickly as possible. `-TimeoutSeconds 3600` sets the maximum measured run duration; the separate readiness timeout defaults to 300 seconds.

Two deterministic high-variance profiles are checked in for experiments where
task-size-aware routing should matter. Both contain the same multiset of 96
small (`100..200`) and 24 heavy (`700..900`) tasks generated with seed 2026;
only their order differs:

```powershell
py scripts/generate_workload.py --profile bimodal-shuffled --output workloads/bimodal-shuffled-120.jsonl
py scripts/generate_workload.py --profile heavy-tail --output workloads/heavy-tail-120.jsonl

.\scripts\run_benchmark.ps1 -Mode adaptive -Alpha 0.05 -Workload workloads/bimodal-shuffled-120.jsonl
.\scripts\run_benchmark.ps1 -Mode baseline -Workload workloads/bimodal-shuffled-120.jsonl -NoBuild
```

`bimodal-shuffled` mixes small and heavy tasks deterministically. `heavy-tail`
places all heavy tasks at the end, exposing end-of-batch stragglers without
changing the amount of work.

Prometheus is available at `http://localhost:9090`. Grafana is available at `http://localhost:3000` with Prometheus preconfigured as its default data source. On a fresh Grafana volume, sign in with `admin` / `admin` and change the password when prompted. Each worker exposes `/metrics` on port 9100 inside the Compose network. The counter `optotech_worker_tasks_completed_total{worker_id="1"}` (and similarly for workers 2 and 3) increases after the worker acknowledges a completed task. In Prometheus or Grafana Explore, use:

```promql
sum by (worker_id) (optotech_worker_tasks_completed_total)
```

for the current count per worker, or:

```promql
sum(optotech_worker_tasks_completed_total)
```

for the current total. Counters start at zero when a worker process starts, so a restart resets its count. For completed tasks over a time window that may include restarts, use `sum by (worker_id) (increase(optotech_worker_tasks_completed_total[1h]))` or `sum(increase(optotech_worker_tasks_completed_total[1h]))`. These counts include only tasks completed since metrics were enabled.

For local C++ development, use the project's CMake configuration:

```bash
cmake -S . -B build
cmake --build build
```

The Round Robin executable is the CMake target `round_robin`.

## Reliability Considerations

The system is designed around manual consumer acknowledgements and publisher confirms. Task IDs will be used to identify potentially repeated deliveries. RabbitMQ persistence and delivery guarantees depend on queue and message configuration; the experimental system does not assume exactly-once execution.

## Scope

This project studies **task scheduling**, not machine learning and not the implementation of a new message broker. The aim is to identify the conditions under which an adaptive strategy is useful, as well as cases where the standard RabbitMQ approach performs equally well or better.

## License

To be determined.
