#include <cstdio>
#include <cstring>
#include <stdexcept>

struct Cleanup {
    int& count;
    ~Cleanup() { ++count; }
};

__declspec(noinline) void expected_failure(int& count) {
    const Cleanup cleanup{count};
    try {
        throw std::runtime_error("expected unwind");
    } catch (...) {
        throw;
    }
}

int main() {
    int count = 0;
    bool caught = false;
    try {
        expected_failure(count);
    } catch (const std::runtime_error& error) {
        caught = std::strcmp(error.what(), "expected unwind") == 0;
    }
    std::printf("caught=%d cleanup_count=%d\n", caught ? 1 : 0, count);
    return caught && count == 1 ? 0 : 1;
}
