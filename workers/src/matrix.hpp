#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace worker {

using Matrix = std::vector<std::vector<std::int64_t>>;

// Deliberately single-threaded. Identical work on every worker makes
// execution timings comparable, and this access pattern is cache-friendly.
inline Matrix multiply(const Matrix& a, const Matrix& b) {
    const std::size_t n = a.size();
    if (n == 0 || b.size() != n) {
        throw std::invalid_argument("matrices must be nonempty and equally sized");
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i].size() != n || b[i].size() != n) {
            throw std::invalid_argument("matrices must be square");
        }
    }

    Matrix result(n, std::vector<std::int64_t>(n, 0));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < n; ++k) {
            const std::int64_t a_ik = a[i][k];
            for (std::size_t j = 0; j < n; ++j) {
                result[i][j] += a_ik * b[k][j];
            }
        }
    }
    return result;
}

inline std::int64_t checksum(const Matrix& matrix) {
    std::int64_t sum = 0;
    for (const auto& row : matrix) {
        for (std::int64_t value : row) {
            sum += value;
        }
    }
    return sum;
}

} // namespace worker
