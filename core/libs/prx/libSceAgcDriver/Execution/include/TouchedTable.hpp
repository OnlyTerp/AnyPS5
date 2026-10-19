#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TOUCHEDTABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TOUCHEDTABLE_HPP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace AgcDriver::GuestMemory {

enum class TableBacking { Heap, Reserved };

template <class T>
class TouchedTable {
    static_assert(std::is_unsigned_v<T> && sizeof(std::uint64_t) % sizeof(T) == 0);

public:
    static constexpr std::size_t PageBytes = 65536;
    static constexpr std::size_t PageEntries = PageBytes / sizeof(T);

    TouchedTable() = default;
    TouchedTable(const TouchedTable&) = delete;
    TouchedTable& operator=(const TouchedTable&) = delete;

    ~TouchedTable() {
        release();
    }

    bool Allocate(std::size_t entries, TableBacking kind) {
        release();
        if (entries == 0) return false;
        const auto pages = (entries + PageEntries - 1) / PageEntries;
        const auto bytes = pages * PageBytes;
        void* reserved = nullptr;
        if (kind == TableBacking::Heap) {
            reserved = std::calloc(bytes, 1);
        } else {
#ifdef _WIN32
            reserved = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
            reserved = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (reserved == MAP_FAILED) reserved = nullptr;
#endif
        }
        if (reserved == nullptr) return false;
        touched = std::make_unique<std::atomic<std::uint64_t>[]>((pages + 63) / 64);
        values = static_cast<T*>(reserved);
        count = entries;
        reservedBytes = bytes;
        backing = kind;
        return true;
    }

    std::size_t Entries() const {
        return count;
    }

    const void* Reservation() const {
        return values;
    }

    std::size_t ReservedBytes() const {
        return reservedBytes;
    }

    std::size_t CommittedBytes() const {
        if (backing == TableBacking::Heap) return reservedBytes;
        std::size_t pages = 0;
        for (std::size_t word = 0; word < (reservedBytes / PageBytes + 63) / 64; ++word) pages += static_cast<std::size_t>(std::popcount(touched[word].load(std::memory_order_acquire)));
        return pages * PageBytes;
    }

    bool Touched(std::size_t index) const {
        if (backing == TableBacking::Heap) return true;
        const auto page = index / PageEntries;
        return ((touched[page / 64].load(std::memory_order_acquire) >> (page % 64)) & 1u) != 0;
    }

    T Load(std::size_t index) const {
        if (!Touched(index)) return 0;
        return std::atomic_ref<T>(values[index]).load(std::memory_order_relaxed);
    }

    void Store(std::size_t index, T value) {
        touch(index / PageEntries);
        std::atomic_ref<T>(values[index]).store(value, std::memory_order_relaxed);
    }

    void Clear(std::size_t first, std::size_t end) {
        while (first < end) {
            const auto stop = pageEnd(first, end);
            if (Touched(first)) {
                for (auto index = first; index < stop; ++index) std::atomic_ref<T>(values[index]).store(0, std::memory_order_relaxed);
            }
            first = stop;
        }
    }

    std::size_t RunEnd(std::size_t index, T value, std::size_t end) const {
        constexpr std::size_t Wide = sizeof(std::uint64_t) / sizeof(T);
        std::uint64_t wide = 0;
        for (std::size_t lane = 0; lane < Wide; ++lane) wide |= static_cast<std::uint64_t>(value) << (lane * 8u * sizeof(T));
        while (index < end) {
            const auto stop = pageEnd(index, end);
            if (!Touched(index)) {
                if (value != 0) return index;
                index = stop;
                continue;
            }
            while (index < stop) {
                if (index % Wide == 0 && index + Wide <= stop && std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(values + index)).load(std::memory_order_relaxed) == wide) {
                    index += Wide;
                    continue;
                }
                if (std::atomic_ref<T>(values[index]).load(std::memory_order_relaxed) != value) return index;
                ++index;
            }
        }
        return index;
    }

    template <class Predicate>
    bool AnyOf(std::size_t first, std::size_t end, Predicate predicate) const {
        while (first < end) {
            const auto stop = pageEnd(first, end);
            if (Touched(first)) {
                for (auto index = first; index < stop; ++index) {
                    if (predicate(std::atomic_ref<T>(values[index]).load(std::memory_order_relaxed))) return true;
                }
            }
            first = stop;
        }
        return false;
    }

    T Max(std::size_t first, std::size_t end) const {
        T highest = 0;
        static_cast<void>(AnyOf(first, end, [&highest](T value) {
            if (value > highest) highest = value;
            return false;
        }));
        return highest;
    }

private:
    static std::size_t pageEnd(std::size_t index, std::size_t end) {
        const auto next = (index / PageEntries + 1) * PageEntries;
        return next < end ? next : end;
    }

    void touch(std::size_t page) {
        if (backing == TableBacking::Heap) return;
        auto& word = touched[page / 64];
        const auto bit = std::uint64_t{1} << (page % 64);
        if ((word.load(std::memory_order_acquire) & bit) != 0) return;
        std::lock_guard lock(mutex);
        if ((word.load(std::memory_order_relaxed) & bit) != 0) return;
        auto* at = reinterpret_cast<std::byte*>(values) + page * PageBytes;
#ifdef _WIN32
        if (VirtualAlloc(at, PageBytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) throw std::runtime_error("cannot commit a page of a guest memory table");
#else
        if (mprotect(at, PageBytes, PROT_READ | PROT_WRITE) != 0) throw std::runtime_error("cannot commit a page of a guest memory table");
#endif
        word.fetch_or(bit, std::memory_order_release);
    }

    void release() {
        if (values == nullptr) return;
        if (backing == TableBacking::Heap) {
            std::free(values);
        } else {
#ifdef _WIN32
            VirtualFree(values, 0, MEM_RELEASE);
#else
            munmap(values, reservedBytes);
#endif
        }
        values = nullptr;
        count = 0;
        reservedBytes = 0;
        touched.reset();
    }

    T* values = nullptr;
    std::size_t count = 0;
    std::size_t reservedBytes = 0;
    TableBacking backing = TableBacking::Heap;
    std::unique_ptr<std::atomic<std::uint64_t>[]> touched;
    std::mutex mutex;
};

}

#endif
