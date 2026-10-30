#include <cstdlib>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct Body {
    int task_id;
    int matrix_size;
    std::vector<std::vector<int>> matrix_a;
    std::vector<std::vector<int>> matrix_b;
};

std::string MarshalBody(const Body& body);

std::string CreateRandomTask() {
    static int next_task_id = 1;
    const int matrix_size = 100 + std::rand() % 401;

    Body task{
        next_task_id++,
        matrix_size,
        std::vector<std::vector<int>>(
            matrix_size,
            std::vector<int>(matrix_size)
        ),
        std::vector<std::vector<int>>(
            matrix_size,
            std::vector<int>(matrix_size)
        )
    };

    for (int row = 0; row < matrix_size; ++row) {
        for (int column = 0; column < matrix_size; ++column) {
            task.matrix_a[row][column] = std::rand() % 10;
            task.matrix_b[row][column] = std::rand() % 10;
        }
    }

    return MarshalBody(task);
}

using json = nlohmann::json;

std::string MarshalBody(const Body& body) {
    json result;

    result["task_id"] = body.task_id;
    result["matrix_size"] = body.matrix_size;
    result["matrix_a"] = body.matrix_a;
    result["matrix_b"] = body.matrix_b;

    return result.dump();
}
 
