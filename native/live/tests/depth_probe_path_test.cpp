#define VK_NO_PROTOTYPES
#include "../depth_probe.h"
#include <cassert>
#include <cstdio>
#include <new>

namespace { bool reject_allocations = false; }

void* operator new(std::size_t size) {
    if (reject_allocations) throw std::bad_alloc();
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

int main() {
    // Longer than string's small-buffer capacity: force the diagnostic-path
    // copy to fail before any Probe exists or original Vulkan lookup occurs.
    assert(setenv("CS2_CAMERA_DEPTH_PROBE", "/unused/depth-provenance-allocation-failure.jsonl", 1) == 0);
    reject_allocations = true;
    const char* path = cs2_camera_depth::ProbePath();
    reject_allocations = false;
    assert(path == nullptr);
    assert(cs2_camera_depth::ProbePath() == nullptr);
    std::puts("Depth diagnostic path allocation failure leaves Vulkan lookup unwrapped");
}
