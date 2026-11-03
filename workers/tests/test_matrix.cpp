#include "../src/matrix.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
    const worker::Matrix a{{1, 2}, {3, 4}};
    const worker::Matrix b{{5, 6}, {7, 8}};
    const worker::Matrix expected{{19, 22}, {43, 50}};
    assert(worker::multiply(a, b) == expected);
    assert(worker::checksum(expected) == 134);

    const worker::Matrix identity{{1, 0}, {0, 1}};
    assert(worker::multiply(a, identity) == a);
    assert(worker::multiply(identity, b) == b);

    const worker::Matrix signed_values{{-1, 2}, {3, -4}};
    const worker::Matrix signed_expected{{5, 6}, {-9, -10}};
    assert(worker::multiply(signed_values, a) == signed_expected);

    bool rejected = false;
    try {
        (void)worker::multiply({{1, 2}}, {{3, 4}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    std::cout << "All matrix tests passed\n";
}
