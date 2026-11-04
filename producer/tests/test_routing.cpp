#include "routing.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    if (producer::queueForMode("adaptive") != "tasks.incoming" ||
        producer::queueForMode("round-robin") != "tasks.incoming" ||
        producer::queueForMode("baseline") != "tasks.shared") {
        std::cerr << "Producer chose the wrong queue for a strategy\n";
        return 1;
    }

    try {
        (void)producer::queueForMode("unknown");
    } catch (const std::invalid_argument&) {
        return 0;
    }

    std::cerr << "Producer accepted an unknown strategy\n";
    return 1;
}
