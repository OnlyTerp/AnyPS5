#include "prx/libSceAgcDriver/Execution/include/TouchedTable.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <fstream>
#include <sstream>
#endif

namespace {

using AgcDriver::GuestMemory::TableBacking;
using AgcDriver::GuestMemory::TouchedTable;

constexpr std::size_t TablePage = TouchedTable<std::uint8_t>::PageBytes;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::size_t MappedBytes(const void* reservation, std::size_t bytes) {
    const auto begin = reinterpret_cast<std::uintptr_t>(reservation);
    const auto end = begin + bytes;
    std::size_t committed = 0;
#ifdef _WIN32
    for (auto cursor = begin; cursor < end;) {
        MEMORY_BASIC_INFORMATION memory{};
        Require(VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == sizeof(memory) && memory.RegionSize != 0, "cannot query the table's reservation");
        const auto regionEnd = std::min<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize, end);
        if (memory.State == MEM_COMMIT) committed += regionEnd - cursor;
        cursor = regionEnd;
    }
#else
    std::ifstream maps("/proc/self/maps");
    Require(maps.is_open(), "cannot read /proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range;
        std::string permissions;
        fields >> range >> permissions;
        const auto dash = range.find('-');
        const auto first = static_cast<std::uintptr_t>(std::stoull(range.substr(0, dash), nullptr, 16));
        const auto last = static_cast<std::uintptr_t>(std::stoull(range.substr(dash + 1), nullptr, 16));
        if (permissions.compare(0, 2, "rw") != 0 || last <= begin || first >= end) continue;
        committed += std::min(last, end) - std::max(first, begin);
    }
#endif
    return committed;
}

void CheckCommitCharge() {
    constexpr std::size_t Entries = std::size_t{250} << 20;
    constexpr std::size_t Regions = 64;
    constexpr std::size_t PagesPerRegion = 16;
    TouchedTable<std::uint8_t> table;
    Require(table.Allocate(Entries, TableBacking::Reserved), "cannot reserve a 250 MiB table");
    Require(table.ReservedBytes() >= Entries && MappedBytes(table.Reservation(), table.ReservedBytes()) == 0 && table.CommittedBytes() == 0, "a reserved table is committed before its first write");
    std::vector<std::size_t> firsts;
    for (std::size_t region = 0; region < Regions; ++region) {
        const auto first = region * (Entries / Regions) + region * 4099u;
        firsts.push_back(first);
        for (std::size_t page = 0; page < PagesPerRegion; ++page) table.Store(first + page, 3);
    }
    const auto committed = table.CommittedBytes();
    const auto mapped = MappedBytes(table.Reservation(), table.ReservedBytes());
    Require(committed >= Regions * TablePage && committed <= 2 * Regions * TablePage, "recording " + std::to_string(Regions) + " scattered regions committed " + std::to_string(committed) + " bytes, not one or two table pages each");
    Require(mapped == committed, "the system reports " + std::to_string(mapped) + " committed bytes in the reservation, the table " + std::to_string(committed));
    std::mt19937_64 random(7);
    for (int probe = 0; probe < 100000; ++probe) {
        const auto index = static_cast<std::size_t>(random() % Entries);
        const bool recorded = std::any_of(firsts.begin(), firsts.end(), [&](std::size_t first) { return index >= first && index < first + PagesPerRegion; });
        Require(table.Load(index) == (recorded ? 3u : 0u), "a load returned another value than the one recorded");
    }
    Require(table.RunEnd(firsts[1] + PagesPerRegion, 0, firsts[2]) == firsts[2], "a run of zeros over untouched table pages did not end at the next recorded page");
    Require(table.RunEnd(firsts[1], 3, firsts[2]) == firsts[1] + PagesPerRegion, "a run of a recorded value did not end after its region");
    Require(table.RunEnd(firsts[1] + PagesPerRegion + 2 * TablePage, 3, Entries) == firsts[1] + PagesPerRegion + 2 * TablePage, "a run of a recorded value continued into an untouched table page");
    table.Clear(0, Entries);
    Require(table.CommittedBytes() == committed && MappedBytes(table.Reservation(), table.ReservedBytes()) == committed, "loads, run scans or a clear committed table pages");
    Require(table.Load(firsts[5]) == 0 && table.RunEnd(0, 0, Entries) == Entries, "a clear left recorded pages");
    std::printf("commit charge: %zu scattered regions of %zu pages in a %zu MiB table commit %zu KiB (system: %zu KiB)\n", Regions, PagesPerRegion, table.ReservedBytes() >> 20, committed >> 10, mapped >> 10);
}

template <class T>
void CheckEquivalence(TableBacking backing, const char* name) {
    constexpr std::size_t Entries = 40 * TouchedTable<T>::PageEntries + 123;
    TouchedTable<T> table;
    Require(table.Allocate(Entries, backing), std::string("cannot allocate the ") + name + " table");
    std::vector<T> reference(Entries, 0);
    std::mt19937_64 random(sizeof(T) * 31 + static_cast<unsigned>(backing));
    const auto pick = [&] { return static_cast<std::size_t>(random() % Entries); };
    const auto around = [&](std::size_t index) { return std::min(Entries, index + static_cast<std::size_t>(random() % (3 * TouchedTable<T>::PageEntries))); };
    for (int step = 0; step < 200000; ++step) {
        const auto what = random() % 100;
        const auto index = pick();
        const auto label = std::string(name) + " step " + std::to_string(step);
        if (what < 40) {
            constexpr auto PageEntries = TouchedTable<T>::PageEntries;
            auto first = index - index % 8;
            if ((first / PageEntries) % 3 == 1) first = (first / PageEntries + 1) * PageEntries;
            const auto value = static_cast<T>(random() % 4);
            const auto run = static_cast<std::size_t>(random() % 64);
            for (auto at = first; at < std::min({Entries, first + run, (first / PageEntries + 1) * PageEntries}); ++at) {
                table.Store(at, value);
                reference[at] = value;
            }
        } else if (what < 70) {
            Require(table.Load(index) == reference[index], label + ": a load differs");
        } else if (what < 90) {
            const auto value = static_cast<T>(random() % 4);
            const auto end = around(index);
            auto expected = index;
            while (expected < end && reference[expected] == value) ++expected;
            Require(table.RunEnd(index, value, end) == std::max(expected, index), label + ": a run scan differs");
        } else if (what < 95) {
            const auto end = around(index);
            table.Clear(index, end);
            std::fill(reference.begin() + static_cast<std::ptrdiff_t>(index), reference.begin() + static_cast<std::ptrdiff_t>(end), T{0});
        } else {
            const auto end = around(index);
            const auto since = static_cast<T>(random() % 4);
            const bool newer = std::any_of(reference.begin() + static_cast<std::ptrdiff_t>(index), reference.begin() + static_cast<std::ptrdiff_t>(end), [&](T value) { return value > since; });
            const auto highest = index == end ? T{0} : *std::max_element(reference.begin() + static_cast<std::ptrdiff_t>(index), reference.begin() + static_cast<std::ptrdiff_t>(end));
            Require(table.AnyOf(index, end, [&](T value) { return value > since; }) == newer && table.Max(index, end) == highest, label + ": a range query differs");
        }
    }
    for (std::size_t index = 0; index < Entries; ++index) Require(table.Load(index) == reference[index], std::string(name) + ": the final contents differ");
    if (backing == TableBacking::Reserved) Require(table.CommittedBytes() < table.ReservedBytes() && !table.Touched(TouchedTable<T>::PageEntries), std::string(name) + ": a table page that was never written is committed");
    std::printf("equivalence: %s matches a committed array over 200000 loads, stores, run scans, clears and range queries (%zu of %zu KiB committed)\n", name, table.CommittedBytes() >> 10, table.ReservedBytes() >> 10);
}

void CheckConcurrentFirstWrites() {
    constexpr std::size_t Threads = 8;
    constexpr std::size_t Pages = 32;
    TouchedTable<std::uint32_t> table;
    Require(table.Allocate(Pages * TouchedTable<std::uint32_t>::PageEntries, TableBacking::Reserved), "cannot reserve the shared table");
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (std::size_t thread = 0; thread < Threads; ++thread) {
        workers.emplace_back([&, thread] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::size_t page = 0; page < Pages; ++page) table.Store(page * TouchedTable<std::uint32_t>::PageEntries + thread * 97u, static_cast<std::uint32_t>(thread + 1));
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    for (std::size_t page = 0; page < Pages; ++page) {
        for (std::size_t thread = 0; thread < Threads; ++thread) Require(table.Load(page * TouchedTable<std::uint32_t>::PageEntries + thread * 97u) == thread + 1, "a first write raced by other threads was lost");
    }
    Require(table.CommittedBytes() == Pages * TablePage && MappedBytes(table.Reservation(), table.ReservedBytes()) == Pages * TablePage, "racing first writes did not commit each table page once");
    std::printf("concurrency: %zu threads writing first into the same %zu table pages commit each once\n", Threads, Pages);
}

template <class Run>
double NanosecondsPer(std::size_t operations, Run run) {
    const auto start = std::chrono::steady_clock::now();
    run();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / static_cast<double>(operations);
}

int Bench() {
    constexpr std::size_t Entries = std::size_t{64} << 20;
    constexpr std::size_t Operations = std::size_t{1} << 27;
    auto* plain = static_cast<std::uint8_t*>(std::calloc(Entries, 1));
    Require(plain != nullptr, "cannot allocate the committed array");
    TouchedTable<std::uint8_t> heap;
    TouchedTable<std::uint8_t> reserved;
    TouchedTable<std::uint8_t> untouched;
    Require(heap.Allocate(Entries, TableBacking::Heap) && reserved.Allocate(Entries, TableBacking::Reserved) && untouched.Allocate(Entries, TableBacking::Reserved), "cannot allocate the benchmark tables");
    for (std::size_t index = 0; index < Entries; index += TouchedTable<std::uint8_t>::PageEntries) reserved.Store(index, 0);
    std::uint64_t sink = 0;
    std::mt19937_64 random(11);
    for (const std::size_t window : {Entries, std::size_t{256} << 10}) {
        std::vector<std::size_t> indices(1u << 16);
        for (auto& index : indices) index = static_cast<std::size_t>(random() % window);
        for (int round = 0; round < 3; ++round) {
            const auto plainStore = NanosecondsPer(Operations, [&] { for (std::size_t i = 0; i < Operations; ++i) std::atomic_ref<std::uint8_t>(plain[indices[i & 0xffffu]]).store(static_cast<std::uint8_t>(i), std::memory_order_relaxed); });
            const auto plainLoad = NanosecondsPer(Operations, [&] { for (std::size_t i = 0; i < Operations; ++i) sink += std::atomic_ref<std::uint8_t>(plain[indices[i & 0xffffu]]).load(std::memory_order_relaxed); });
            const auto heapStore = NanosecondsPer(Operations, [&] { for (std::size_t i = 0; i < Operations; ++i) heap.Store(indices[i & 0xffffu], static_cast<std::uint8_t>(i)); });
            const auto heapLoad = NanosecondsPer(Operations, [&] { for (std::size_t i = 0; i < Operations; ++i) sink += heap.Load(indices[i & 0xffffu]); });
            const auto reservedStore = NanosecondsPer(Operations, [&] { for (std::size_t i = 0; i < Operations; ++i) reserved.Store(indices[i & 0xffffu], static_cast<std::uint8_t>(i)); });
            const auto reservedLoad = NanosecondsPer(Operations, [&] { for (std::size_t i = 0; i < Operations; ++i) sink += reserved.Load(indices[i & 0xffffu]); });
            const auto untouchedLoad = NanosecondsPer(Operations, [&] { for (std::size_t i = 0; i < Operations; ++i) sink += untouched.Load(indices[i & 0xffffu]); });
            std::printf("bench, %zu random entries in %zu KiB, round %d, ns per operation: committed array store %.3f load %.3f; heap table store %.3f load %.3f; reserved table, pages touched, store %.3f load %.3f; reserved table, untouched, load %.3f\n", indices.size(), window >> 10, round, plainStore, plainLoad, heapStore, heapLoad, reservedStore, reservedLoad, untouchedLoad);
        }
    }
    std::free(plain);
    std::printf("bench sink %llu\n", static_cast<unsigned long long>(sink));
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) return Bench();
        CheckCommitCharge();
        CheckEquivalence<std::uint8_t>(TableBacking::Reserved, "a reserved byte table");
        CheckEquivalence<std::uint8_t>(TableBacking::Heap, "a heap byte table");
        CheckEquivalence<std::uint32_t>(TableBacking::Reserved, "a reserved stamp table");
        CheckEquivalence<std::uint32_t>(TableBacking::Heap, "a heap stamp table");
        CheckConcurrentFirstWrites();
    } catch (const std::exception& error) {
        std::cerr << "touched table test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "touched table tests passed\n";
    return 0;
}
