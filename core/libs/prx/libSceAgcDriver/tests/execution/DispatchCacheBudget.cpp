#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace {

using AgcDriver::DriverDetail::DispatchEntries;
using AgcDriver::DriverDetail::DispatchVariant;
using AgcDriver::DriverDetail::DispatchVariantBytes;
using AgcDriver::DriverDetail::PriorValueSets;

constexpr std::uint64_t CaptureBytes = std::uint64_t{64} << 20u;
constexpr std::uint64_t Budget = std::uint64_t{256} << 20u;
constexpr std::size_t EntryLimit = 8192;
constexpr std::uint64_t Captures = 40;
constexpr std::uint64_t Base = 0x200000000ull;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string Mib(std::uint64_t bytes) {
    return std::to_string(bytes >> 20u) + " MiB";
}

std::shared_ptr<DispatchVariant> Capture(std::uint32_t value) {
    auto variant = std::make_shared<DispatchVariant>();
    variant->runs.emplace_back(Base, Base + CaptureBytes);
    variant->words.assign(CaptureBytes / sizeof(std::uint32_t), value);
    return variant;
}

void Hits(const DispatchEntries& cache, std::uint64_t key, std::uint32_t value) {
    const auto entry = cache.Find(key);
    Require(entry != nullptr && !entry->variants.empty(), "key " + std::to_string(key) + " was evicted");
    const auto& words = entry->variants.front()->words;
    Require(words.size() == CaptureBytes / sizeof(std::uint32_t) && words.front() == value && words.back() == value, "key " + std::to_string(key) + " holds another capture");
}

void CapturesStayUnderTheBudget() {
    DispatchEntries cache(EntryLimit, Budget);
    std::uint64_t evicted = 0;
    for (std::uint64_t key = 1; key <= Captures; ++key) {
        auto variant = Capture(static_cast<std::uint32_t>(key));
        Require(cache.Admits(*variant), "a 64 MiB capture is refused under a 256 MiB budget");
        cache.Insert(key, std::move(variant), key);
        evicted += cache.Trim(key);
        Require(cache.Bytes() <= Budget, "after " + std::to_string(key) + " captures of 64 MiB the cache holds " + Mib(cache.Bytes()) + ", over its " + Mib(Budget) + " budget");
        Hits(cache, key, static_cast<std::uint32_t>(key));
        if (key == 20) Require(cache.Touch(18, 21, 0), "touching key 18 did not move it");
        if (key > 20 && key <= 22) Hits(cache, 18, 18);
    }
    Require(cache.Size() >= 2, "the cache keeps " + std::to_string(cache.Size()) + " entries of a 256 MiB budget");
    Require(cache.Find(Captures - 1) != nullptr && cache.Find(1) == nullptr, "the least recently used capture was kept over a newer one");
    Require(evicted == Captures - cache.Size(), "evictions " + std::to_string(evicted) + " do not account for the " + std::to_string(Captures - cache.Size()) + " captures dropped");
}

void VariantsOfOneEntryStayUnderTheBudget() {
    DispatchEntries cache(EntryLimit, Budget);
    cache.Insert(7, Capture(1), 0);
    for (std::uint32_t value = 2; value <= 6; ++value) {
        cache.Prepend(7, Capture(value), 4, value);
        cache.Trim(7);
        Require(cache.Bytes() <= Budget, "variant " + std::to_string(value) + " of one entry left the cache at " + Mib(cache.Bytes()));
        Hits(cache, 7, value);
    }
}

std::uint64_t PrivateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0, "cannot read the process memory counters");
    return counters.PrivateUsage;
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t size = 0;
    std::uint64_t resident = 0;
    statm >> size >> resident;
    Require(static_cast<bool>(statm), "cannot read /proc/self/statm");
    return resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

void PriorValueSetsKeepNoCopies() {
    PriorValueSets sets(EntryLimit);
    const auto before = PrivateBytes();
    for (std::uint64_t key = 1; key <= 8; ++key) sets.Remember(key, *Capture(static_cast<std::uint32_t>(key)));
    const auto after = PrivateBytes();
    const auto grown = after > before ? after - before : 0;
    Require(grown < CaptureBytes, "remembering 8 value sets of 64 MiB kept " + Mib(grown) + " of process memory");
    Require(sets.Matches(8, *Capture(8)) && !sets.Matches(8, *Capture(7)), "a value set was not matched by its own words or was matched by another key's");
    for (std::uint32_t value = 100; value < 103; ++value) sets.Remember(1, *Capture(value));
    Require(!sets.Matches(1, *Capture(1)) && sets.Matches(1, *Capture(100)) && sets.Matches(1, *Capture(102)), "a key does not keep exactly its last 3 value sets");
}

void OversizedCaptureIsRefused() {
    DispatchEntries cache(EntryLimit, CaptureBytes / 2);
    Require(!cache.Admits(*Capture(1)), "a 64 MiB capture was admitted under a 32 MiB budget");
    auto padded = std::make_shared<DispatchVariant>();
    padded->words.reserve(1024);
    padded->words.resize(16);
    Require(DispatchVariantBytes(*padded) >= padded->words.capacity() * sizeof(std::uint32_t), "a variant is accounted by size, not by the memory it holds");
}

}

int main() {
    try {
        CapturesStayUnderTheBudget();
        VariantsOfOneEntryStayUnderTheBudget();
        PriorValueSetsKeepNoCopies();
        OversizedCaptureIsRefused();
        std::cout << "captures of 64 MiB kept the dispatch cache under its 256 MiB budget and still hit\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
