#include "prx/libSceAgcDriver/Execution/include/PipelineCreation.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver {

namespace {

struct RamSide {
    std::mutex mutex;
    std::chrono::steady_clock::time_point sampled{};
    std::size_t guestMapped = 0;
    CreationGate::MemorySource deviceLocal = nullptr;
};

RamSide& RamSideState() {
    static auto* state = new RamSide();
    return *state;
}

std::size_t GuestMappedBytes() {
    std::size_t bytes = 0;
#ifdef _WIN32
    if (!GuestArena::GuestArenaAvailable_nid_postfix()) return 0;
    std::uintptr_t base = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&base, &arenaBytes);
    const auto end = base + arenaBytes;
    for (auto cursor = base; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) break;
        const auto regionEnd = std::min(end, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
        if (regionEnd <= cursor) break;
        const auto size = regionEnd - cursor;
        if (info.State == MEM_COMMIT && info.Type == MEM_MAPPED && !GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(cursor, size)) bytes += size;
        cursor = regionEnd;
    }
#endif
    return bytes;
}

std::size_t PrivateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return counters.PrivateUsage;
#endif
    return 0;
}

}

bool& CreationAhead::Flag() {
    thread_local bool ahead = false;
    return ahead;
}

CreationGate::Shared& CreationGate::State() {
    static auto& shared = []() -> Shared& {
        auto* created = new Shared;
        created->memory = &RamSideBytes;
        return *created;
    }();
    return shared;
}

std::size_t CreationGate::DefaultBudgetBytes() {
    static const std::size_t budget = [] {
        if (const char* configured = std::getenv("APS5_PIPELINE_CREATION_BUDGET_MIB"); configured != nullptr && *configured != '\0') {
            char* end = nullptr;
            const auto mib = std::strtoull(configured, &end, 10);
            if (end != configured && *end == '\0' && mib != 0) return static_cast<std::size_t>(mib << 20u);
        }
        std::uint64_t physical = 0;
#ifdef _WIN32
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) physical = status.ullTotalPhys;
#else
        const auto pages = sysconf(_SC_PHYS_PAGES);
        const auto pageSize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && pageSize > 0) physical = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
#endif
        return static_cast<std::size_t>(physical != 0 ? physical / 16u : std::uint64_t{4} << 30u);
    }();
    return budget;
}

std::size_t CreationGate::RamSideBytes() {
    auto& state = RamSideState();
    std::lock_guard lock(state.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (state.sampled == std::chrono::steady_clock::time_point{} || now - state.sampled >= std::chrono::seconds(1)) {
        state.guestMapped = GuestMappedBytes();
        state.sampled = now;
    }
    const auto total = PrivateBytes() + state.guestMapped;
    const auto deviceLocal = state.deviceLocal != nullptr ? state.deviceLocal() : 0u;
    return total > deviceLocal ? total - deviceLocal : 0u;
}

void CreationGate::UseRamSide(MemorySource deviceLocal) {
    {
        auto& state = RamSideState();
        std::lock_guard lock(state.mutex);
        state.deviceLocal = deviceLocal;
    }
    SetMemorySource(&RamSideBytes, MemoryLimitBytes);
}

}
