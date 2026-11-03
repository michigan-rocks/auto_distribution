"""Simple integration test against a *fresh isolated* RabbitMQ test broker.

Requires the management plugin (port 15672) and a running worker. Consumes
messages from tasks.results, so do not run against a shared experiment.
"""
import base64
import json
import os
import time
import urllib.request

BASE_URL = os.getenv("RABBITMQ_MANAGEMENT_URL", "http://localhost:15672")
USERNAME = os.getenv("RABBITMQ_USER", "user")
PASSWORD = os.getenv("RABBITMQ_PASSWORD", "user")
WORKER_ID = int(os.getenv("WORKER_ID", "1"))


def post(path, data):
    authorization = base64.b64encode(f"{USERNAME}:{PASSWORD}".encode()).decode()
    request = urllib.request.Request(
        BASE_URL + path,
        data=json.dumps(data).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Basic " + authorization},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.load(response)


def main():
    task_id = time.time_ns()
    payload = {
        "task_id": task_id,
        "matrix_size": 2,
        "matrix_a": [[1, 2], [3, 4]],
        "matrix_b": [[5, 6], [7, 8]],
    }
    response = post("/api/exchanges/%2F/amq.default/publish", {
        "properties": {"content_type": "application/json", "delivery_mode": 2},
        "routing_key": f"worker.{WORKER_ID}",
        "payload": json.dumps(payload),
        "payload_encoding": "string",
    })
    assert response["routed"], "Task not routed; is the worker queue declared?"
    print("Task published:", task_id)

    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        messages = post("/api/queues/%2F/tasks.results/get", {
            "count": 1,
            "ackmode": "ack_requeue_false",
            "encoding": "auto",
            "truncate": 5000000,
        })
        for message in messages:
            result = json.loads(message["payload"])
            if result.get("task_id") == task_id:
                assert result["checksum"] == 134, result
                if "matrix_result" in result:
                    assert result["matrix_result"] == [[19, 22], [43, 50]], result
                print("PASS: result:", result)
                return
        time.sleep(0.25)
    raise TimeoutError("No matching result within 30 seconds; see worker logs")


if __name__ == "__main__":
    main()
