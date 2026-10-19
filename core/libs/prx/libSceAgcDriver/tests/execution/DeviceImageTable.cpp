#include "prx/libSceAgcDriver/Graphics/include/DeviceImageTable.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BindlessHeap.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "DeviceImageTableAbi.hpp"
#include "ImageTableAbi.hpp"
#include "RuntimeAbi.hpp"
#include "Recompiler.hpp"
#include "Triangle_frag_spv.h"
#include "Triangle_mesh_spv.h"
#include "Triangle_vert_spv.h"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;
namespace Abi = ShaderRecompiler::DeviceImageTableAbi;
namespace Runtime = ShaderRecompiler::RuntimeAbi;
using Key = DeviceImageTable::Key;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Side = 4;
constexpr std::uint32_t Type2D = 9;
constexpr std::array<std::uint32_t, 5> Formats{1u, 13u, 14u, 22u, 56u};
constexpr std::uint64_t InterfaceId = 0x1234'5678'9abc'def0ull;

alignas(256) std::array<std::array<std::uint32_t, Side * Side>, Formats.size()> Textures{};

std::array<std::uint32_t, 8> TextureDescriptor(const void* data, std::uint32_t format) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

std::shared_ptr<const DeviceImageTable::Interface> SampledInterface() {
    static const auto interface = [] {
        auto made = std::make_shared<DeviceImageTable::Interface>();
        made->id = InterfaceId;
        made->image.resourceClass = ShaderRecompiler::ImageResourceClass::Sampled;
        made->image.numericClass = ShaderRecompiler::IrTextureNumericClass::Float;
        made->image.dimension = ShaderRecompiler::RdnaImageDimension::Dim2D;
        made->image.read = true;
        made->image.table = 0;
        made->modes = ShaderRecompiler::ResourceMaterializer::RuntimeImageModes(made->image);
        return made;
    }();
    return interface;
}

DeviceImageTable::InterfaceLookup Lookup() {
    return [](std::uint64_t id) { return id == InterfaceId ? SampledInterface() : ShaderRecompiler::ResourceMaterializer::FindDeviceImageInterface(id); };
}

Key KeyFor(const std::array<std::uint32_t, 8>& descriptor, std::uint32_t samplerClass = 0u) {
    Key key{};
    std::copy(descriptor.begin(), descriptor.end(), key.begin());
    key[Abi::InterfaceWord] = static_cast<std::uint32_t>(InterfaceId);
    key[Abi::InterfaceWord + 1u] = static_cast<std::uint32_t>(InterfaceId >> 32u);
    key[Abi::SamplerClassWord] = samplerClass;
    return key;
}

void WriteRecords(std::span<std::uint32_t> ring, std::span<const Key> keys, std::uint32_t count, std::uint32_t pcFlags = 0u) {
    ring[Abi::RingCountWord] = count;
    for (std::uint32_t record = 0; record < std::min(count, DeviceImageTable::RingCapacity); ++record) {
        auto* at = ring.data() + Abi::RingRecordsWord + static_cast<std::size_t>(record) * Abi::RecordWords;
        const auto& key = keys[record % keys.size()];
        std::copy(key.begin(), key.end(), at);
        at[Abi::RecordPcWord] = (0x100u + record * 4u) | pcFlags;
    }
}

std::string HexWords(std::span<const std::uint32_t> words) {
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < words.size(); ++index) text << (index == 0 ? "" : " ") << std::setw(8) << words[index];
    return text.str();
}

std::vector<std::uint32_t> ReadTable(const Context& context, Recorder& recorder, const DeviceImageTable& table, std::size_t words) {
    Buffer readback(context, words * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy region{0, 0, words * sizeof(std::uint32_t)};
    context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, table.TableBuffer(), readback.Handle(), 1, &region);
    recorder.Sync();
    std::vector<std::uint32_t> result(words);
    std::memcpy(result.data(), readback.Bytes().data(), words * sizeof(std::uint32_t));
    return result;
}

std::vector<Key> FormatKeys() {
    std::vector<Key> keys;
    for (std::size_t index = 0; index < Formats.size(); ++index) keys.push_back(KeyFor(TextureDescriptor(Textures[index].data(), Formats[index])));
    return keys;
}

ShaderRecompiler::RuntimeImageModeMatch ExpectedMode(const Key& key) {
    ShaderRecompiler::DescriptorValue descriptor;
    descriptor.dwordCount = Abi::DescriptorWords;
    descriptor.availableDwordMask = 0xffu;
    std::copy_n(key.begin(), Abi::DescriptorWords, descriptor.dwords.begin());
    const auto interface = SampledInterface();
    return ShaderRecompiler::ResourceMaterializer::TryRuntimeImageMode(interface->image, descriptor, interface->modes);
}

void ResolvesAndUploads(AgcDriver::VulkanDevice& device) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "the device has an active recorder");
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup());
    const auto keys = FormatKeys();
    table.Prepare(context, *recorder);
    WriteRecords(table.RingChunk(*recorder), keys, 8u);
    recorder->Sync();
    auto counts = table.Totals();
    Require(counts.drained == 8u && counts.dropped == 0u, "the batch's eight ring records are drained when it completes");
    Require(counts.inserted == 0u, "nothing is inserted before the next prepare");
    table.Prepare(context, *recorder);
    counts = table.Totals();
    Require(counts.distinct == keys.size() && counts.inserted == keys.size() && counts.entries == keys.size(), "the five distinct records are resolved and inserted (distinct " + std::to_string(counts.distinct) + ", inserted " + std::to_string(counts.inserted) + ")");
    auto& heap = BindlessHeap::For(context);
    std::set<std::pair<std::uint32_t, std::uint32_t>> slots;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto value = table.Find(keys[index]);
        Require(value.has_value() && value->fault == 0u && value->samplerSlot == 0u, "format " + std::to_string(Formats[index]) + " is inserted without a fault");
        const auto expected = ExpectedMode(keys[index]);
        Require(expected.mode.has_value() && value->mode == *expected.mode, "format " + std::to_string(Formats[index]) + " names the mode the CPU table path picks");
        const auto kind = static_cast<std::uint32_t>(ShaderRecompiler::DescriptorBindingForImage(SampledInterface()->modes.at(value->mode)));
        Require(heap.SlotTexture(kind, value->slot) != nullptr, "format " + std::to_string(Formats[index]) + " holds a texture in its heap slot");
        Require(slots.insert({kind, value->slot}).second, "every format has its own heap slot");
    }
    const auto words = table.KeyWords();
    const auto uploaded = ReadTable(context, *recorder, table, Abi::HeaderWords + words.size());
    Require(uploaded[Abi::HeaderVersion] == Abi::Version && uploaded[Abi::HeaderMask] == counts.buckets - 1u && uploaded[Abi::HeaderRingCapacity] == DeviceImageTable::RingCapacity, "the header names the version, the bucket mask and the ring capacity");
    Require(uploaded[Abi::HeaderTableLow] == 0u && uploaded[Abi::HeaderTableHigh] == 0u && uploaded[Abi::HeaderRingLow] == 0u && uploaded[Abi::HeaderRingHigh] == 0u, "the header's address words stay zero");
    Require(std::equal(words.begin(), words.end(), uploaded.begin() + Abi::HeaderWords), "the device's buckets equal the host key table");
    const auto uploads = table.Totals().uploads;
    table.Prepare(context, *recorder);
    Require(table.Totals().uploads == uploads, "a prepare with nothing changed uploads nothing");
    recorder->Sync();
    std::printf("resolved %zu formats: %llu uploads, %llu bytes\n", keys.size(), static_cast<unsigned long long>(table.Totals().uploads), static_cast<unsigned long long>(table.Totals().uploadBytes));
}

void ResolvesRegisteredInterfaces(AgcDriver::VulkanDevice& device) {
    using ShaderRecompiler::ResourceMaterializer;
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    auto* owned = DeviceImageTable::Existing(context);
    Require(owned != nullptr, "a device serving the path owns a device image table");
    ShaderRecompiler::ShaderInfo info;
    auto image = SampledInterface()->image;
    image.deviceTable = true;
    info.images.push_back(image);
    info.runtimeImageModes.push_back(ResourceMaterializer::RuntimeImageModes(image));
    const auto id = ResourceMaterializer::DeviceImageInterfaceId(image, info.runtimeImageModes[0]);
    auto key = KeyFor(TextureDescriptor(Textures[4].data(), Formats[4]));
    key[Abi::InterfaceWord] = static_cast<std::uint32_t>(id);
    key[Abi::InterfaceWord + 1u] = static_cast<std::uint32_t>(id >> 32u);
    try {
        owned->Prepare(context, *recorder);
        WriteRecords(owned->RingChunk(*recorder), std::span<const Key>(&key, 1u), 1u);
        recorder->Sync();
        owned->Prepare(context, *recorder);
        Require(false, "the device's table resolved an interface no program registered");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("which no compiled program registered") != std::string::npos, std::string("an unregistered interface is refused: ") + error.what());
    }
    recorder->Sync();
    ResourceMaterializer::RegisterDeviceImageInterfaces(info);
    Require(ResourceMaterializer::FindDeviceImageInterface(id) != nullptr, "the recompiler's registry holds the interface");
    const auto before = owned->Totals();
    owned->Prepare(context, *recorder);
    WriteRecords(owned->RingChunk(*recorder), std::span<const Key>(&key, 1u), 1u);
    recorder->Sync();
    owned->Prepare(context, *recorder);
    const auto value = owned->Find(key);
    Require(owned->Totals().inserted == before.inserted + 1u && value.has_value() && value->fault == 0u, "the device's own table resolves a key through the interface the recompiler registered");
    const auto expected = ExpectedMode(key);
    Require(expected.mode.has_value() && value->mode == *expected.mode, "the registered interface gives the mode the CPU table path picks");
    recorder->Sync();
}

void RejectsAndFaults(AgcDriver::VulkanDevice& device) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup());
    auto invalid = TextureDescriptor(Textures[0].data(), Formats[0]);
    invalid[1] &= ~(0x1ffu << 20u);
    auto unmapped = TextureDescriptor(reinterpret_cast<const void*>(std::uintptr_t{0x10000}), Formats[4]);
    const std::array<Key, 2> keys{KeyFor(invalid), KeyFor(unmapped)};
    const auto checked = ExpectedMode(keys[0]);
    Require(!checked.mode.has_value(), "the CPU table path rejects a descriptor of format 0");
    table.Prepare(context, *recorder);
    WriteRecords(table.RingChunk(*recorder), keys, 2u);
    recorder->Sync();
    table.Prepare(context, *recorder);
    const auto first = table.Find(keys[0]);
    Require(first.has_value() && first->fault == 1u + static_cast<std::uint32_t>(checked.reason), "a descriptor the mode check rejects is inserted with 1 + its reason");
    const auto second = table.Find(keys[1]);
    Require(second.has_value() && second->fault == 1u + static_cast<std::uint32_t>(ShaderRecompiler::ImageTableAbi::PoisonReason::DriverRejected), "a descriptor over unmapped memory is inserted as rejected by the driver");
    Require(table.Totals().rejected == 2u && table.Totals().inserted == 0u, "both are rejections, neither holds a texture");
    WriteRecords(table.RingChunk(*recorder), std::span<const Key>(keys).first(1), 1u, Abi::FaultFlag);
    recorder->Sync();
    const auto fault = table.LastFault();
    const auto name = ShaderRecompiler::ImageTableAbi::PoisonReasonName(checked.reason);
    Require(table.Totals().faults == 1u, "the fault record is counted");
    Require(fault.find("image table: pc 0x100 selected the descriptor (words " + HexWords(std::span<const std::uint32_t>(invalid)) + ")") != std::string::npos && fault.find(std::string(name)) != std::string::npos, "the fault names the pc, the descriptor's words and the reason: " + fault);
    std::printf("fault: %s\n", fault.c_str());
}

constexpr std::size_t BlockBytes = std::size_t{4} << 20u;
constexpr std::size_t TextureStride = 1024u;
constexpr std::uint32_t WatchedTextures = 2000u;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    std::uint8_t* Texture(std::uint32_t index) const { return block + static_cast<std::size_t>(index) * TextureStride; }

private:
    std::uint8_t* block = nullptr;
};

double Microseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}

void RefreshesWrittenEntries(AgcDriver::VulkanDevice& device) {
    GuestBlock block;
    for (std::uint32_t index = 0; index < WatchedTextures; ++index) std::memset(block.Texture(index), static_cast<int>(index & 0xffu), Side * Side * 4u);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    AgcDriver::GuestMemory::BumpCollectEpoch();
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::puts("guest memory has no write watch: device image entry refreshes are not tested");
        return;
    }
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup());
    std::vector<Key> keys;
    for (std::uint32_t index = 0; index < WatchedTextures; ++index) keys.push_back(KeyFor(TextureDescriptor(block.Texture(index), Formats[4])));
    table.Prepare(context, *recorder);
    WriteRecords(table.RingChunk(*recorder), keys, WatchedTextures);
    recorder->Sync();
    auto start = std::chrono::steady_clock::now();
    table.Prepare(context, *recorder);
    const auto insertUs = Microseconds(start);
    auto counts = table.Totals();
    Require(counts.inserted == WatchedTextures && counts.entries == WatchedTextures, "the 2000 entries are inserted (" + std::to_string(counts.inserted) + ")");
    const auto insertBytes = counts.uploadBytes;
    recorder->Sync();
    table.Prepare(context, *recorder);
    start = std::chrono::steady_clock::now();
    constexpr int Prepares = 100;
    for (int round = 0; round < Prepares; ++round) table.Prepare(context, *recorder);
    const auto sameEpochUs = Microseconds(start) / Prepares;
    Require(table.Totals().refreshed == 0u, "nothing is refreshed while nothing was written");
    double newEpochUs = 0;
    for (int round = 0; round < Prepares; ++round) {
        AgcDriver::GuestMemory::BumpCollectEpoch();
        start = std::chrono::steady_clock::now();
        table.Prepare(context, *recorder);
        newEpochUs += Microseconds(start);
    }
    newEpochUs /= Prepares;
    counts = table.Totals();
    Require(counts.refreshed == 0u, "a new collect epoch with nothing written refreshes nothing (" + std::to_string(counts.refreshed) + ")");
    const auto uploads = counts.uploads;
    const auto written = block.Texture(777u);
    std::memset(written, 0x5a, Side * Side * 4u);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    start = std::chrono::steady_clock::now();
    table.Prepare(context, *recorder);
    const auto refreshUs = Microseconds(start);
    counts = table.Totals();
    Require(counts.refreshed == 1u, "a CPU write to one texture refreshes exactly its entry (" + std::to_string(counts.refreshed) + " refreshed)");
    const auto value = table.Find(keys[777]);
    Require(value.has_value() && value->fault == 0u, "the refreshed entry stays in the table");
    recorder->Sync();
    std::printf("2000 entries: insert %.1f us per key (%.1f bytes uploaded per insert); prepare with nothing changed %.2f us in the same epoch, %.1f us in a new epoch (%llu scans); one written texture refreshed in %.1f us (%llu entries of its 64 KiB blocks checked), %llu uploads after it\n",
        insertUs / WatchedTextures, static_cast<double>(insertBytes) / WatchedTextures, sameEpochUs, newEpochUs, static_cast<unsigned long long>(counts.scans), refreshUs, static_cast<unsigned long long>(counts.checked), static_cast<unsigned long long>(counts.uploads - uploads));
}

void EvictsLeastRecentlyUsed(AgcDriver::VulkanDevice& device) {
    std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup(), DeviceImageTable::Budget{4u, std::uint64_t{1} << 40u, 0u, 0u});
    const auto keys = FormatKeys();
    table.Prepare(context, *recorder);
    WriteRecords(table.RingChunk(*recorder), std::span<const Key>(keys).first(4), 4u);
    recorder->Sync();
    table.Prepare(context, *recorder);
    Require(table.Totals().entries == 4u && table.Totals().evicted == 0u, "four entries fit the budget");
    auto oldest = table.EntryTexture(keys[0]);
    Require(oldest != nullptr, "the oldest entry holds its texture");
    const auto held = oldest.use_count();
    WriteRecords(table.RingChunk(*recorder), std::span<const Key>(keys).subspan(4), 1u);
    recorder->Sync();
    table.Prepare(context, *recorder);
    const auto counts = table.Totals();
    Require(counts.entries == 3u && counts.evicted == 2u, "a fifth entry over a budget of four evicts down to three (" + std::to_string(counts.entries) + " left, " + std::to_string(counts.evicted) + " evicted)");
    Require(!table.Find(keys[0]).has_value() && !table.Find(keys[1]).has_value(), "the two least recently used entries are evicted");
    Require(table.Find(keys[2]).has_value() && table.Find(keys[3]).has_value() && table.Find(keys[4]).has_value(), "the recently used entries stay");
    Require(oldest.use_count() == held, "an evicted entry's texture is kept by the open batch");
    recorder->Sync();
    recorder->FlushDeferredReleases();
    gpu.unlock();
    for (int wait = 0; wait < 5000 && oldest.use_count() >= held; ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(oldest.use_count() == held - 1, "the evicted texture is released once its batch completed (" + std::to_string(oldest.use_count()) + " of " + std::to_string(held) + ")");
}

void CountsOverflow(AgcDriver::VulkanDevice& device) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup());
    const auto keys = FormatKeys();
    table.Prepare(context, *recorder);
    WriteRecords(table.RingChunk(*recorder), keys, DeviceImageTable::RingCapacity + 100u);
    recorder->Sync();
    const auto counts = table.Totals();
    Require(counts.drained == DeviceImageTable::RingCapacity && counts.dropped == 100u, "records past the ring's capacity are counted as dropped (drained " + std::to_string(counts.drained) + ", dropped " + std::to_string(counts.dropped) + ")");
    table.Prepare(context, *recorder);
    Require(table.Totals().inserted == keys.size(), "the records that fit are still resolved");
    recorder->Sync();
}

alignas(256) std::array<std::uint32_t, 64> CopyInput{};
alignas(256) std::array<std::uint32_t, 64> CopyOutput{};
alignas(256) constexpr std::array<std::uint32_t, 6> CopyCode{0x34040082u, 0xe0000000u, 0x80000100u, 0xe0701000u, 0x80010102u, 0xbf810000u};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

ShaderRecompiler::RecompileResult CopyProgram(AgcDriver::VulkanDevice& device, bool deviceTables) {
    std::array<std::uint32_t, 8> userData{};
    auto input = BufferDescriptor(CopyInput.data(), static_cast<std::uint32_t>(CopyInput.size()));
    input[1] |= 4u << 16u;
    input[3] = 0x11800000u | (20u << 12u) | 4u;
    const auto output = BufferDescriptor(CopyOutput.data(), static_cast<std::uint32_t>(CopyOutput.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{{ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(CopyCode.data()), CopyCode, 0, {}}, {64u, 0u, userData, compute, std::nullopt, std::nullopt, {}}, device.ComputeTarget(64u), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    auto result = ShaderRecompiler::Recompile(request);
    result.deviceImageTables = deviceTables;
    return result;
}

void RunCopy(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result, const std::string& what) {
    for (std::uint32_t index = 0; index < CopyInput.size(); ++index) CopyInput[index] = 0x5a000000u + index * 0x10203u;
    CopyOutput.fill(0xdeadbeefu);
    device.Dispatch(result, 1, 1, 1);
    device.WaitIdle();
    for (std::uint32_t index = 0; index < CopyOutput.size(); ++index) Require(CopyOutput[index] == CopyInput[index], what + " copies its buffer (lane " + std::to_string(index) + ")");
}

void ComputeLayout(AgcDriver::VulkanDevice& device) {
    auto* table = DeviceImageTable::Existing(device.GraphicsContext());
    Require(table != nullptr, "a device serving the path owns a device image table");
    const auto result = CopyProgram(device, true);
    const auto before = table->Totals();
    RunCopy(device, result, "a dispatch whose pipeline layout carries sets 2 and 3");
    const auto counts = table->Totals();
    Require(counts.prepares == before.prepares + 1u, "the dispatch prepared the device image table once");
    Require(counts.drained == before.drained && counts.faults == before.faults, "a dispatch that records nothing drains nothing");
}

ShaderRecompiler::RecompileResult DeviceTableProgram(AgcDriver::VulkanDevice& device) {
    std::array<std::uint32_t, 8> userData{};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{{ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(CopyCode.data()), CopyCode, 0, {}}, {64u, 0u, userData, compute, std::nullopt, std::nullopt, {}}, device.ComputeTarget(64u), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    auto result = ShaderRecompiler::Recompile(request);
    result.deviceImageTables = true;
    return result;
}

constexpr std::uint64_t ProgramAddress = 0x7700'0000ull;

void OverflowFallsBack(AgcDriver::VulkanDevice& device) {
    const auto program = DeviceTableProgram(device);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    auto* owned = DeviceImageTable::Existing(context);
    Require(owned != nullptr, "a device serving the path owns a device image table");
    auto& table = *owned;
    table.SetInterfaceLookup(Lookup());
    device.NoteDeviceImageProgram(program, ProgramAddress);
    Require(device.DeviceImagesServe(ProgramAddress), "a noted program is served by the device path");
    const std::array<ShaderRecompiler::PipelineIdentity, 1> programs{program.Identity()};
    const auto keys = FormatKeys();
    table.Prepare(context, *recorder, programs);
    WriteRecords(table.RingChunk(*recorder), keys, static_cast<std::uint32_t>(keys.size()));
    recorder->Sync();
    for (std::uint32_t batch = 1; batch <= DeviceImageTable::OverflowBatches; ++batch) {
        Require(table.ServesProgram(ProgramAddress), "the program keeps the device path before " + std::to_string(DeviceImageTable::OverflowBatches) + " overflowing batches");
        table.Prepare(context, *recorder, programs);
        WriteRecords(table.RingChunk(*recorder), keys, DeviceImageTable::RingCapacity + 1u);
        recorder->Sync();
    }
    Require(!table.ServesProgram(ProgramAddress) && !table.ServesIdentity(programs[0]) && table.Totals().fallbacks == 1u, "a program whose misses overflow the ring in three batches in a row after the table resolved its misses falls back to the CPU path");
    Require(!device.DeviceImagesServe(ProgramAddress) && device.DeviceImagesServe(ProgramAddress + 0x100u), "the device's next recompile request for that program, and only for it, asks for the CPU table path");
    Require(device.Target().deviceImageTables, "the device still serves the path to other programs");
    DeviceImageTable other(context, Lookup());
    other.NoteProgram(program, ProgramAddress);
    constexpr std::uint32_t Overflow = DeviceImageTable::RingCapacity + 1u;
    for (const auto count : {1u, Overflow, Overflow, 1u, Overflow, Overflow}) {
        other.Prepare(context, *recorder, programs);
        WriteRecords(other.RingChunk(*recorder), keys, count);
        recorder->Sync();
    }
    Require(other.ServesProgram(ProgramAddress) && other.OverflowStreak(programs[0]) == 2u, "a batch without overflow between them resets the count (streak " + std::to_string(other.OverflowStreak(programs[0])) + ")");
}

void OverflowsBeforeAResolveCountOnce(AgcDriver::VulkanDevice& device) {
    const auto program = DeviceTableProgram(device);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup());
    table.NoteProgram(program, ProgramAddress);
    const std::array<ShaderRecompiler::PipelineIdentity, 1> programs{program.Identity()};
    const auto keys = FormatKeys();
    for (std::uint32_t batch = 0; batch < DeviceImageTable::OverflowBatches + 1u; ++batch) {
        static_cast<void>(recorder->Commands());
        table.Prepare(context, *recorder, programs);
        WriteRecords(table.RingChunk(*recorder), keys, DeviceImageTable::RingCapacity + 1u);
        recorder->Submit();
    }
    Require(table.Totals().drained == 0u, "the overflowing batches are still in flight");
    recorder->Sync();
    const auto drained = table.Totals();
    Require(drained.dropped == DeviceImageTable::OverflowBatches + 1u, "every batch overflowed the ring (dropped " + std::to_string(drained.dropped) + ")");
    Require(table.ServesProgram(ProgramAddress) && drained.fallbacks == 0u, "batches that overflow before the table resolved the misses of the first count as one overflow");
    table.Prepare(context, *recorder, programs);
    Require(table.Totals().inserted == keys.size(), "the records that fit are resolved");
    recorder->Sync();
}

void OverflowsBeforeTheirInsertsDoNotCount(AgcDriver::VulkanDevice& device) {
    const auto program = DeviceTableProgram(device);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup());
    table.NoteProgram(program, ProgramAddress);
    const std::array<ShaderRecompiler::PipelineIdentity, 1> programs{program.Identity()};
    const auto keys = FormatKeys();
    std::vector<std::uint64_t> serials;
    for (std::uint32_t batch = 0; batch < DeviceImageTable::OverflowBatches + 2u; ++batch) {
        static_cast<void>(recorder->Commands());
        table.Prepare(context, *recorder, programs);
        WriteRecords(table.RingChunk(*recorder), keys, DeviceImageTable::RingCapacity + 1u);
        serials.push_back(recorder->SubmitAndEpoch());
    }
    Require(table.Totals().drained == 0u, "the overflowing batches are still in flight");
    for (const auto serial : serials) {
        recorder->FinishUpTo(serial);
        static_cast<void>(recorder->Commands());
        table.Prepare(context, *recorder, programs);
    }
    const auto drained = table.Totals();
    Require(drained.dropped == serials.size() && drained.inserted == keys.size(), "every batch overflowed the ring and the first batch's records were resolved before the others drained (dropped " + std::to_string(drained.dropped) + ", inserted " + std::to_string(drained.inserted) + ")");
    const auto streak = table.OverflowStreak(programs[0]);
    Require(table.ServesProgram(ProgramAddress) && drained.fallbacks == 0u && streak == 0u, "batches taken before the inserts that resolved their misses neither count toward the overflow streak nor make the program fall back (streak " + std::to_string(streak) + ", " + std::to_string(drained.fallbacks) + " fallbacks)");
    recorder->Sync();
}

void WarmOverflowFallsBack(AgcDriver::VulkanDevice& device) {
    const auto program = DeviceTableProgram(device);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup());
    table.NoteProgram(program, ProgramAddress);
    const std::array<ShaderRecompiler::PipelineIdentity, 1> programs{program.Identity()};
    const auto warm = FormatKeys();
    table.Prepare(context, *recorder, programs);
    WriteRecords(table.RingChunk(*recorder), warm, static_cast<std::uint32_t>(warm.size()));
    recorder->Sync();
    std::vector<Key> keys(DeviceImageTable::RingCapacity + 1u, warm[0]);
    for (std::uint32_t batch = 0; batch < DeviceImageTable::OverflowBatches; ++batch) {
        Require(table.ServesProgram(ProgramAddress), "the program keeps the device path before " + std::to_string(DeviceImageTable::OverflowBatches) + " overflowing batches");
        for (std::uint32_t index = 0; index < keys.size(); ++index) keys[index][Abi::SamplerWord] = batch * static_cast<std::uint32_t>(keys.size()) + index + 1u;
        table.Prepare(context, *recorder, programs);
        WriteRecords(table.RingChunk(*recorder), keys, static_cast<std::uint32_t>(keys.size()));
        recorder->Sync();
    }
    const auto counts = table.Totals();
    const auto resolved = warm.size() + (DeviceImageTable::OverflowBatches - 1u) * DeviceImageTable::RingCapacity;
    Require(counts.inserted == resolved && counts.dropped == DeviceImageTable::OverflowBatches, "the table resolved the warm keys and each batch's recorded misses before the next batch was taken (" + std::to_string(counts.inserted) + " inserted, " + std::to_string(counts.dropped) + " dropped)");
    Require(!table.ServesProgram(ProgramAddress) && counts.fallbacks == 1u, "a program that misses more distinct keys than the ring holds in three batches in a row, each taken after the previous batch's misses were inserted, falls back to the CPU path");
    const auto reason = "its misses overflowed the ring of " + std::to_string(DeviceImageTable::RingCapacity) + " records in " + std::to_string(DeviceImageTable::OverflowBatches) + " batches in a row";
    Require(table.LastFallback() == reason, "the fallback gives the overflow reason: " + table.LastFallback());
}

void FullTableFallsBack(AgcDriver::VulkanDevice& device) {
    const auto program = DeviceTableProgram(device);
    GuestBlock block;
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    DeviceImageTable table(context, Lookup(), DeviceImageTable::Budget{1u << 20u, std::uint64_t{1} << 40u, 0u, 0u, 16u});
    table.NoteProgram(program, ProgramAddress);
    const std::array<ShaderRecompiler::PipelineIdentity, 1> programs{program.Identity()};
    std::vector<Key> keys;
    for (std::uint32_t index = 0; index < 12u; ++index) keys.push_back(KeyFor(TextureDescriptor(block.Texture(index), Formats[4])));
    table.Prepare(context, *recorder, programs);
    WriteRecords(table.RingChunk(*recorder), keys, static_cast<std::uint32_t>(keys.size()));
    recorder->Sync();
    table.Prepare(context, *recorder, programs);
    const auto counts = table.Totals();
    Require(counts.buckets == 16u && counts.entries < keys.size() && counts.tableFull != 0u, "a table of at most 16 buckets fills (" + std::to_string(counts.entries) + " entries)");
    Require(!table.ServesProgram(ProgramAddress) && counts.fallbacks == 1u, "a program whose key finds the table full falls back to the CPU path");
    recorder->Sync();
}

void HeapKeptFree(AgcDriver::VulkanDevice& device, std::uint32_t textures) {
    const auto program = DeviceTableProgram(device);
    GuestBlock block;
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto context = device.GraphicsContext();
    auto& heap = BindlessHeap::For(context);
    const auto cap = heap.TextureSlotCap();
    const auto margin = heap.TextureSlotMargin();
    const auto keep = std::max(margin, GpuHeapEnabled() ? cap - cap / 2u : 0u);
    const std::uint64_t room = cap - keep;
    DeviceImageTable table(context, Lookup());
    table.NoteProgram(program, ProgramAddress);
    const std::array<ShaderRecompiler::PipelineIdentity, 1> programs{program.Identity()};
    std::vector<Key> keys;
    for (std::uint32_t index = 0; index < textures; ++index) keys.push_back(KeyFor(TextureDescriptor(block.Texture(index), Formats[4])));
    Require(!BindlessHeap::NearlyFull(), "the heap starts open to GPU mode");
    table.Prepare(context, *recorder, programs);
    WriteRecords(table.RingChunk(*recorder), keys, static_cast<std::uint32_t>(keys.size()));
    recorder->Sync();
    table.Prepare(context, *recorder, programs);
    const auto counts = table.Totals();
    Require(counts.inserted == room && counts.heapFull == keys.size() - room, "a heap of " + std::to_string(cap) + " slots per kind with a margin of " + std::to_string(margin) + " gives the table " + std::to_string(room) + " of " + std::to_string(keys.size()) + " textures (" + std::to_string(counts.inserted) + " inserted, " + std::to_string(counts.heapFull) + " without a slot)");
    Require(!BindlessHeap::NearlyFull(), "the table's slots leave the heap's margin free, so GPU mode is not refused");
    Require(!table.ServesProgram(ProgramAddress) && counts.fallbacks == 1u, "the program whose keys found no slot the table may take falls back to the CPU path instead of stopping");
    Require(device.Target().deviceImageTables, "the device still serves the path to other programs");
    std::printf("heap of %u slots per kind, margin %u%s: the table took %llu and left %u unused, %u above the margin; %llu keys found no slot it may take, 1 fallback; GPU mode not refused\n", cap, margin, GpuHeapEnabled() ? ", APS5_GPU_HEAP set" : "", static_cast<unsigned long long>(room), keep, keep - margin, static_cast<unsigned long long>(keys.size() - room));
    const auto refused = std::find_if(keys.begin(), keys.end(), [&](const Key& key) { return !table.Find(key).has_value(); });
    Require(refused != keys.end(), "a key the heap refused stays out of the table");
    const auto mode = ExpectedMode(*refused).mode;
    Require(mode.has_value(), "the refused key has a runtime image mode");
    const auto kind = static_cast<std::uint32_t>(ShaderRecompiler::DescriptorBindingForImage(SampledInterface()->modes.at(*mode)));
    const auto texture = LookupDeviceTableTexture(context, std::span<const std::uint32_t, Abi::DescriptorWords>(refused->data(), Abi::DescriptorWords), DeviceTableView{}, 0u);
    Require(texture.texture != nullptr && texture.unservable.empty(), "the refused key's texture is in the cache");
    Require(heap.TextureSlot(kind, texture.texture, texture.view, texture.layout).has_value(), "GPU mode's own request for a refused key's texture still finds a slot");
    std::printf("GPU mode's request for that texture takes a slot; the heap is %s\n", BindlessHeap::NearlyFull() ? "now refused by GPU mode's own take" : "still open");
    recorder->Sync();
}

alignas(4096) std::array<std::uint8_t, 65536> CubeTexels{};

void NonSeamlessCube(AgcDriver::VulkanDevice& device) {
    constexpr std::uint32_t TypeCube = 11;
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto context = device.GraphicsContext();
    auto cube = TextureDescriptor(CubeTexels.data(), Formats[4]);
    cube[3] = 0xfacu | (TypeCube << 28u);
    cube[4] = 5u;
    const DeviceTableView view{ShaderRecompiler::DescriptorImageShape::Image2DArray, false, false};
    const auto seamless = LookupDeviceTableTexture(context, cube, view, 0u);
    Require(seamless.texture != nullptr && seamless.unservable.empty(), "a cube T# resolves through the texture cache");
    try {
        const auto resolved = LookupDeviceTableTexture(context, cube, view, Abi::SamplerNonSeamlessCube);
        Require(context.nonSeamlessCubeMap && resolved.texture != nullptr, "a non-seamless cube sampler is served only by a device with VK_EXT_non_seamless_cube_map");
    } catch (const std::runtime_error& error) {
        Require(!context.nonSeamlessCubeMap && std::string(error.what()).find("needs VK_EXT_non_seamless_cube_map") != std::string::npos, std::string("a non-seamless cube sampler is rejected only without VK_EXT_non_seamless_cube_map: ") + error.what());
    }
    std::printf("non-seamless cube sampler: %s (VK_EXT_non_seamless_cube_map %s)\n", context.nonSeamlessCubeMap ? "served" : "rejected", context.nonSeamlessCubeMap ? "on" : "off");
}

void GraphicsLayouts(AgcDriver::VulkanDevice& device) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto deviceContext = device.GraphicsContext();
    struct Cleanup {
        VkDevice device;
        ~Cleanup() { ClearCachedPipelines(device); }
    } cleanup{deviceContext.device};
    auto* table = DeviceImageTable::Existing(deviceContext);
    Require(table != nullptr, "a device serving the path owns a device image table");
    ShaderRecompiler::RecompileResult vertex, fragment;
    vertex.variantId = 0xd1u;
    fragment.variantId = 0xd2u;
    vertex.spirv = std::vector<std::uint32_t>(std::begin(TRIANGLE_vert_SPV), std::end(TRIANGLE_vert_SPV));
    fragment.spirv = std::vector<std::uint32_t>(std::begin(TRIANGLE_frag_SPV), std::end(TRIANGLE_frag_SPV));
    fragment.deviceImageTables = true;
    const std::array shaders{CompiledShader{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, CompiledShader{ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}};
    State state{};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.hasColorTarget = true;
    state.color.format = VK_FORMAT_R8G8B8A8_UNORM;
    state.colors.push_back(state.color);
    state.blend.colorWriteMask = 15;
    state.blends.push_back(state.blend);
    std::vector<bool> librariesTried;
    for (const bool libraries : {deviceContext.graphicsPipelineLibrary, false}) {
        if (std::find(librariesTried.begin(), librariesTried.end(), libraries) != librariesTried.end()) continue;
        librariesTried.push_back(libraries);
        auto context = deviceContext;
        context.graphicsPipelineLibrary = libraries;
        const auto described = DescribeSetLayouts(context, shaders);
        Require(described.layouts.deviceHeap == BindlessHeap::For(context).HeapLayout() && described.layouts.deviceTable == table->Layout() && described.layouts.deviceGap == table->EmptyLayout(), "a stage reading images through the device table describes sets 2 and 3");
        const auto view = described.View();
        Require(view.deviceTable == table->Layout() && view.deviceHeap == described.layouts.deviceHeap, "the described layouts' view carries the device sets");
        bool created = false;
        const auto pipeline = CachedPipeline(context, state, VertexInputLayout{}, view, shaders, VK_IMAGE_LAYOUT_GENERAL, &created);
        Require(pipeline != nullptr && pipeline->Layout() != VK_NULL_HANDLE, std::string("a graphics pipeline with sets 2 and 3 is created ") + (libraries ? "from pipeline libraries" : "without pipeline libraries"));
        std::printf("graphics pipeline with sets 2 and 3 %s pipeline libraries (split sets %d): created\n", libraries ? "from" : "without", described.layouts.split ? 1 : 0);
    }
    vertex.deviceImageTables = false;
    fragment.deviceImageTables = false;
    const auto plain = DescribeSetLayouts(deviceContext, shaders);
    Require(plain.layouts.deviceTable == VK_NULL_HANDLE && plain.layouts.deviceHeap == VK_NULL_HANDLE, "stages without device tables describe no device sets");
}

void MeshLayouts(AgcDriver::VulkanDevice& device) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto deviceContext = device.GraphicsContext();
    struct Cleanup {
        VkDevice device;
        ~Cleanup() { ClearCachedPipelines(device); }
    } cleanup{deviceContext.device};
    constexpr VkShaderStageFlags heapStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    const auto stages = AgcDriver::Graphics::ImageHeapStages(deviceContext);
    Require((stages & heapStages) == heapStages && (stages & ~(heapStages | VK_SHADER_STAGE_MESH_BIT_EXT)) == 0u, "the heap and table sets are visible to vertex, pixel and compute stages, and to no stage but mesh besides");
    Require(((stages & VK_SHADER_STAGE_MESH_BIT_EXT) != 0u) == deviceContext.meshShader, "the heap and table sets are visible to mesh stages exactly when the device enables VK_EXT_mesh_shader");
    if (!deviceContext.meshShader) {
        std::printf("mesh pipeline with sets 2 and 3: not tested, the device has no VK_EXT_mesh_shader\n");
        return;
    }
    auto* table = DeviceImageTable::Existing(deviceContext);
    Require(table != nullptr, "a device serving the path owns a device image table");
    ShaderRecompiler::RecompileResult mesh, fragment;
    mesh.variantId = 0xd3u;
    fragment.variantId = 0xd4u;
    mesh.spirv = std::vector<std::uint32_t>(std::begin(TRIANGLE_mesh_SPV), std::end(TRIANGLE_mesh_SPV));
    fragment.spirv = std::vector<std::uint32_t>(std::begin(TRIANGLE_frag_SPV), std::end(TRIANGLE_frag_SPV));
    mesh.deviceImageTables = true;
    const std::array shaders{CompiledShader{ShaderRecompiler::ShaderStage::Mesh, &mesh, 0}, CompiledShader{ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}};
    State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Geometry, 0x20u, 64u, 64u, ShaderRecompiler::MeshConfiguration{4u, 1u, 3u, 3u, 1u, 64u, 128u, 0u, 4u}, std::nullopt};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.hasColorTarget = true;
    state.color.format = VK_FORMAT_R8G8B8A8_UNORM;
    state.colors.push_back(state.color);
    state.blend.colorWriteMask = 15;
    state.blends.push_back(state.blend);
    std::vector<bool> librariesTried;
    for (const bool libraries : {deviceContext.graphicsPipelineLibrary, false}) {
        if (std::find(librariesTried.begin(), librariesTried.end(), libraries) != librariesTried.end()) continue;
        librariesTried.push_back(libraries);
        auto context = deviceContext;
        context.graphicsPipelineLibrary = libraries;
        const auto described = DescribeSetLayouts(context, shaders);
        Require(described.layouts.deviceHeap == BindlessHeap::For(context).HeapLayout() && described.layouts.deviceTable == table->Layout() && described.layouts.deviceGap == table->EmptyLayout(), "a mesh stage reading images through the device table describes sets 2 and 3");
        const auto view = described.View();
        Require(view.deviceTable == table->Layout() && view.deviceHeap == described.layouts.deviceHeap, "the mesh pipeline's described layouts carry the device sets");
        bool created = false;
        const auto pipeline = CachedPipeline(context, state, VertexInputLayout{}, view, shaders, VK_IMAGE_LAYOUT_GENERAL, &created);
        Require(pipeline != nullptr && pipeline->Layout() != VK_NULL_HANDLE, std::string("a mesh pipeline with sets 2 and 3 is created ") + (libraries ? "from pipeline libraries" : "without pipeline libraries"));
        std::printf("mesh pipeline with sets 2 and 3 %s pipeline libraries (split sets %d): created, heap and table stages 0x%x\n", libraries ? "from" : "without", described.layouts.split ? 1 : 0, static_cast<unsigned>(stages));
    }
}

VkPhysicalDeviceLimits UsableLimits() {
    VkPhysicalDeviceLimits limits{};
    limits.maxBoundDescriptorSets = 8;
    limits.maxStorageBufferRange = 1u << 30u;
    return limits;
}

DeviceImagePathFeatures UsableFeatures() {
    return {true, true, 1u << 20u, 1u << 12u};
}

void DevicePathLimits() {
    Require(!DeviceImagePathRefusal(UsableLimits(), UsableFeatures()).has_value(), "a device with four sets, the heap's features and its descriptor counts serves the device path");
    auto oneSet = UsableLimits();
    oneSet.maxBoundDescriptorSets = 1;
    const auto refused = DeviceImagePathRefusal(oneSet, UsableFeatures());
    Require(refused.has_value() && refused->find("binds 1 descriptor sets") != std::string::npos, "a device that binds one descriptor set keeps the device path off");
    auto threeSets = UsableLimits();
    threeSets.maxBoundDescriptorSets = Runtime::DeviceTableSet;
    Require(DeviceImagePathRefusal(threeSets, UsableFeatures()).has_value(), "a device that binds three descriptor sets keeps the device path off");
    auto features = UsableFeatures();
    features.heapFeatures = false;
    Require(DeviceImagePathRefusal(UsableLimits(), features).has_value(), "a device without the heap's update-after-bind features keeps the device path off");
    features = UsableFeatures();
    features.nonUniformIndexing = false;
    Require(DeviceImagePathRefusal(UsableLimits(), features).has_value(), "a device without non-uniform sampled image indexing keeps the device path off");
    features = UsableFeatures();
    features.sampledImages = Runtime::GpuImageHeapSlots;
    Require(DeviceImagePathRefusal(UsableLimits(), features).has_value(), "a device that cannot bind the heap's sampled images keeps the device path off");
    features = UsableFeatures();
    features.samplers = Runtime::GpuSamplerHeapSlots - 1u;
    Require(DeviceImagePathRefusal(UsableLimits(), features).has_value(), "a device that cannot bind the heap's samplers keeps the device path off");
    auto smallRange = UsableLimits();
    smallRange.maxStorageBufferRange = 1u << 20u;
    Require(DeviceImagePathRefusal(smallRange, UsableFeatures()).has_value(), "a device whose storage buffer range cannot hold the bucket buffer keeps the device path off");
}

void DeviceTarget(AgcDriver::VulkanDevice& device) {
    const auto target = device.Target();
    Require(target.deviceImageTables == device.DeviceImagePath(), "the recompile target asks for device image tables exactly when the device serves the path");
    Require(device.ComputeTarget(64u).deviceImageTables == device.DeviceImagePath(), "the compute target asks for device image tables exactly when the device serves the path");
    const auto& limits = device.GraphicsContext().limits;
    if (limits.maxBoundDescriptorSets < Runtime::DeviceTableSet + 1u) Require(!device.DeviceImagePath(), "a device binding fewer than four sets keeps the device path off");
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void ClearEnvironment(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

bool TrianglePipeline(AgcDriver::VulkanDevice& device, bool deviceTables) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto context = device.GraphicsContext();
    struct Cleanup {
        VkDevice device;
        ~Cleanup() { ClearCachedPipelines(device); }
    } cleanup{context.device};
    ShaderRecompiler::RecompileResult vertex, fragment;
    vertex.variantId = 0xd5u;
    fragment.variantId = 0xd6u;
    vertex.spirv = std::vector<std::uint32_t>(std::begin(TRIANGLE_vert_SPV), std::end(TRIANGLE_vert_SPV));
    fragment.spirv = std::vector<std::uint32_t>(std::begin(TRIANGLE_frag_SPV), std::end(TRIANGLE_frag_SPV));
    fragment.deviceImageTables = deviceTables;
    const std::array shaders{CompiledShader{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, CompiledShader{ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}};
    State state{};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.hasColorTarget = true;
    state.color.format = VK_FORMAT_R8G8B8A8_UNORM;
    state.colors.push_back(state.color);
    state.blend.colorWriteMask = 15;
    state.blends.push_back(state.blend);
    const auto described = DescribeSetLayouts(context, shaders);
    bool created = false;
    const auto pipeline = CachedPipeline(context, state, VertexInputLayout{}, described.View(), shaders, VK_IMAGE_LAYOUT_GENERAL, &created);
    Require(pipeline != nullptr && pipeline->Layout() != VK_NULL_HANDLE, std::string("the triangle pipeline ") + (deviceTables ? "with" : "without") + " the device sets exists");
    return created;
}

void JournalRecord(AgcDriver::VulkanDevice& device) {
    Require(PipelineJournalEnabled(), "the pipeline journal is on");
    RunCopy(device, CopyProgram(device, true), "the recorded dispatch through sets 2 and 3");
    Require(TrianglePipeline(device, true), "the recorded graphics pipeline with sets 2 and 3 is created");
    FlushPipelineJournal();
    const auto totals = PipelineJournalTotals();
    Require(totals.recordedCompute >= 1u && totals.recordedGraphics >= 1u, "the journal recorded the compute and the graphics pipeline with the device sets");
    std::printf("journal recorded with APS5_GPU_PATH set: %llu compute and %llu graphics pipelines, %llu modules\n", static_cast<unsigned long long>(totals.recordedCompute), static_cast<unsigned long long>(totals.recordedGraphics), static_cast<unsigned long long>(totals.recordedModules));
}

void JournalReplay(AgcDriver::VulkanDevice& device, bool switchOn) {
    Require(PipelineJournalEnabled(), "the pipeline journal is on");
    device.WaitForPipelineJournalReplay();
    const auto totals = PipelineJournalTotals();
    std::printf("journal replayed with APS5_GPU_PATH %s: %llu pipelines loaded, %llu compute and %llu graphics built, %llu present, %llu rejected, %llu failed\n", switchOn ? "set" : "unset", static_cast<unsigned long long>(totals.loadedRecipes), static_cast<unsigned long long>(totals.replayedCompute), static_cast<unsigned long long>(totals.replayedGraphics), static_cast<unsigned long long>(totals.replayPresent), static_cast<unsigned long long>(totals.replayRejected), static_cast<unsigned long long>(totals.replayFailed));
    Require(totals.loadedRecipes >= 1u, "the cold start loaded the recorded journal");
    const auto created = device.ComputePipelinesCreated();
    if (switchOn) {
        Require(totals.replayedCompute >= 1u && totals.replayFailed == 0u, "with APS5_GPU_PATH set the replay builds the compute pipeline with the device sets");
        RunCopy(device, CopyProgram(device, true), "the dispatch after the replay, through sets 2 and 3");
        Require(device.ComputePipelinesCreated() == created, "the dispatch after the replay builds no pipeline: it uses the one the replay built");
        Require(totals.replayedGraphics >= 1u && !TrianglePipeline(device, true), "with APS5_GPU_PATH set the replay builds the graphics pipeline with the device sets, and a later use finds it");
        std::printf("the dispatch and the draw pipeline used the replayed pipelines\n");
        return;
    }
    Require(DeviceImageTable::Existing(device.GraphicsContext()) == nullptr && !device.ComputeTarget(64u).deviceImageTables, "with APS5_GPU_PATH unset the device has no device image table");
    Require(totals.replayFailed == 0u && totals.replayRejected >= 2u, "with APS5_GPU_PATH unset the replay rejects the pipelines whose stages read the device image table instead of failing them");
    RunCopy(device, CopyProgram(device, false), "the dispatch with APS5_GPU_PATH unset");
    Require(device.ComputePipelinesCreated() == created + 1u, "the dispatch with APS5_GPU_PATH unset builds its pipeline when it first needs it");
    Require(TrianglePipeline(device, false), "the graphics pipeline with APS5_GPU_PATH unset is built when it is first needed");
    std::printf("the dispatch and the draw pipeline were built when first needed\n");
}

int JournalMode(const std::string& mode, const std::filesystem::path& directory) {
    const bool switchOn = mode != "--journal-replay-off";
    if (mode == "--journal-record") {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::filesystem::create_directories(directory);
    SetEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
    SetEnvironment("ANYPS5_SHADER_CACHE_DIR", directory.string());
    SetEnvironment("APS5_NO_PIPELINE_JOURNAL", "0");
    SetEnvironment("APS5_PIPELINE_JOURNAL_THREADS", "2");
    if (switchOn) SetEnvironment("APS5_GPU_PATH", "1");
    else ClearEnvironment("APS5_GPU_PATH");
    Require(GpuPathEnabled() == switchOn, "the journal run sets APS5_GPU_PATH as its mode asks");
    auto device = OpenVulkanTestDevice();
    if (device == nullptr) return VulkanTestSkipped;
    if (switchOn && !device->DeviceImagePath()) {
        std::printf("skipped, the device does not serve the device image path\n");
        return VulkanTestSkipped;
    }
    if (mode == "--journal-record") JournalRecord(*device);
    else if (mode == "--journal-replay" || mode == "--journal-replay-off") JournalReplay(*device, switchOn);
    else throw std::runtime_error("unknown journal mode " + mode);
    device->WaitIdle();
    device.reset();
    std::cout << "device image journal tests passed\n";
    return 0;
}

void HeapStagesFollowTheSwitch(AgcDriver::VulkanDevice& device) {
    const auto stages = ImageHeapStages(device.GraphicsContext());
    constexpr VkShaderStageFlags graphics = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!GpuPathEnabled()) {
        Require(stages == graphics, "with APS5_GPU_PATH unset the image heap set keeps the vertex and fragment stages alone");
        return;
    }
    Require((stages & graphics) == graphics && (stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0, "with APS5_GPU_PATH set the image heap set is visible to compute stages, which read device-table images through it");
}

}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode.rfind("--journal-", 0) == 0) {
            Require(argc > 2, "a journal mode takes the journal's directory");
            return JournalMode(mode, argv[2]);
        }
        if (mode == "--switch-off") {
#ifdef _WIN32
            _putenv_s("APS5_GPU_PATH", "");
#else
            unsetenv("APS5_GPU_PATH");
#endif
            Require(!GpuPathEnabled(), "the switch-off run keeps APS5_GPU_PATH unset");
            auto device = OpenVulkanTestDevice();
            if (device == nullptr) return VulkanTestSkipped;
            HeapStagesFollowTheSwitch(*device);
            device->WaitIdle();
            device.reset();
            std::cout << "device image table switch-off tests passed\n";
            return 0;
        }
        const bool heapCap = mode == "--heap-cap";
        const bool heapMargin = argc > 2 && mode == "--heap-margin";
        const std::string cap = heapMargin ? argv[2] : "2";
#ifdef _WIN32
        _putenv_s("APS5_GPU_PATH", "1");
        if (heapCap || heapMargin) _putenv_s("APS5_HEAP_SLOT_CAP", cap.c_str());
#else
        setenv("APS5_GPU_PATH", "1", 1);
        if (heapCap || heapMargin) setenv("APS5_HEAP_SLOT_CAP", cap.c_str(), 1);
#endif
        Require(GpuPathEnabled(), "APS5_GPU_PATH turns the device path on");
        DevicePathLimits();
        for (std::size_t index = 0; index < Textures.size(); ++index) {
            for (std::size_t texel = 0; texel < Textures[index].size(); ++texel) Textures[index][texel] = static_cast<std::uint32_t>(0x01020304u * (index + 1u) + texel);
        }
        auto device = OpenVulkanTestDevice();
        if (device == nullptr) return VulkanTestSkipped;
        DeviceTarget(*device);
        HeapStagesFollowTheSwitch(*device);
        std::printf("device path on %s: %s\n", device->DeviceName().c_str(), device->DeviceImagePath() ? "on" : "off");
        if (!device->DeviceImagePath()) {
            std::printf("skipped, the device does not serve the device image path\n");
            return VulkanTestSkipped;
        }
        if (heapCap || heapMargin) {
            HeapKeptFree(*device, heapCap ? static_cast<std::uint32_t>(Formats.size()) : static_cast<std::uint32_t>(std::stoul(cap)) + 32u);
            device->WaitIdle();
            device.reset();
            std::cout << "device image table heap cap tests passed\n";
            return 0;
        }
        ResolvesRegisteredInterfaces(*device);
        ResolvesAndUploads(*device);
        RejectsAndFaults(*device);
        CountsOverflow(*device);
        RefreshesWrittenEntries(*device);
        EvictsLeastRecentlyUsed(*device);
        FullTableFallsBack(*device);
        OverflowFallsBack(*device);
        OverflowsBeforeAResolveCountOnce(*device);
        OverflowsBeforeTheirInsertsDoNotCount(*device);
        WarmOverflowFallsBack(*device);
        NonSeamlessCube(*device);
        ComputeLayout(*device);
        GraphicsLayouts(*device);
        MeshLayouts(*device);
        device->WaitIdle();
        device.reset();
        std::cout << "device image table tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "device image table tests failed: " << error.what() << '\n';
        return 1;
    }
}
