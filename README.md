# Adaptive Task Scheduler

An experimental distributed task-processing system built with **C++20** and **RabbitMQ**. The project explores whether an adaptive scheduler can improve task completion times compared with conventional message distribution strategies.

This project is being developed as part of a research paper for the **Optotech 2026** conference (Industrial Programming section).

> **Status:** Work in progress. The RabbitMQ-connected producer is the starting point; the dispatcher, workers, and monitoring infrastructure are being developed.

## Research Goal

A shared queue with RabbitMQ's `prefetch = 1` already provides effective load distribution: workers request new tasks as they finish existing ones. However, it does not predict how long a particular task will take on a particular worker.

Our hypothesis is that **adaptive scheduling based on estimated task cost, worker performance, and current load** may reduce end-to-end latency under heterogeneous workloads. We will test this hypothesis rather than assume the adaptive approach is always better.

## Architecture

The proposed system has three types of C++ applications:

- **Producer:** generates matrix-multiplication tasks with randomized dimensions and configurable arrival intervals.
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

To evaluate the algorithm fairly, the same worker implementations will also consume from **one shared RabbitMQ queue** using `prefetch = 1`, with no adaptive dispatcher in the task path.

## Scheduling Strategies

The experimental comparison is planned to include:

1. **RabbitMQ baseline:** shared queue with `prefetch = 1`.
2. **Round Robin:** tasks assigned to workers in cyclic order.
3. **Least Loaded:** tasks assigned using the current known worker backlog.
4. **Adaptive Scheduling:** tasks assigned using estimates of task execution cost and worker availability.

The adaptive strategy will gather execution statistics over time. Initial tasks will be distributed for **cold-start exploration**, without assuming that any worker is fastest before measurements are available.

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

Planned workload profiles include uniform task sizes, a mix of small and large matrices, burst traffic, and sustained overload. A fixed random seed enables repeatable task sequences across scheduling strategies.

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
| Producer, dispatcher, workers | C++20 |
| Message broker | RabbitMQ (AMQP 0-9-1) |
| RabbitMQ C++ client | SimpleAmqpClient |
| Message format | JSON |
| Build system | CMake |
| Metrics collection | Prometheus / prometheus-cpp |
| Dashboards | Grafana |
| Service orchestration | Docker Compose |

## Running the Project

The full multi-service setup is **not yet finalized**. Once the Docker Compose configuration is in place, the intended startup command will be:

```bash
docker compose up --build -d
```

For local C++ development, use the project's CMake configuration:

```bash
cmake -S . -B build
cmake --build build
```

Executable names, configuration variables, and service endpoints will be documented as the corresponding components are implemented.

## Reliability Considerations

The system is designed around manual consumer acknowledgements and publisher confirms. Task IDs will be used to identify potentially repeated deliveries. RabbitMQ persistence and delivery guarantees depend on queue and message configuration; the experimental system does not assume exactly-once execution.

## Scope

This project studies **task scheduling**, not machine learning and not the implementation of a new message broker. The aim is to identify the conditions under which an adaptive strategy is useful, as well as cases where the standard RabbitMQ approach performs equally well or better.

## License

To be determined.
