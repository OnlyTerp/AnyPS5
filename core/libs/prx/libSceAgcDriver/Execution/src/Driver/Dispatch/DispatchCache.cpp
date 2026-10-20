#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include <cstdlib>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver::DriverDetail {

bool Driver::stampValidate() {
    static const bool stamp = std::getenv("APS5_DISPATCH_STAMP_VALIDATE") != nullptr;
    return stamp;
}

bool Driver::dataHits() {
    static const bool hits = std::getenv("APS5_NO_DATA_HITS") == nullptr && VulkanDevice::TemplateDataRefresh();
    return hits;
}

bool Driver::verifyDataHits() {
    static const bool verify = std::getenv("APS5_VERIFY_DATA_HITS") != nullptr;
    return verify;
}

std::size_t Driver::dispatchVariants() {
    static const std::size_t variants = [] {
        if (stampValidate()) return std::size_t{1};
        const char* text = std::getenv("APS5_DISPATCH_VARIANTS");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 8ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, 1, MaxDispatchVariants));
    }();
    return variants;
}

bool Driver::insertCompare() {
    static const bool compare = std::getenv("APS5_INSERT_COMPARE") != nullptr;
    return compare;
}

std::size_t Driver::dispatchCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        if (parsed != 0) return static_cast<std::size_t>(parsed);
        if (stampValidate()) return std::size_t{16384};
        return dispatchVariants() > 2 ? std::size_t{16384} : std::size_t{65536};
    }();
    return entries;
}

std::uint64_t Driver::DispatchCacheBytes() {
    static const std::uint64_t bytes = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_BYTES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{384} << 20u;
    }();
    return bytes;
}

std::uint64_t Driver::dispatchCacheBudget() {
    static const std::uint64_t budget = [] {
        if (std::getenv("APS5_DISPATCH_CACHE_BYTES") != nullptr) return DispatchCacheBytes();
#ifdef _WIN32
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status)) throw std::runtime_error("dispatch cache: cannot read the physical memory size");
        const std::uint64_t physical = status.ullTotalPhys;
#else
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long page = sysconf(_SC_PAGESIZE);
        if (pages <= 0 || page <= 0) throw std::runtime_error("dispatch cache: cannot read the physical memory size");
        const std::uint64_t physical = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
#endif
        return std::min<std::uint64_t>(DispatchCacheBytes(), physical / 32);
    }();
    return budget;
}

Driver::DispatchCacheFootprint Driver::DispatchCacheSize() {
    std::lock_guard lock(dispatchCacheMutex);
    return {dispatchCache.Size(), dispatchCache.Bytes()};
}

std::uint64_t DispatchVariantBytes(const DispatchVariant& variant) {
    return sizeof(DispatchVariant) + variant.words.capacity() * sizeof(std::uint32_t) + variant.shared.capacity() * sizeof(SharedWords) + variant.runs.capacity() * sizeof(std::pair<std::uint64_t, std::uint64_t>) + (variant.dataPositions.capacity() + variant.dataSlots.capacity()) * sizeof(std::uint32_t) + variant.captured.capacity() * sizeof(ShaderRecompiler::MemoryRegion) + variant.spans.capacity() * sizeof(std::pair<std::uint64_t, std::size_t>) + (variant.vertexInfo != nullptr ? sizeof(ShaderRecompiler::ShaderVertexStageInfo) : 0);
}

DispatchEntries::DispatchEntries(std::size_t entryLimit, std::uint64_t byteBudget) : entryLimit(entryLimit), byteBudget(byteBudget) {}

std::shared_ptr<DispatchEntry> DispatchEntries::Find(std::uint64_t key) const {
    const auto found = entries.find(key);
    return found != entries.end() ? found->second : nullptr;
}

bool DispatchEntries::Admits(const DispatchVariant& variant) const {
    return DispatchVariantBytes(variant) <= byteBudget;
}

void DispatchEntries::Insert(std::uint64_t key, std::shared_ptr<DispatchVariant> variant, std::uint64_t touched) {
    auto created = std::make_shared<DispatchEntry>();
    created->touched = touched;
    account(*variant, true);
    created->variants.push_back(std::move(variant));
    order.push_front(key);
    created->order = order.begin();
    entries.emplace(key, std::move(created));
}

std::uint64_t DispatchEntries::Prepend(std::uint64_t key, std::shared_ptr<DispatchVariant> variant, std::size_t variantLimit, std::uint64_t touched) {
    auto& slot = entries.at(key);
    auto replacement = std::make_shared<DispatchEntry>();
    replacement->variants.reserve(variantLimit);
    account(*variant, true);
    replacement->variants.push_back(std::move(variant));
    std::uint64_t dropped = 0;
    for (const auto& kept : slot->variants) {
        if (replacement->variants.size() < variantLimit) {
            replacement->variants.push_back(kept);
        } else {
            account(*kept, false);
            ++dropped;
        }
    }
    replacement->touched = touched;
    replacement->order = slot->order;
    order.splice(order.begin(), order, replacement->order);
    slot = std::move(replacement);
    return dropped;
}

void DispatchEntries::Promote(std::uint64_t key, std::size_t rank) {
    auto& slot = entries.at(key);
    const auto& variants = slot->variants;
    auto rotated = std::make_shared<DispatchEntry>();
    rotated->variants.reserve(variants.size());
    rotated->variants.push_back(variants.at(rank));
    for (std::size_t i = 0; i < variants.size(); ++i) {
        if (i != rank) rotated->variants.push_back(variants[i]);
    }
    rotated->touched = slot->touched;
    rotated->order = slot->order;
    slot = std::move(rotated);
}

bool DispatchEntries::Touch(std::uint64_t key, std::uint64_t now, std::uint64_t interval) {
    const auto& entry = entries.at(key);
    if (now - entry->touched <= interval) return false;
    order.splice(order.begin(), order, entry->order);
    entry->touched = now;
    return true;
}

std::uint64_t DispatchEntries::Trim(std::uint64_t keep) {
    std::uint64_t evicted = 0;
    while (OverLimits()) {
        auto last = std::prev(order.end());
        if (*last == keep) {
            if (last == order.begin()) break;
            last = std::prev(last);
        }
        erase(entries.find(*last));
        ++evicted;
    }
    if (bytes > byteBudget) {
        if (const auto kept = entries.find(keep); kept != entries.end() && kept->second->variants.size() > 1) {
            auto newest = std::make_shared<DispatchEntry>();
            newest->variants.push_back(kept->second->variants.front());
            for (std::size_t i = 1; i < kept->second->variants.size(); ++i) account(*kept->second->variants[i], false);
            newest->touched = kept->second->touched;
            newest->order = kept->second->order;
            kept->second = std::move(newest);
        }
    }
    return evicted;
}

std::uint64_t DispatchEntries::Clear() {
    const auto cleared = static_cast<std::uint64_t>(entries.size());
    entries.clear();
    order.clear();
    variants = 0;
    bytes = 0;
    return cleared;
}

void DispatchEntries::account(const DispatchVariant& variant, bool added) {
    if (added) {
        ++variants;
        bytes += DispatchVariantBytes(variant);
    } else {
        --variants;
        bytes -= DispatchVariantBytes(variant);
    }
}

void DispatchEntries::erase(std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>>::iterator it) {
    for (const auto& variant : it->second->variants) account(*variant, false);
    order.erase(it->second->order);
    entries.erase(it);
}

namespace {

std::uint64_t ValueSetHash(const DispatchVariant& variant) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
    };
    mix(variant.runs.size());
    for (const auto& [begin, end] : variant.runs) {
        mix(begin);
        mix(end);
    }
    mix(variant.words.size());
    for (const auto word : variant.words) mix(word);
    return hash;
}

}

PriorValueSets::PriorValueSets(std::size_t keyLimit) : keyLimit(keyLimit) {}

bool PriorValueSets::Matches(std::uint64_t key, const DispatchVariant& variant) const {
    const auto found = rings.find(key);
    return found != rings.end() && std::find(found->second.begin(), found->second.end(), ValueSetHash(variant)) != found->second.end();
}

void PriorValueSets::Remember(std::uint64_t key, const DispatchVariant& variant) {
    auto& ring = rings[key];
    ring.push_front(ValueSetHash(variant));
    while (ring.size() > 3) ring.pop_back();
    if (rings.size() > keyLimit) rings.clear();
}

}
