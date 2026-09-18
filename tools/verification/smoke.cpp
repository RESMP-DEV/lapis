#include <array>
#include <exception>
#include <future>
#include <iostream>
#include <numeric>
#include <vector>

// Exercises standard-library allocation and synchronized worker execution.
// This verifies the toolchain, not lapis session or rendering behavior.
int main() try {
    constexpr std::array expected{1, 2, 3, 4};
    auto worker = std::async(std::launch::async, [expected] {
        const std::vector<int> values(expected.begin(), expected.end());
        return std::accumulate(values.begin(), values.end(), 0);
    });
    return worker.get() == 10 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "Toolchain smoke failed: " << error.what() << '\n';
    return 2;
}
