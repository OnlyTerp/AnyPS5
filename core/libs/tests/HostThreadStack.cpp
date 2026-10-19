#include "prx/libc/include/HostThread.hpp"
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {

constexpr std::size_t StackBytes = std::size_t{64} << 20u;
constexpr std::size_t FrameBytes = 4000;
constexpr std::size_t FloorBytes = std::size_t{256} << 10u;

struct StackUse {
    std::uintptr_t low = 0;
    std::uintptr_t high = 0;
    std::size_t committedAtStart = 0;
    std::size_t committedAfterDescent = 0;
    std::uintptr_t deepest = 0;
};

bool StackLimits(std::uintptr_t& low, std::uintptr_t& high) {
#ifdef _WIN32
    ULONG_PTR lowLimit = 0;
    ULONG_PTR highLimit = 0;
    GetCurrentThreadStackLimits(&lowLimit, &highLimit);
    low = lowLimit;
    high = highLimit;
    return high > low;
#else
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) return false;
    void* address = nullptr;
    std::size_t size = 0;
    const bool queried = pthread_attr_getstack(&attributes, &address, &size) == 0;
    pthread_attr_destroy(&attributes);
    low = reinterpret_cast<std::uintptr_t>(address);
    high = low + size;
    return queried && high > low;
#endif
}

std::size_t Committed(std::uintptr_t low, std::uintptr_t high) {
#ifdef _WIN32
    std::size_t committed = 0;
    for (auto cursor = low; cursor < high;) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) || memory.RegionSize == 0) return 0;
        if (memory.State == MEM_COMMIT) committed += memory.RegionSize;
        cursor = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    }
    return committed;
#else
    return high - low;
#endif
}

std::uintptr_t Descend(std::uintptr_t floor);
std::uintptr_t (*volatile descend)(std::uintptr_t) = Descend;

std::uintptr_t Descend(std::uintptr_t floor) {
    volatile unsigned char frame[FrameBytes];
    frame[0] = 0x5a;
    frame[FrameBytes - 1] = 0xa5;
    const auto here = reinterpret_cast<std::uintptr_t>(&frame[0]);
    const auto deepest = here < floor ? here : descend(floor);
    if (frame[0] != 0x5a || frame[FrameBytes - 1] != 0xa5) return 0;
    return deepest;
}

void* Measure(void* argument) {
    auto& promise = *static_cast<std::promise<StackUse>*>(argument);
    StackUse use;
    if (!StackLimits(use.low, use.high)) {
        promise.set_value(use);
        return nullptr;
    }
    use.committedAtStart = Committed(use.low, use.high);
    use.deepest = descend(use.low + FloorBytes);
    use.committedAfterDescent = Committed(use.low, use.high);
    promise.set_value(use);
    return nullptr;
}

bool Check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

}

int main() {
    std::promise<StackUse> promise;
    auto measured = promise.get_future();
    if (!HostThread::StartDetached(StackBytes, Measure, &promise)) {
        std::cerr << "the host thread did not start\n";
        return 1;
    }
    const auto use = measured.get();
    const auto mib = [](std::size_t bytes) { return std::to_string(bytes >> 20u) + " MiB (" + std::to_string(bytes >> 10u) + " KiB)"; };
    bool passed = Check(use.high - use.low >= StackBytes, "the host thread's stack holds " + mib(use.high - use.low) + ", less than the 64 MiB asked for");
#ifdef _WIN32
    passed = Check(use.committedAtStart < (std::size_t{1} << 20u), "a host thread with a 64 MiB stack committed " + mib(use.committedAtStart) + " before touching it: the size was a commit, not a reservation") && passed;
    passed = Check(use.committedAfterDescent >= StackBytes - 2 * FloorBytes, "after touching its whole stack the thread committed only " + mib(use.committedAfterDescent)) && passed;
#endif
    passed = Check(use.deepest != 0 && use.deepest >= use.low && use.deepest < use.low + FloorBytes + 2 * FrameBytes, "the thread did not reach the bottom of its stack intact") && passed;
    if (!passed) return 1;
    std::cout << "a host thread with a 64 MiB stack committed " << (use.committedAtStart >> 10u) << " KiB at start and " << (use.committedAfterDescent >> 20u) << " MiB after touching it\n";
    return 0;
}
