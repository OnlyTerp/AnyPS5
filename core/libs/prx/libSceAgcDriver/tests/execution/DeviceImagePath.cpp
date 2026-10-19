#include "prx/libSceAgcDriver/Execution/include/AsyncLog.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/HeapStages.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BindlessHeap.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DeviceImageTable.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "RuntimeAbi.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::DeviceImageTable;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Entries = 2000;
constexpr std::uint32_t RecordBytes = 32;
constexpr std::uint32_t Side = 4;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Textures = 16;
constexpr std::size_t BlockBytes = std::size_t{8} << 20u;
constexpr std::size_t TableBlockBytes = std::size_t{2} << 20u;
constexpr std::size_t TextureStride = 0x1000;
constexpr std::size_t TableOffset = 0x100000;
constexpr std::size_t SrtOffset = 0x180000;
constexpr std::size_t ColorOffset = 0x200000;
constexpr std::uint32_t ColorWidth = 64;
constexpr std::uint32_t ColorHeight = 4;
constexpr std::uint32_t RenderTargetAttrib3 = (static_cast<std::uint32_t>(AgcDriver::Graphics::ColorTileMode::RenderTarget) << 14u) | (1u << 24u) | (1u << 27u);
constexpr std::uint32_t LinearAttrib3 = (static_cast<std::uint32_t>(AgcDriver::Graphics::ColorTileMode::Linear) << 14u) | (1u << 24u) | (1u << 27u);
constexpr std::uint32_t UnrecordedKey = 1503u;
constexpr std::uint32_t VillageRecords = 32768;
constexpr std::uint32_t VillageStride = 0x30;
constexpr std::uint32_t VillageAddend = 0x10;
constexpr std::uint32_t VillageTextures = 256;
constexpr std::uint32_t VillageDraws = 850;
constexpr std::uint32_t VillageFrames = 20;
constexpr std::uint32_t VillageWarmup = 5;
constexpr std::uint32_t VillageTargets = 32;
constexpr std::uint32_t VillageTargetKey = 32000;
constexpr std::size_t VillageColorOffset = 0x100000;
constexpr std::size_t VillageTargetOffset = 0x300000;
constexpr std::uint32_t NullEntry = 1000;
constexpr std::uint32_t InvalidEntry = 1001;
constexpr std::uint32_t TargetEntry = 1990;
constexpr std::size_t TargetOffset = 0x300000;
constexpr std::size_t LargeColorOffset = 0x400000;
constexpr std::uint32_t LargeSide = 512;
constexpr std::uint32_t FreshEntry = 1900;
constexpr std::uint32_t FreshTextures = 16;
constexpr std::uint32_t StreamedKeys = 4;
constexpr std::array<std::uint32_t, 5> Formats{1u, 13u, 14u, 22u, 56u};
constexpr std::array<const char*, 5> FormatNames{"8 unorm", "16 float", "8_8 unorm", "32 float", "8_8_8_8 unorm"};
constexpr std::array<std::uint16_t, 4> HalfValues{0x3266u, 0x38cdu, 0x3666u, 0x3a66u};
constexpr std::array<float, 4> FloatValues{0.2f, 0.6f, 0.4f, 0.8f};
constexpr std::array<std::uint8_t, 4> WrittenTexel{0x55u, 0x66u, 0x77u, 0x88u};

alignas(256) constexpr std::array<std::uint32_t, 18> ComputeCode{
    0x7e0002f0u, 0x7e0202f0u, 0xf4080200u, 0xfa000000u, 0xf4080300u, 0xfa000010u, 0xbf8cc07fu, 0x8f108502u, 0xf42c0504u, 0x20000000u,
    0xbf8cc07fu, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 18> OtherComputeCode{
    0x7e0002f2u, 0x7e0202f0u, 0xf4080200u, 0xfa000000u, 0xf4080300u, 0xfa000010u, 0xbf8cc07fu, 0x8f108502u, 0xf42c0504u, 0x20000000u,
    0xbf8cc07fu, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 18> ExecClearCode{
    0x7e0002f0u, 0x7e0202f0u, 0xf4080200u, 0xfa000000u, 0xf4080300u, 0xfa000010u, 0xbf8cc07fu, 0x8f108502u, 0xf42c0504u, 0x20000000u,
    0xbf8cc07fu, 0xbefe0380u, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 21> LaneKeyComputeCode{
    0x7e220500u, 0x90119f11u, 0x7e0002f0u, 0x7e0202f0u, 0xf4080200u, 0xfa000000u, 0xf4080300u, 0xfa000010u, 0xbf8cc07fu, 0x80110211u, 0x8f108511u,
    0xf42c0504u, 0x20000000u, 0xbf8cc07fu, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 16> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 14> PixelCode{
    0x8f108508u, 0xf42c0500u, 0x20000000u, 0xbf8cc07fu, 0x7e0002f0u, 0x7e0202f0u, 0xf09c0f08u, 0x00250000u, 0xbf8c0000u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 12> DirectPixelCode{0x7e0002f0u, 0x7e0202f0u, 0xf09c0f08u, 0x00400000u, 0xbf8c0000u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 14> LargePixelCode{
    0x8f108508u, 0xf42c0500u, 0x20000000u, 0xbf8cc07fu, 0x7e0002f2u, 0x7e0202f0u, 0xf09c0f08u, 0x00250000u, 0xbf8c0000u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 18> LaneKeyPixelCode{
    0x7e040f00u, 0x7e220502u, 0x90118111u, 0x80110811u, 0x8f108511u, 0xf42c0500u, 0x20000000u, 0xbf8cc07fu, 0x7e0002f0u,
    0x7e0202f0u, 0xf09c0f08u, 0x00250000u, 0xbf8c0000u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 13> VillagePixelCode{
    0x9310b008u, 0xf42c0500u, 0x20000010u, 0xbf8cc07fu, 0x7e0002f0u, 0x7e0202f0u, 0xf09c0f08u, 0x00250000u, 0xbf8c0000u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> ProducerPixelCode{0x7e0002f0u, 0x7e0202f2u, 0x7e040280u, 0x7e0602f2u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> OtherProducerPixelCode{0x7e0002f2u, 0x7e0202f0u, 0x7e040280u, 0x7e0602f2u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u};

alignas(4096) std::array<std::array<float, 4>, 64> Outputs{};
alignas(16) std::array<std::uint32_t, 4> DrawArguments{3u, 1u, 0u, 0u};
alignas(16) std::array<std::uint32_t, 5> IndexedArguments{3u, 1u, 0u, 0u, 0u};
alignas(256) std::array<std::uint16_t, 4> Indices{0u, 1u, 2u, 0u};
constexpr std::byte Kept{0x40};

class GuestBlock {
public:
    explicit GuestBlock(std::size_t size = BlockBytes) : bytes(size) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        }
#endif
        Require(block != nullptr, "cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
        munmap(block, bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Texture(std::uint32_t index) const { return block + static_cast<std::size_t>(index) * TextureStride; }
    std::uint32_t* Table() const { return reinterpret_cast<std::uint32_t*>(block + TableOffset); }
    std::uint32_t* Srt() const { return reinterpret_cast<std::uint32_t*>(block + SrtOffset); }
    std::uint8_t* Color() const { return block + ColorOffset; }
    std::uint8_t* At(std::size_t offset) const { return block + offset; }

private:
    std::size_t bytes = 0;
    std::uint8_t* block = nullptr;
};

class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        file = std::tmpfile();
        Require(file != nullptr, "cannot open a temporary file for stderr");
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        _dup2(_fileno(file), _fileno(stderr));
#else
        saved = dup(fileno(stderr));
        dup2(fileno(file), fileno(stderr));
#endif
    }

    std::string Finish() {
        AgcDriver::FlushAsyncLog_nid_no_patch();
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        std::rewind(file);
        std::string text;
        char buffer[4096];
        for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) text.append(buffer, read);
        std::fclose(file);
        std::fwrite(text.data(), 1, text.size(), stderr);
        return text;
    }

private:
    std::FILE* file = nullptr;
    int saved = -1;
};

float HalfToFloat(std::uint16_t half) {
    const auto exponent = static_cast<int>((half >> 10u) & 0x1fu);
    const auto mantissa = static_cast<float>(half & 0x3ffu);
    return std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15);
}

struct Texel {
    std::uint32_t format = 0;
    std::vector<std::uint8_t> bytes;
    std::array<float, 4> value{};
};

Texel TexelOf(std::uint32_t texture) {
    const auto kind = texture % static_cast<std::uint32_t>(Formats.size());
    const auto variant = texture / static_cast<std::uint32_t>(Formats.size());
    Texel texel;
    texel.format = Formats[kind];
    const auto unorm = [](std::uint8_t byte) { return static_cast<float>(byte) / 255.0f; };
    switch (kind) {
        case 0: {
            const auto red = static_cast<std::uint8_t>(0x40u + 0x30u * variant);
            texel.bytes = {red};
            texel.value = {unorm(red), 0.0f, 0.0f, 1.0f};
            break;
        }
        case 1: {
            const auto half = HalfValues[variant];
            texel.bytes = {static_cast<std::uint8_t>(half), static_cast<std::uint8_t>(half >> 8u)};
            texel.value = {HalfToFloat(half), 0.0f, 0.0f, 1.0f};
            break;
        }
        case 2: {
            const auto red = static_cast<std::uint8_t>(0x11u + 0x22u * variant);
            const auto green = static_cast<std::uint8_t>(0x99u - 0x11u * variant);
            texel.bytes = {red, green};
            texel.value = {unorm(red), unorm(green), 0.0f, 1.0f};
            break;
        }
        case 3: {
            const auto bits = std::bit_cast<std::uint32_t>(FloatValues[variant]);
            texel.bytes = {static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8u), static_cast<std::uint8_t>(bits >> 16u), static_cast<std::uint8_t>(bits >> 24u)};
            texel.value = {FloatValues[variant], 0.0f, 0.0f, 1.0f};
            break;
        }
        default: {
            const std::array<std::uint8_t, 4> rgba{static_cast<std::uint8_t>(0x10u + 0x30u * variant), static_cast<std::uint8_t>(0x80u - 0x20u * variant), static_cast<std::uint8_t>(0xc0u - 0x30u * variant), static_cast<std::uint8_t>(0xffu - 0x40u * variant)};
            texel.bytes.assign(rgba.begin(), rgba.end());
            texel.value = {unorm(rgba[0]), unorm(rgba[1]), unorm(rgba[2]), unorm(rgba[3])};
            break;
        }
    }
    return texel;
}

void FillTexture(std::uint8_t* memory, std::span<const std::uint8_t> bytes) {
    for (std::size_t offset = 0; offset + bytes.size() <= TextureStride; offset += bytes.size()) std::memcpy(memory + offset, bytes.data(), bytes.size());
}

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

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::uint32_t TextureOf(std::uint32_t entry) {
    return entry % Textures;
}

std::array<std::uint32_t, 8> TargetDescriptor(const void* data);

std::array<std::uint8_t, 4> FreshTexel(std::uint32_t fresh) {
    return {static_cast<std::uint8_t>(0x20u + 8u * fresh), static_cast<std::uint8_t>(0xe0u - 8u * fresh), static_cast<std::uint8_t>(0x40u + 4u * fresh), 0xffu};
}

std::array<float, 4> FreshValue(std::uint32_t entry) {
    const auto texel = FreshTexel(entry - FreshEntry);
    return {texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f, texel[3] / 255.0f};
}

void FillFresh(const GuestBlock& block) {
    for (std::uint32_t fresh = 0; fresh < FreshTextures; ++fresh) {
        auto* texture = block.Texture(Textures + fresh);
        FillTexture(texture, FreshTexel(fresh));
        const auto descriptor = TextureDescriptor(texture, Formats[4]);
        std::copy(descriptor.begin(), descriptor.end(), block.Table() + static_cast<std::size_t>(FreshEntry + fresh) * (RecordBytes / 4u));
    }
}

void FillGuest(const GuestBlock& block) {
    for (std::uint32_t texture = 0; texture < Textures; ++texture) FillTexture(block.Texture(texture), TexelOf(texture).bytes);
    auto* table = block.Table();
    for (std::uint32_t entry = 0; entry < Entries; ++entry) {
        const auto texture = TextureOf(entry);
        const auto descriptor = TextureDescriptor(block.Texture(texture), TexelOf(texture).format);
        std::copy(descriptor.begin(), descriptor.end(), table + static_cast<std::size_t>(entry) * (RecordBytes / 4u));
    }
    std::fill_n(table + static_cast<std::size_t>(NullEntry) * (RecordBytes / 4u), RecordBytes / 4u, 0u);
    auto invalid = TextureDescriptor(block.Texture(0), 0u);
    std::copy(invalid.begin(), invalid.end(), table + static_cast<std::size_t>(InvalidEntry) * (RecordBytes / 4u));
    const auto target = TargetDescriptor(block.At(TargetOffset));
    std::copy(target.begin(), target.end(), table + static_cast<std::size_t>(TargetEntry) * (RecordBytes / 4u));
    auto* srt = block.Srt();
    const auto tableWords = BufferDescriptor(table, Entries * RecordBytes);
    std::copy(tableWords.begin(), tableWords.end(), srt);
    std::fill_n(srt + 4, 4, 0u);
}

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header ComputeHeader{};
alignas(8) Header OtherComputeHeader{};
alignas(8) Header ExecClearHeader{};
alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header DirectPixelHeader{};
alignas(8) Header LargePixelHeader{};
alignas(8) Header VillagePixelHeader{};
alignas(8) Header LaneKeyComputeHeader{};
alignas(8) Header LaneKeyPixelHeader{};
alignas(8) Header ProducerHeader{};
alignas(8) Header OtherProducerHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void Dispatch(std::vector<std::uint32_t>& words, const GuestBlock& block, const std::uint32_t* code, std::uint32_t key, std::array<float, 4>& output, std::uint32_t threads = 1) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    const auto srt = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block.Srt()));
    const auto set = [&](std::uint32_t offset, std::uint32_t value) { Append(words, 0x76, {offset, value}); };
    set(0x20c, static_cast<std::uint32_t>(address >> 8u));
    set(0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    set(0x207, threads);
    for (const std::uint32_t offset : {0x208u, 0x209u}) set(offset, 1);
    set(0x212, 0);
    set(0x213, 16u << 1u);
    std::array<std::uint32_t, 16> userData{};
    userData[0] = static_cast<std::uint32_t>(srt);
    userData[1] = static_cast<std::uint32_t>(srt >> 32u);
    userData[2] = key;
    const auto outputWords = BufferDescriptor(output.data(), sizeof(output));
    std::copy(outputWords.begin(), outputWords.end(), userData.begin() + 4);
    for (std::uint32_t index = 0; index < userData.size(); ++index) set(0x240u + index, userData[index]);
    Append(words, 0x15, {1, 1, 1, 0x8041u});
}

void SubmitCompute(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "the compute submission failed");
    AgcDriverWaitIdle_nid_postfix();
    std::vector<std::byte> stored(sizeof(Outputs));
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Outputs.data())), stored, 4);
    std::memcpy(Outputs.data(), stored.data(), stored.size());
}

enum class DrawKind { Direct, Indirect, IndexedIndirect };

const char* DrawName(DrawKind kind) {
    return kind == DrawKind::Direct ? "direct draw" : kind == DrawKind::Indirect ? "DRAW_INDIRECT" : "DRAW_INDEX_INDIRECT";
}

std::vector<std::uint32_t> DrawCommands(const GuestBlock& block, DrawKind kind, const std::uint32_t* pixelCode, std::span<const std::uint32_t> userData) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const auto color = reinterpret_cast<std::uintptr_t>(block.Color());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, RenderTargetAttrib3}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)}, {0x292, 2},
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, static_cast<std::uint32_t>(userData.size()) << 1u},
    };
    for (std::uint32_t index = 0; index < userData.size(); ++index) shader.emplace_back(0x00cu + index, userData[index]);
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    if (kind == DrawKind::Direct) {
        Append(commands, 0x2d, {3, 2});
        return commands;
    }
    const auto arguments = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(kind == DrawKind::Indirect ? static_cast<const void*>(DrawArguments.data()) : static_cast<const void*>(IndexedArguments.data())));
    Append(commands, 0x11, {1, static_cast<std::uint32_t>(arguments), static_cast<std::uint32_t>(arguments >> 32u)});
    if (kind == DrawKind::Indirect) {
        Append(commands, 0x24, {0, 0, 0, 2});
        return commands;
    }
    const auto indices = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Indices.data()));
    Append(commands, 0x2a, {0});
    Append(commands, 0x26, {static_cast<std::uint32_t>(indices), static_cast<std::uint32_t>(indices >> 32u)});
    Append(commands, 0x13, {3});
    Append(commands, 0x25, {0, 0, 0, 0});
    return commands;
}

std::vector<std::uint32_t> DrawCommands(const GuestBlock& block, std::uint32_t key, DrawKind kind) {
    std::array<std::uint32_t, 9> userData{};
    const auto table = BufferDescriptor(block.Table(), Entries * RecordBytes);
    std::copy(table.begin(), table.end(), userData.begin());
    userData[8] = key;
    return DrawCommands(block, kind, PixelCode.data(), userData);
}

struct Counters {
    std::uint64_t snapshots = 0;
    std::uint64_t tableBuilds = 0;
    DeviceImageTable::Counts device;
};

Counters Read() {
    Counters counters;
    counters.snapshots = ShaderRecompiler::ResourceMaterializer::TableSnapshotColumns();
    counters.tableBuilds = AgcDriver::Graphics::ImageTableBuilds();
    for (const auto& totals : DeviceImageTable::DeviceTotals()) {
        counters.device.prepares += totals.prepares;
        counters.device.drained += totals.drained;
        counters.device.dropped += totals.dropped;
        counters.device.distinct += totals.distinct;
        counters.device.inserted += totals.inserted;
        counters.device.faults += totals.faults;
        counters.device.rejected += totals.rejected;
        counters.device.refreshed += totals.refreshed;
        counters.device.checked += totals.checked;
        counters.device.entries += totals.entries;
        counters.device.heapFull += totals.heapFull;
        counters.device.fallbacks += totals.fallbacks;
        counters.device.expectations += totals.expectations;
        counters.device.expectationHits += totals.expectationHits;
        counters.device.expectationNs += totals.expectationNs;
    }
    return counters;
}

std::string Describe(const Counters& before, const Counters& after) {
    return " (CPU table snapshots " + std::to_string(after.snapshots - before.snapshots) + ", CPU table bindings " + std::to_string(after.tableBuilds - before.tableBuilds) + ", device table prepares " + std::to_string(after.device.prepares - before.device.prepares) + ", miss records " + std::to_string(after.device.drained - before.device.drained) + ", inserted " + std::to_string(after.device.inserted - before.device.inserted) + ")";
}

void RequireCpuTable(const Counters& before, const Counters& after, const std::string& what) {
    Require(after.snapshots > before.snapshots && after.device.prepares == before.device.prepares, what + ": the CPU table path did not snapshot the table" + Describe(before, after));
}

void RequireMisses(const Counters& before, const Counters& after, std::uint64_t keys, const std::string& what) {
    Require(after.snapshots == before.snapshots && after.tableBuilds == before.tableBuilds && after.device.prepares > before.device.prepares, what + ": the device path snapshotted or bound the CPU table, or did not use the device table" + Describe(before, after));
    Require(after.device.drained >= before.device.drained + keys, what + ": the device path did not record its " + std::to_string(keys) + " missed keys" + Describe(before, after));
}

void RequireHits(const Counters& before, const Counters& after, const std::string& what) {
    Require(after.snapshots == before.snapshots && after.tableBuilds == before.tableBuilds && after.device.prepares > before.device.prepares, what + ": the device path snapshotted or bound the CPU table, or did not use the device table" + Describe(before, after));
    Require(after.device.drained == before.device.drained, what + ": the device path recorded misses for keys it had inserted" + Describe(before, after));
}

void RequireTexel(const std::array<float, 4>& output, const std::array<float, 4>& expected, const std::string& what) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        Require(std::fabs(output[channel] - expected[channel]) < 1e-6f, what + ": channel " + std::to_string(channel) + " is " + std::to_string(output[channel]) + ", expected " + std::to_string(expected[channel]));
    }
}

struct Harness {
    const GuestBlock& block;
    bool device = false;
    bool expecting = true;
};

void RequireMissRing(const Harness& harness, const std::string& part) {
    Require(!harness.device || !harness.expecting, part + " reads the miss ring, which a draw whose keys the driver expects before its batch does not write: run it with APS5_NO_DEVICE_IMAGE_EXPECT=1");
}

constexpr std::array<std::uint32_t, 10> ComputeKeys{3u, 1764u, 37u, 54u, 1495u, 72u, 1657u, 1600u, 33u, 1810u};

void RunComputeKeys(const Harness& harness, std::span<const std::uint32_t> keys, const std::uint32_t* code = ComputeCode.data()) {
    for (std::size_t index = 0; index < keys.size(); ++index) Outputs[index].fill(-1.0f);
    std::vector<std::uint32_t> words;
    for (std::size_t index = 0; index < keys.size(); ++index) Dispatch(words, harness.block, code, keys[index], Outputs[index]);
    SubmitCompute(words);
}

void RequireComputeTexels(std::span<const std::uint32_t> keys, const std::string& what) {
    for (std::size_t index = 0; index < keys.size(); ++index) RequireTexel(Outputs[index], TexelOf(TextureOf(keys[index])).value, what + ", key " + std::to_string(keys[index]));
}

void ComputeTable(const Harness& harness) {
    std::vector<std::uint32_t> formats;
    for (const auto key : ComputeKeys) {
        const auto format = TexelOf(TextureOf(key)).format;
        if (std::find(formats.begin(), formats.end(), format) == formats.end()) formats.push_back(format);
    }
    Require(formats.size() >= 5u, "the compute keys sample fewer than five formats");
    auto before = Read();
    RunComputeKeys(harness, ComputeKeys);
    auto after = Read();
    for (std::size_t index = 0; index < ComputeKeys.size(); ++index) {
        const auto texture = TextureOf(ComputeKeys[index]);
        const auto what = "(a) compute, first batch, key " + std::to_string(ComputeKeys[index]) + " (" + FormatNames[texture % Formats.size()] + ")";
        RequireTexel(Outputs[index], TexelOf(texture).value, what);
    }
    if (harness.device) {
        RequireHits(before, after, "(a) compute, first batch");
        Require(after.device.inserted == before.device.inserted + ComputeKeys.size(), "(a) compute, first batch: the device path did not insert the ten keys before the batch" + Describe(before, after));
    } else {
        RequireCpuTable(before, after, "(a) compute, first batch");
    }
    std::printf("(a) compute, first batch of %zu dispatches: %s%s\n", ComputeKeys.size(), harness.device ? "texels, keys inserted before the batch" : "texels through the CPU table", Describe(before, after).c_str());
    before = after;
    RunComputeKeys(harness, ComputeKeys);
    after = Read();
    for (std::size_t index = 0; index < ComputeKeys.size(); ++index) {
        const auto texture = TextureOf(ComputeKeys[index]);
        RequireTexel(Outputs[index], TexelOf(texture).value, "(a) compute, second batch, key " + std::to_string(ComputeKeys[index]) + " (" + FormatNames[texture % Formats.size()] + ")");
    }
    if (harness.device) {
        RequireHits(before, after, "(a) compute, second batch");
        Require(after.device.inserted == before.device.inserted, "(a) compute, second batch: the device path inserted keys it already held" + Describe(before, after));
    }
    std::printf("(a) compute, second batch: the texels of %zu formats%s\n", formats.size(), Describe(before, after).c_str());
}

AgcDriver::Graphics::ColorTargetLayout ColorLayout() {
    return AgcDriver::Graphics::ColorTargetLayout(ColorWidth, ColorHeight, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
}

void DrawOnce(const Harness& harness, std::uint32_t key, DrawKind kind) {
    std::memset(harness.block.Color(), static_cast<int>(Kept), ColorLayout().Bytes());
    auto commands = DrawCommands(harness.block, key, kind);
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, std::string("the ") + DrawName(kind) + " submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

std::vector<std::byte> ReadColor(const Harness& harness) {
    std::vector<std::byte> stored(ColorLayout().Bytes());
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(harness.block.Color())), stored);
    return stored;
}

std::size_t RequireCovered(const std::vector<std::byte>& stored, const AgcDriver::Graphics::ColorTargetLayout& layout, std::uint32_t width, std::uint32_t height, const std::array<float, 4>& value, const std::string& what) {
    std::size_t drawn = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = layout.Offset(x, y);
            if (std::all_of(stored.begin() + static_cast<std::ptrdiff_t>(offset), stored.begin() + static_cast<std::ptrdiff_t>(offset + 4), [](std::byte byte) { return byte == Kept; })) continue;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const auto texel = static_cast<long>(stored[offset + channel]);
                const auto expected = std::lround(value[channel] * 255.0f);
                const bool halfStep = value[channel] * 255.0f == 127.5f && texel == 127;
                Require(texel == expected || halfStep, what + ": pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") channel " + std::to_string(channel) + " is " + std::to_string(texel) + ", expected " + std::to_string(expected));
            }
            ++drawn;
        }
    }
    Require(drawn != 0, what + ": the draw covered no pixel");
    return drawn;
}

void RequireDrawn(const std::vector<std::byte>& stored, const std::array<float, 4>& value, const std::string& what) {
    static_cast<void>(RequireCovered(stored, ColorLayout(), ColorWidth, ColorHeight, value, what));
}

void Draws(const Harness& harness) {
    const std::array<std::pair<DrawKind, std::uint32_t>, 3> draws{{{DrawKind::Direct, 1546u}, {DrawKind::Indirect, 1867u}, {DrawKind::IndexedIndirect, 1772u}}};
    for (const auto& [kind, key] : draws) {
        const auto texture = TextureOf(key);
        const auto name = std::string("(b) ") + DrawName(kind) + ", key " + std::to_string(key) + " (" + FormatNames[texture % Formats.size()] + ")";
        auto before = Read();
        DrawOnce(harness, key, kind);
        auto stored = ReadColor(harness);
        auto after = Read();
        RequireDrawn(stored, TexelOf(texture).value, name + ", first draw");
        if (harness.device) {
            RequireHits(before, after, name + ", first draw");
            Require(after.device.inserted > before.device.inserted, name + ", first draw: the device path inserted no key before the batch" + Describe(before, after));
        } else {
            RequireCpuTable(before, after, name + ", first draw");
        }
        std::printf("%s, first draw: %s%s\n", name.c_str(), harness.device ? "the texel, key inserted before the batch" : "the texel through the CPU table", Describe(before, after).c_str());
        before = after;
        DrawOnce(harness, key, kind);
        RequireDrawn(ReadColor(harness), TexelOf(texture).value, name + ", second draw");
        after = Read();
        if (harness.device) {
            RequireHits(before, after, name + ", second draw");
            Require(after.device.expectationHits > before.device.expectationHits, name + ", second draw: the expectation of an unchanged record in a later submission did not come from the memo");
        }
        std::printf("%s, second draw: the texel%s\n", name.c_str(), Describe(before, after).c_str());
    }
}

void OneShot(const Harness& harness) {
    constexpr std::uint32_t computeKey = 1766u;
    constexpr std::uint32_t drawKey = 1774u;
    Require(TextureOf(computeKey) != TextureOf(drawKey), "the one-shot keys share a texture");
    const auto computeTexture = TextureOf(computeKey);
    const auto computeWhat = "(one-shot) compute, key " + std::to_string(computeKey) + " (" + FormatNames[computeTexture % Formats.size()] + ") sampled once";
    auto before = Read();
    RunComputeKeys(harness, std::span(&computeKey, 1));
    auto after = Read();
    RequireTexel(Outputs[0], TexelOf(computeTexture).value, computeWhat);
    if (harness.device) {
        RequireHits(before, after, computeWhat);
        Require(after.device.inserted > before.device.inserted, computeWhat + ": the device path inserted no key before the batch" + Describe(before, after));
    } else {
        RequireCpuTable(before, after, computeWhat);
    }
    std::printf("%s: the texel%s\n", computeWhat.c_str(), Describe(before, after).c_str());
    const auto drawTexture = TextureOf(drawKey);
    const auto drawWhat = "(one-shot) direct draw, key " + std::to_string(drawKey) + " (" + FormatNames[drawTexture % Formats.size()] + ") in one render pass";
    before = Read();
    DrawOnce(harness, drawKey, DrawKind::Direct);
    const auto stored = ReadColor(harness);
    after = Read();
    RequireDrawn(stored, TexelOf(drawTexture).value, drawWhat);
    if (harness.device) {
        RequireHits(before, after, drawWhat);
        Require(after.device.inserted > before.device.inserted, drawWhat + ": the device path inserted no key before the batch" + Describe(before, after));
    } else {
        RequireCpuTable(before, after, drawWhat);
    }
    std::printf("%s: the texel%s\n", drawWhat.c_str(), Describe(before, after).c_str());
}

void Freshness(const Harness& harness) {
    RunComputeKeys(harness, ComputeKeys);
    RunComputeKeys(harness, ComputeKeys);
    std::array<std::array<float, 4>, ComputeKeys.size()> previous{};
    std::copy_n(Outputs.begin(), ComputeKeys.size(), previous.begin());
    const auto written = TextureOf(ComputeKeys[1]);
    Require(written % Formats.size() == 4u, "the written texture is not 8_8_8_8 unorm");
    FillTexture(harness.block.Texture(written), WrittenTexel);
    const auto before = Read();
    RunComputeKeys(harness, ComputeKeys);
    const auto after = Read();
    for (std::size_t index = 0; index < ComputeKeys.size(); ++index) {
        const auto what = "(c) freshness, key " + std::to_string(ComputeKeys[index]);
        if (TextureOf(ComputeKeys[index]) == written) {
            RequireTexel(Outputs[index], {WrittenTexel[0] / 255.0f, WrittenTexel[1] / 255.0f, WrittenTexel[2] / 255.0f, WrittenTexel[3] / 255.0f}, what + " (the written texture)");
        } else {
            RequireTexel(Outputs[index], previous[index], what + " (an unwritten texture)");
        }
    }
    if (harness.device) {
        RequireHits(before, after, "(c) freshness");
        Require(after.device.checked > before.device.checked, "(c) freshness: the device path re-checked no entry for a written texture");
    }
    std::printf("(c) freshness: a CPU write to texture %u changed only its texels%s, %llu entries re-checked, %llu given a new texture\n", written, Describe(before, after).c_str(), static_cast<unsigned long long>(after.device.checked - before.device.checked), static_cast<unsigned long long>(after.device.refreshed - before.device.refreshed));
}

void Lanes(const Harness& harness) {
    auto before = Read();
    Outputs[0].fill(-1.0f);
    std::vector<std::uint32_t> words;
    Dispatch(words, harness.block, ComputeCode.data(), NullEntry, Outputs[0]);
    SubmitCompute(words);
    auto after = Read();
    RequireTexel(Outputs[0], {}, "(d) a null T#");
    if (harness.device) Require(after.device.drained == before.device.drained, "(d) a null T# recorded a miss" + Describe(before, after));
    std::printf("(d) a null T# reads zeros%s\n", Describe(before, after).c_str());
    const auto fresh = 13u;
    Require(TextureOf(1773u) == fresh, "the EXEC test key does not select its unused texture");
    before = Read();
    Outputs[1].fill(-1.0f);
    words.clear();
    Dispatch(words, harness.block, ExecClearCode.data(), 1773u, Outputs[1]);
    SubmitCompute(words);
    after = Read();
    RequireTexel(Outputs[1], {-1.0f, -1.0f, -1.0f, -1.0f}, "(d) a lane with EXEC clear stored");
    if (harness.device) Require(after.device.drained == before.device.drained, "(d) a lane with EXEC clear recorded a miss" + Describe(before, after));
    std::printf("(d) a lane with EXEC clear records nothing%s\n", Describe(before, after).c_str());
}

void Faults(const Harness& harness) {
    const auto before = Read();
    Outputs[0].fill(-1.0f);
    std::vector<std::uint32_t> commands;
    Dispatch(commands, harness.block, ComputeCode.data(), InvalidEntry, Outputs[0]);
    StderrCapture capture;
    SubmitCompute(commands);
    const auto log = capture.Finish();
    const auto after = Read();
    const bool reported = log.find("selected the descriptor") != std::string::npos;
    std::printf("(e) the batch selecting an invalid descriptor: %s%s\n", reported ? "error reported" : "no error", Describe(before, after).c_str());
    Require(reported, std::string("(e) the ") + (harness.device ? "device" : "CPU") + " path did not raise the rejected descriptor's error in the batch that sampled its entry");
    if (harness.device) Require(after.device.faults > before.device.faults && after.device.inserted == before.device.inserted, "(e) the device path did not resolve the rejected descriptor before the batch that sampled it" + Describe(before, after));
    Require(log.find("invalid format") != std::string::npos, "(e) the error does not name the CPU path's reason: " + log);
    const auto words = TextureDescriptor(harness.block.Texture(0), 0u);
    char first[16];
    std::snprintf(first, sizeof(first), "%08x", words[0]);
    Require(log.find(first) != std::string::npos, std::string("(e) the error does not name the descriptor's words: ") + log);
}

void Rechecks(const Harness& harness) {
    auto commands = DrawCommands(harness.block, 1546u, DrawKind::Direct);
    for (int extra = 0; extra < 3; ++extra) Append(commands, 0x2d, {3, 2});
    const auto before = Read();
    std::memset(harness.block.Color(), static_cast<int>(Kept), ColorLayout().Bytes());
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the four-draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    RequireDrawn(ReadColor(harness), TexelOf(TextureOf(1546u)).value, "(g) four draws in one submission");
    const auto after = Read();
    if (harness.device) Require(after.device.checked - before.device.checked <= after.device.entries, "(g) four draws in one submission re-checked " + std::to_string(after.device.checked - before.device.checked) + " entries of " + std::to_string(after.device.entries));
    if (harness.device) Require(after.device.expectationHits - before.device.expectationHits >= 3u, "(g) four draws in one submission took " + std::to_string(after.device.expectationHits - before.device.expectationHits) + " of their expectations from the memo, expected at least 3");
    std::printf("(g) four draws in one submission: %llu entries re-checked of %u%s\n", static_cast<unsigned long long>(after.device.checked - before.device.checked), after.device.entries, Describe(before, after).c_str());
}

std::vector<std::uint32_t> ProduceAndSample(const GuestBlock& block, const std::uint32_t* producer) {
    auto commands = DrawCommands(block, TargetEntry, DrawKind::Direct);
    commands.resize(commands.size() - 3u);
    const auto target = reinterpret_cast<std::uintptr_t>(block.At(TargetOffset));
    const auto color = reinterpret_cast<std::uintptr_t>(block.Color());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const auto produce = reinterpret_cast<std::uintptr_t>(producer);
    Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(target >> 8u)});
    Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(target >> 40u)});
    Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(produce >> 8u)});
    Append(commands, 0x76, {0x009, static_cast<std::uint32_t>(produce >> 40u)});
    Append(commands, 0x76, {0x00b, 0u});
    Append(commands, 0x2d, {3, 2});
    Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(color >> 8u)});
    Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(color >> 40u)});
    Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(pixel >> 8u)});
    Append(commands, 0x76, {0x009, static_cast<std::uint32_t>(pixel >> 40u)});
    Append(commands, 0x76, {0x00b, 9u << 1u});
    Append(commands, 0x2d, {3, 2});
    return commands;
}

void GpuWrittenTexture(const Harness& harness) {
    const std::array<std::array<float, 4>, 2> colors{{{0.5f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.5f, 0.0f, 1.0f}}};
    const std::array<const std::uint32_t*, 3> producers{ProducerPixelCode.data(), ProducerPixelCode.data(), OtherProducerPixelCode.data()};
    std::memset(harness.block.At(TargetOffset), 0, ColorLayout().Bytes());
    for (std::size_t frame = 0; frame < producers.size(); ++frame) {
        auto commands = ProduceAndSample(harness.block, producers[frame]);
        const auto before = Read();
        std::memset(harness.block.Color(), static_cast<int>(Kept), ColorLayout().Bytes());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the render-then-sample submission failed");
        AgcDriverWaitIdle_nid_postfix();
        const auto stored = ReadColor(harness);
        const auto after = Read();
        const auto expected = colors[frame == 2 ? 1 : 0];
        const auto what = "(h) frame " + std::to_string(frame + 1) + ": a render target drawn, then sampled through the table";
        RequireDrawn(stored, expected, what);
        if (harness.device) Require(after.device.drained == before.device.drained, what + ": the entry missed" + Describe(before, after));
        if (harness.device && frame == 2) Require(after.device.inserted == before.device.inserted, what + ": the GPU-written entry was inserted again" + Describe(before, after));
        std::printf("%s: %s, %llu entries given a new texture%s\n", what.c_str(), "the producer's texels", static_cast<unsigned long long>(after.device.refreshed - before.device.refreshed), Describe(before, after).c_str());
    }
}

struct Blocker {
    std::atomic<bool> release{false};
    void Enqueue() {
        AgcDriver::DriverDetail::DrawPipeline::Queue0().Enqueue([this] {
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }, {});
    }
};

void Finish() {
    auto idle = std::async(std::launch::async, [] { AgcDriverWaitIdle_nid_postfix(); });
    if (idle.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        std::fputs("device image path harness: the queue worker and the draw pipeline deadlocked\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    idle.get();
}

void WriteRecord(const GuestBlock& block, std::uint32_t entry, std::uint32_t texture) {
    const auto descriptor = TextureDescriptor(block.Texture(texture), TexelOf(texture).format);
    std::copy(descriptor.begin(), descriptor.end(), block.Table() + static_cast<std::size_t>(entry) * (RecordBytes / 4u));
}

std::array<float, 4> DrawnValue(const std::vector<std::byte>& stored) {
    const auto layout = ColorLayout();
    for (std::uint32_t y = 0; y < ColorHeight; ++y) {
        for (std::uint32_t x = 0; x < ColorWidth; ++x) {
            const auto offset = layout.Offset(x, y);
            if (std::all_of(stored.begin() + static_cast<std::ptrdiff_t>(offset), stored.begin() + static_cast<std::ptrdiff_t>(offset + 4), [](std::byte byte) { return byte == Kept; })) continue;
            std::array<float, 4> value{};
            for (std::size_t channel = 0; channel < 4; ++channel) value[channel] = static_cast<float>(stored[offset + channel]) / 255.0f;
            return value;
        }
    }
    throw std::runtime_error("the draw covered no pixel");
}

bool SameTexel(const std::array<float, 4>& drawn, const std::array<float, 4>& value) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::lround(drawn[channel] * 255.0f) != std::lround(value[channel] * 255.0f)) return false;
    }
    return true;
}

void RequireNewRecord(const Harness& harness, const std::vector<std::byte>& stored, std::uint32_t old, std::uint32_t written, bool expected, const std::string& what) {
    const auto drawn = DrawnValue(stored);
    Require(!SameTexel(drawn, TexelOf(old).value), what + ": the draw after the write sampled the texture the record named before it");
    if (harness.device && !expected) Require(SameTexel(drawn, {}) || SameTexel(drawn, TexelOf(written).value), what + ": the draw after the write sampled neither zeros (a miss on the new key) nor the texture the record names now");
    else RequireDrawn(stored, TexelOf(written).value, what);
}

void StaleRecord(const Harness& harness) {
    constexpr std::uint32_t entry = 1545u;
    constexpr std::array<std::uint32_t, 3> textures{9u, 14u, 9u};
    Require(TextureOf(entry) == textures[0] && TexelOf(textures[1]).format == TexelOf(textures[0]).format && TexelOf(textures[2]).format == TexelOf(textures[0]).format, "the rewritten record's textures are not one format");
    DrawOnce(harness, entry, DrawKind::Direct);
    DrawOnce(harness, entry, DrawKind::Direct);
    RequireDrawn(ReadColor(harness), TexelOf(textures[0]).value, "(i) the record before any write");
    WriteRecord(harness.block, entry, textures[1]);
    auto before = Read();
    DrawOnce(harness, entry, DrawKind::Direct);
    auto stored = ReadColor(harness);
    auto after = Read();
    RequireNewRecord(harness, stored, textures[0], textures[1], true, "(i) a CPU write to the record in the previous submission");
    if (harness.device) Require(after.device.drained == before.device.drained, "(i) a CPU write to the record in the previous submission: the new key missed" + Describe(before, after));
    const auto nextFirst = DrawnValue(stored);
    DrawOnce(harness, entry, DrawKind::Direct);
    RequireDrawn(ReadColor(harness), TexelOf(textures[1]).value, "(i) the second draw after a write in the previous submission");
    std::printf("(i) record rewritten between submissions (the next collect epoch): first draw %s, second draw the new texture%s\n", SameTexel(nextFirst, {}) ? "zeros, the new key missed" : "the new texture", Describe(before, after).c_str());
    std::memset(harness.block.Color(), static_cast<int>(Kept), ColorLayout().Bytes());
    auto commands = DrawCommands(harness.block, entry, DrawKind::Direct);
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    auto& pipeline = AgcDriver::DriverDetail::DrawPipeline::Queue0();
    Blocker blocker;
    before = Read();
    blocker.Enqueue();
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the held draw's submission failed");
    const auto start = std::chrono::steady_clock::now();
    while (pipeline.Outstanding() < 3u && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto outstanding = pipeline.Outstanding();
    WriteRecord(harness.block, entry, textures[2]);
    blocker.release = true;
    Finish();
    Require(outstanding >= 3u, "(i) the draw was not held behind the committer item (" + std::to_string(outstanding) + " items outstanding), so the write did not land inside its submission");
    stored = ReadColor(harness);
    after = Read();
    const auto sameFirst = DrawnValue(stored);
    if (harness.device) RequireNewRecord(harness, stored, textures[1], textures[2], false, "(i) a CPU write to the record after the draw was decoded, in its own submission");
    DrawOnce(harness, entry, DrawKind::Direct);
    RequireDrawn(ReadColor(harness), TexelOf(textures[2]).value, "(i) the draw after a write inside the previous submission");
    const char* sampled = SameTexel(sameFirst, {}) ? "zeros, the new key missed" : SameTexel(sameFirst, TexelOf(textures[2]).value) ? "the new texture" : SameTexel(sameFirst, TexelOf(textures[1]).value) ? "the old texture (the CPU table was taken when the draw was decoded)" : "another value";
    std::printf("(i) record rewritten after the draw was decoded, in the same collect epoch: first draw %s, next draw the new texture%s\n", sampled, Describe(before, after).c_str());
    WriteRecord(harness.block, entry, textures[0]);
}

void Unexpected(const Harness& harness) {
    constexpr std::uint32_t computeKey = 1775u;
    constexpr std::uint32_t drawKey = 1789u;
    const auto computeTexture = TextureOf(computeKey);
    const auto computeWhat = "(n) compute, key " + std::to_string(computeKey) + " (" + FormatNames[computeTexture % Formats.size()] + ") offset by a lane value the pre-pass cannot evaluate";
    auto before = Read();
    Outputs[0].fill(-1.0f);
    std::vector<std::uint32_t> words;
    Dispatch(words, harness.block, LaneKeyComputeCode.data(), computeKey, Outputs[0], 64u);
    SubmitCompute(words);
    auto after = Read();
    RequireTexel(Outputs[0], TexelOf(computeTexture).value, computeWhat);
    RequireCpuTable(before, after, computeWhat);
    if (harness.device) Require(after.device.fallbacks == before.device.fallbacks + 1u && after.device.drained == before.device.drained, computeWhat + ": the program did not fall back to the CPU table path before its batch (fallbacks " + std::to_string(after.device.fallbacks - before.device.fallbacks) + ")" + Describe(before, after));
    std::printf("%s: the texel through the CPU table, %llu fallbacks%s\n", computeWhat.c_str(), static_cast<unsigned long long>(after.device.fallbacks - before.device.fallbacks), Describe(before, after).c_str());
    const auto drawTexture = TextureOf(drawKey);
    const auto drawWhat = "(n) direct draw, key " + std::to_string(drawKey) + " (" + FormatNames[drawTexture % Formats.size()] + ") offset by a lane value the pre-pass cannot evaluate";
    std::array<std::uint32_t, 9> userData{};
    const auto table = BufferDescriptor(harness.block.Table(), Entries * RecordBytes);
    std::copy(table.begin(), table.end(), userData.begin());
    userData[8] = drawKey;
    auto commands = DrawCommands(harness.block, DrawKind::Direct, LaneKeyPixelCode.data(), userData);
    commands.resize(commands.size() - 3u);
    Append(commands, 0x69, {0x1b3, 1});
    Append(commands, 0x69, {0x1b4, 1});
    Append(commands, 0x2d, {3, 2});
    before = Read();
    std::memset(harness.block.Color(), static_cast<int>(Kept), ColorLayout().Bytes());
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the lane-key draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    const auto stored = ReadColor(harness);
    after = Read();
    RequireDrawn(stored, TexelOf(drawTexture).value, drawWhat);
    RequireCpuTable(before, after, drawWhat);
    if (harness.device) Require(after.device.fallbacks == before.device.fallbacks + 1u && after.device.drained == before.device.drained, drawWhat + ": the program did not fall back to the CPU table path before its batch (fallbacks " + std::to_string(after.device.fallbacks - before.device.fallbacks) + ")" + Describe(before, after));
    std::printf("%s: the texel through the CPU table, %llu fallbacks%s\n", drawWhat.c_str(), static_cast<unsigned long long>(after.device.fallbacks - before.device.fallbacks), Describe(before, after).c_str());
}

void FallsBackFromWarmCache(const Harness& harness) {
    const std::array<std::uint32_t, 4> keys{1546u, 1867u, 1772u, 1773u};
    auto before = Read();
    DrawOnce(harness, keys[0], DrawKind::Direct);
    static_cast<void>(ReadColor(harness));
    DrawOnce(harness, keys[0], DrawKind::Direct);
    RequireDrawn(ReadColor(harness), TexelOf(TextureOf(keys[0])).value, "(f) the warm draw");
    for (std::size_t index = 1; index < keys.size(); ++index) {
        DrawOnce(harness, keys[index], DrawKind::Direct);
        static_cast<void>(ReadColor(harness));
        DrawOnce(harness, keys[index], DrawKind::Direct);
    }
    static_cast<void>(ReadColor(harness));
    auto after = Read();
    if (harness.device) Require(after.device.heapFull != before.device.heapFull && after.device.fallbacks == before.device.fallbacks + 1u, "(f) a heap capped at two slots per kind did not send the program back to the CPU table path (heap full " + std::to_string(after.device.heapFull - before.device.heapFull) + ", fallbacks " + std::to_string(after.device.fallbacks - before.device.fallbacks) + ")");
    std::printf("(f) %zu keys on a heap of two slots per kind: %llu without a slot, %llu fallbacks%s\n", keys.size(), static_cast<unsigned long long>(after.device.heapFull - before.device.heapFull), static_cast<unsigned long long>(after.device.fallbacks - before.device.fallbacks), Describe(before, after).c_str());
    for (const auto key : keys) {
        before = Read();
        DrawOnce(harness, key, DrawKind::Direct);
        const auto what = "(f) after the fallback, key " + std::to_string(key) + " from a warm draw cache";
        RequireDrawn(ReadColor(harness), TexelOf(TextureOf(key)).value, what);
        after = Read();
        if (harness.device) Require(after.device.prepares == before.device.prepares && after.device.drained == before.device.drained, what + ": the draw still read the device image table" + Describe(before, after));
        std::printf("%s: the texel%s\n", what.c_str(), Describe(before, after).c_str());
    }
}

constexpr std::array<std::uint32_t, 2> FillingKeys{32u, 33u};
constexpr std::array<std::uint32_t, 3> MeetingKeys{34u, 35u, 36u};

void Captured(std::string& errors, const std::function<void()>& submit) {
    StderrCapture capture;
    try {
        submit();
    } catch (...) {
        errors += capture.Finish();
        throw;
    }
    errors += capture.Finish();
}

void GpuModeDraw(const Harness& harness, std::uint32_t texture, bool heapStage, const std::string& what, std::string& errors) {
    const auto taken = AgcDriver::DriverDetail::HeapStagesTaken();
    const auto before = Read();
    std::array<std::uint32_t, 12> userData{};
    const auto descriptor = TextureDescriptor(harness.block.Texture(texture), TexelOf(texture).format);
    std::copy(descriptor.begin(), descriptor.end(), userData.begin());
    Captured(errors, [&] {
        std::memset(harness.block.Color(), static_cast<int>(Kept), ColorLayout().Bytes());
        auto commands = DrawCommands(harness.block, DrawKind::Direct, DirectPixelCode.data(), userData);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the GPU-mode draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
    });
    RequireDrawn(ReadColor(harness), TexelOf(texture).value, what);
    const auto after = Read();
    Require((AgcDriver::DriverDetail::HeapStagesTaken() > taken) == heapStage && after.device.prepares == before.device.prepares, what + (heapStage ? ": the draw did not take GPU mode's heap stage path" : ": the draw took GPU mode's heap stage path on a refused heap") + Describe(before, after));
    std::printf("%s: the texel through %s%s\n", what.c_str(), heapStage ? "a GPU-mode heap stage" : "the per-draw path", Describe(before, after).c_str());
}

void ComputeMeetsFullHeap(const Harness& harness) {
    Require(harness.device, "(l) needs the device image path");
    RequireMissRing(harness, "(l)");
    const bool gpuHeap = AgcDriver::Graphics::GpuHeapEnabled();
    std::string errors;
    auto before = Read();
    Captured(errors, [&] { RunComputeKeys(harness, FillingKeys); });
    Captured(errors, [&] { RunComputeKeys(harness, FillingKeys); });
    auto after = Read();
    Require(after.device.inserted == before.device.inserted + 1u && after.device.heapFull == before.device.heapFull + 1u && after.device.fallbacks == before.device.fallbacks + 1u, "(l) the first program's two textures did not take one of the heap's two slots and fall back (heap full " + std::to_string(after.device.heapFull - before.device.heapFull) + ")" + Describe(before, after));
    Require(!AgcDriver::Graphics::BindlessHeap::NearlyFull(), "(l) the table's slot refused the heap to GPU mode");
    before = after;
    Captured(errors, [&] { RunComputeKeys(harness, FillingKeys); });
    after = Read();
    RequireCpuTable(before, after, "(l) the first program after its fallback");
    RequireComputeTexels(FillingKeys, "(l) the first program after its fallback");
    std::printf("(l) a compute program asks for both of the heap's two slots, takes 1 and leaves the heap's margin of 1 free: the texels through the CPU table%s\n", Describe(before, after).c_str());
    if (gpuHeap) {
        GpuModeDraw(harness, 5u, true, "(l) a GPU-mode draw of a texture in its user data takes the other slot", errors);
        Require(AgcDriver::Graphics::BindlessHeap::NearlyFull(), "(l) GPU mode's take of the margin's slot did not refuse the heap");
    }
    before = Read();
    Captured(errors, [&] { RunComputeKeys(harness, MeetingKeys, OtherComputeCode.data()); });
    after = Read();
    RequireMisses(before, after, MeetingKeys.size(), "(l) a second compute program, first batch");
    const auto missed = after;
    Captured(errors, [&] { RunComputeKeys(harness, MeetingKeys, OtherComputeCode.data()); });
    after = Read();
    Require(after.device.inserted == missed.device.inserted, "(l) a compute program meeting the full heap took a slot" + Describe(missed, after));
    Require(after.device.heapFull >= missed.device.heapFull + 1u && after.device.fallbacks == missed.device.fallbacks + 1u, "(l) a compute program meeting the full heap was not sent back to the CPU table path (heap full " + std::to_string(after.device.heapFull - missed.device.heapFull) + ", fallbacks " + std::to_string(after.device.fallbacks - missed.device.fallbacks) + ")");
    std::printf("(l) a second compute program meets the %s heap: %llu keys without a slot, %llu fallback, no slot taken%s\n", gpuHeap ? "full and refused" : "kept", static_cast<unsigned long long>(after.device.heapFull - missed.device.heapFull), static_cast<unsigned long long>(after.device.fallbacks - missed.device.fallbacks), Describe(missed, after).c_str());
    before = after;
    Captured(errors, [&] { RunComputeKeys(harness, MeetingKeys, OtherComputeCode.data()); });
    after = Read();
    RequireCpuTable(before, after, "(l) the second program after its fallback");
    RequireComputeTexels(MeetingKeys, "(l) the second program after its fallback");
    std::printf("(l) the second program after its fallback: the texels through the CPU table%s\n", Describe(before, after).c_str());
    if (gpuHeap) GpuModeDraw(harness, 6u, false, "(l) a GPU-mode draw of another texture on the refused heap", errors);
    else Require(!AgcDriver::Graphics::BindlessHeap::NearlyFull(), "(l) the table's programs refused the heap to GPU mode");
    Require(errors.find("heap is full") == std::string::npos, "(l) the full heap raised an error");
}

AgcDriver::Graphics::ColorTargetLayout LinearLayout() {
    return AgcDriver::Graphics::ColorTargetLayout(ColorWidth, ColorHeight, AgcDriver::Graphics::ColorTileMode::Linear, 4);
}

std::vector<std::byte> DrawLinear(const Harness& harness, const GuestBlock& target, std::uint32_t key) {
    auto commands = DrawCommands(harness.block, key, DrawKind::Direct);
    commands.resize(commands.size() - 3u);
    const auto color = reinterpret_cast<std::uintptr_t>(target.At(0));
    Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(color >> 8u)});
    Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(color >> 40u)});
    Append(commands, 0x69, {0x3b8, LinearAttrib3});
    Append(commands, 0x2d, {3, 2});
    std::memset(target.At(0), static_cast<int>(Kept), LinearLayout().Bytes());
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the submission of a draw into the linear target failed");
    AgcDriverWaitIdle_nid_postfix();
    std::vector<std::byte> stored(LinearLayout().Bytes());
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(color), stored);
    return stored;
}

void UnrecordedDraw(const Harness& harness) {
    Require(TextureOf(UnrecordedKey) == Textures - 1u, "the unrecorded draw's key does not select its unused texture");
    GuestBlock target(TableBlockBytes);
    const auto value = TexelOf(TextureOf(UnrecordedKey)).value;
    const auto start = Read();
    auto before = start;
    const auto stored = DrawLinear(harness, target, UnrecordedKey);
    auto after = Read();
    const auto outside = "(m) key " + std::to_string(UnrecordedKey) + ", a draw into a linear target outside the recorder";
    RequireCpuTable(before, after, outside);
    const auto pixels = RequireCovered(stored, LinearLayout(), ColorWidth, ColorHeight, value, outside);
    std::printf("%s: the texel on %zu pixels through the CPU table%s\n", outside.c_str(), pixels, Describe(before, after).c_str());
    before = after;
    DrawOnce(harness, UnrecordedKey, DrawKind::Direct);
    const bool missed = harness.device && !harness.expecting;
    RequireDrawn(ReadColor(harness), missed ? std::array<float, 4>{} : value, "(m) the next recorded draw of the same program");
    after = Read();
    if (missed) RequireMisses(before, after, 1u, "(m) the next recorded draw of the same program");
    else if (harness.device) RequireHits(before, after, "(m) the next recorded draw of the same program");
    else RequireCpuTable(before, after, "(m) the next recorded draw of the same program");
    std::printf("(m) the next recorded draw of the same program: %s%s\n", missed ? "zeros, key recorded" : harness.device ? "the texel through the device table, key expected" : "the texel through the CPU table", Describe(before, after).c_str());
    before = after;
    DrawOnce(harness, UnrecordedKey, DrawKind::Direct);
    RequireDrawn(ReadColor(harness), value, "(m) the recorded draw after it");
    after = Read();
    if (harness.device) RequireHits(before, after, "(m) the recorded draw after it");
    std::printf("(m) the recorded draw after it: the texel%s\n", Describe(before, after).c_str());
    Require(after.device.fallbacks == start.device.fallbacks, "(m) the draw outside the recorder sent the program back to the CPU table path (fallbacks " + std::to_string(after.device.fallbacks - start.device.fallbacks) + ")");
}

AgcDriver::Graphics::ColorTargetLayout LargeLayout() {
    return AgcDriver::Graphics::ColorTargetLayout(LargeSide, LargeSide, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
}

std::vector<std::byte> DrawLarge(const Harness& harness, std::span<const std::uint32_t> entries) {
    auto commands = DrawCommands(harness.block, entries.front(), DrawKind::Direct);
    commands.resize(commands.size() - 3u);
    const auto color = reinterpret_cast<std::uintptr_t>(harness.block.At(LargeColorOffset));
    const auto pixel = reinterpret_cast<std::uintptr_t>(LargePixelCode.data());
    const auto half = static_cast<float>(LargeSide / 2u);
    const auto corner = LargeSide | (LargeSide << 16u);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> large{
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)}, {0x3b0, ((LargeSide - 1u) << 14u) | (LargeSide - 1u)},
        {0x0d, corner}, {0x82, corner}, {0x91, corner}, {0x95, corner},
        {0x10f, std::bit_cast<std::uint32_t>(half)}, {0x110, std::bit_cast<std::uint32_t>(half)}, {0x111, std::bit_cast<std::uint32_t>(-half)}, {0x112, std::bit_cast<std::uint32_t>(half)},
    };
    for (const auto& [offset, value] : large) Append(commands, 0x69, {offset, value});
    Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(pixel >> 8u)});
    Append(commands, 0x76, {0x009, static_cast<std::uint32_t>(pixel >> 40u)});
    for (const auto entry : entries) {
        Append(commands, 0x76, {0x00cu + 8u, entry});
        Append(commands, 0x2d, {3, 2});
    }
    std::memset(harness.block.At(LargeColorOffset), static_cast<int>(Kept), LargeLayout().Bytes());
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the submission of draws into the large target failed");
    AgcDriverWaitIdle_nid_postfix();
    std::vector<std::byte> stored(LargeLayout().Bytes());
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(harness.block.At(LargeColorOffset))), stored);
    return stored;
}

std::size_t LargeCovered(const std::vector<std::byte>& stored) {
    const auto layout = LargeLayout();
    std::size_t covered = 0;
    for (std::uint32_t y = 0; y < LargeSide; ++y) {
        for (std::uint32_t x = 0; x < LargeSide; ++x) {
            const auto offset = layout.Offset(x, y);
            if (!std::all_of(stored.begin() + static_cast<std::ptrdiff_t>(offset), stored.begin() + static_cast<std::ptrdiff_t>(offset + 4), [](std::byte byte) { return byte == Kept; })) ++covered;
        }
    }
    return covered;
}

std::size_t RequireLargeDrawn(const std::vector<std::byte>& stored, const std::array<float, 4>& value, const std::string& what) {
    return RequireCovered(stored, LargeLayout(), LargeSide, LargeSide, value, what);
}

void RequireDeduplicated(const Counters& before, const Counters& after, std::size_t keys, std::size_t pixels, const std::string& what) {
    const auto records = after.device.drained - before.device.drained;
    const auto dropped = after.device.dropped - before.device.dropped;
    Require(dropped == 0u && records <= keys, what + ": " + std::to_string(records) + " miss records and " + std::to_string(dropped) + " dropped for " + std::to_string(keys) + " missed keys on " + std::to_string(pixels) + " pixels per draw, not one record per key" + Describe(before, after));
}

void Deduplicates(const Harness& harness, bool heapCap) {
    RequireMissRing(harness, "(j)");
    const std::array<std::uint32_t, 1> one{FreshEntry};
    std::array<std::uint32_t, 8> eight{};
    for (std::uint32_t index = 0; index < eight.size(); ++index) eight[index] = FreshEntry + 1u + index;
    for (const auto keys : {std::span<const std::uint32_t>(one), std::span<const std::uint32_t>(eight)}) {
        const auto name = "(j) " + std::to_string(keys.size()) + " draws of new keys into a " + std::to_string(LargeSide) + "x" + std::to_string(LargeSide) + " target" + (heapCap ? " on a heap of two slots per kind" : "");
        const auto value = FreshValue(keys.back());
        auto before = Read();
        auto stored = DrawLarge(harness, keys);
        auto after = Read();
        if (!harness.device) {
            RequireCpuTable(before, after, name);
            const auto pixels = RequireLargeDrawn(stored, value, name);
            std::printf("%s: the texel on %zu pixels%s\n", name.c_str(), pixels, Describe(before, after).c_str());
            continue;
        }
        const bool overCap = heapCap && keys.size() > 1u;
        const auto pixels = overCap ? LargeCovered(stored) : RequireLargeDrawn(stored, {}, name + ", first batch");
        RequireMisses(before, after, 1u, name + ", first batch");
        RequireDeduplicated(before, after, keys.size(), pixels, name + ", first batch");
        std::printf("%s, first batch: %s on %zu pixels per draw, %llu miss records, %llu dropped%s\n", name.c_str(), overCap ? "drawn" : "zeros", pixels, static_cast<unsigned long long>(after.device.drained - before.device.drained), static_cast<unsigned long long>(after.device.dropped - before.device.dropped), Describe(before, after).c_str());
        const auto start = before;
        before = after;
        stored = DrawLarge(harness, keys);
        after = Read();
        if (!overCap) {
            RequireHits(before, after, name + ", second batch");
            Require(after.device.fallbacks == before.device.fallbacks, name + ", second batch: the program fell back to the CPU table path");
            RequireLargeDrawn(stored, value, name + ", second batch");
            std::printf("%s, second batch: the texel, no miss records%s\n", name.c_str(), Describe(before, after).c_str());
            continue;
        }
        RequireDeduplicated(before, after, keys.size(), pixels, name + ", second batch");
        Require(after.device.heapFull > start.device.heapFull && after.device.fallbacks == start.device.fallbacks + 1u, name + ": the keys past the heap's two slots did not send the program to the CPU table path in two batches (heap full " + std::to_string(after.device.heapFull - start.device.heapFull) + ", fallbacks " + std::to_string(after.device.fallbacks - start.device.fallbacks) + ")");
        std::printf("%s, second batch: %llu lookups without a slot over both batches, the program falls back once, %llu miss records, %llu dropped%s\n", name.c_str(), static_cast<unsigned long long>(after.device.heapFull - start.device.heapFull), static_cast<unsigned long long>(after.device.drained - before.device.drained), static_cast<unsigned long long>(after.device.dropped - before.device.dropped), Describe(before, after).c_str());
        stored = DrawLarge(harness, keys);
        RequireLargeDrawn(stored, value, name + ", third batch");
        std::printf("%s, third batch: the texel\n", name.c_str());
    }
}

void Streams(const Harness& harness) {
    RequireMissRing(harness, "(k)");
    constexpr std::uint32_t first = FreshEntry + 9u;
    for (std::uint32_t batch = 0; batch <= StreamedKeys; ++batch) {
        std::vector<std::uint32_t> keys;
        if (batch < StreamedKeys) keys.push_back(first + batch);
        if (batch > 0u) keys.push_back(first + batch - 1u);
        const auto name = "(k) streaming, batch " + std::to_string(batch + 1u) + (batch < StreamedKeys ? " with a new key" : "") + (batch > 0u ? " after the previous batch's key" : "");
        const auto before = Read();
        const auto stored = DrawLarge(harness, keys);
        const auto after = Read();
        if (!harness.device) {
            RequireCpuTable(before, after, name);
            RequireLargeDrawn(stored, FreshValue(keys.back()), name);
            std::printf("%s: the texel%s\n", name.c_str(), Describe(before, after).c_str());
            continue;
        }
        Require(after.snapshots == before.snapshots && after.tableBuilds == before.tableBuilds && after.device.prepares > before.device.prepares && after.device.fallbacks == before.device.fallbacks, name + ": the program left the device path" + Describe(before, after));
        const auto pixels = RequireLargeDrawn(stored, batch > 0u ? FreshValue(keys.back()) : std::array<float, 4>{}, name);
        RequireDeduplicated(before, after, batch < StreamedKeys ? 1u : 0u, pixels, name);
        if (batch < StreamedKeys) RequireMisses(before, after, 1u, name);
        std::printf("%s: %s on %zu pixels per draw, %llu miss records, %llu dropped%s\n", name.c_str(), batch > 0u ? "the previous key's texel" : "zeros", pixels, static_cast<unsigned long long>(after.device.drained - before.device.drained), static_cast<unsigned long long>(after.device.dropped - before.device.dropped), Describe(before, after).c_str());
    }
}

std::array<std::uint32_t, 8> TargetDescriptor(const void* data) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (56u << 20u) | (((ColorWidth - 1u) & 3u) << 30u),
        ((ColorWidth - 1u) >> 2u) | ((ColorHeight - 1u) << 14u),
        0xfacu | (static_cast<std::uint32_t>(AgcDriver::Graphics::ColorTileMode::RenderTarget) << 20u) | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

void FillVillage(const GuestBlock& block, const GuestBlock& tableBlock) {
    for (std::uint32_t texture = 0; texture < VillageTextures; ++texture) FillTexture(block.Texture(texture), TexelOf(texture).bytes);
    auto* table = tableBlock.At(0);
    std::memset(table, 0, static_cast<std::size_t>(VillageRecords) * VillageStride);
    for (std::uint32_t record = 0; record < VillageRecords; ++record) {
        const bool target = record >= VillageTargetKey && record < VillageTargetKey + VillageTargets;
        const auto descriptor = target ? TargetDescriptor(block.At(VillageTargetOffset + static_cast<std::size_t>(record - VillageTargetKey) * 0x10000u)) : TextureDescriptor(block.Texture(record % VillageTextures), TexelOf(record % VillageTextures).format);
        std::memcpy(table + static_cast<std::size_t>(record) * VillageStride + VillageAddend, descriptor.data(), sizeof(descriptor));
    }
    for (std::uint32_t target = 0; target < VillageTargets; ++target) std::memset(block.At(VillageTargetOffset + static_cast<std::size_t>(target) * 0x10000u), 0, ColorLayout().Bytes());
}

std::vector<std::uint32_t> VillageFrame(const GuestBlock& block, const GuestBlock& tableBlock, std::uint32_t written, std::uint32_t draws = VillageDraws) {
    const auto setColor = [&](std::vector<std::uint32_t>& commands, const void* target) {
        const auto address = reinterpret_cast<std::uintptr_t>(target);
        Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(address >> 8u)});
        Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(address >> 40u)});
    };
    const auto setPixel = [&](std::vector<std::uint32_t>& commands, const std::uint32_t* code, std::uint32_t userWords) {
        const auto address = reinterpret_cast<std::uintptr_t>(code);
        Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(address >> 8u)});
        Append(commands, 0x76, {0x009, static_cast<std::uint32_t>(address >> 40u)});
        Append(commands, 0x76, {0x00b, userWords << 1u});
    };
    auto commands = DrawCommands(block, 0u, DrawKind::Direct);
    commands.resize(commands.size() - 3u);
    for (std::uint32_t target = 0; target < written; ++target) {
        setColor(commands, block.At(VillageTargetOffset + static_cast<std::size_t>(target) * 0x10000u));
        setPixel(commands, ProducerPixelCode.data(), 0u);
        Append(commands, 0x2d, {3, 2});
    }
    setColor(commands, block.At(VillageColorOffset));
    setPixel(commands, VillagePixelCode.data(), 9u);
    const auto table = BufferDescriptor(tableBlock.At(0), VillageRecords * VillageStride);
    for (std::uint32_t index = 0; index < 4u; ++index) Append(commands, 0x76, {0x00cu + index, table[index]});
    for (std::uint32_t index = 4; index < 8u; ++index) Append(commands, 0x76, {0x00cu + index, 0u});
    const auto arguments = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(DrawArguments.data()));
    Append(commands, 0x11, {1, static_cast<std::uint32_t>(arguments), static_cast<std::uint32_t>(arguments >> 32u)});
    for (std::uint32_t draw = 0; draw < draws; ++draw) {
        const auto key = draw < written ? VillageTargetKey + draw : (draw * 7919u) % VillageTargetKey;
        Append(commands, 0x76, {0x00cu + 8u, key});
        Append(commands, 0x24, {0, 0, 0, 2});
    }
    return commands;
}

int VillageBench(const GuestBlock& block, std::uint32_t written, double seconds) {
    Require(written <= VillageTargets, "at most 32 GPU-written textures");
    GuestBlock tableBlock(TableBlockBytes);
    FillVillage(block, tableBlock);
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(VillagePixelHeader, VillagePixelCode.data(), sizeof(VillagePixelCode), 1);
    Registered(ProducerHeader, ProducerPixelCode.data(), sizeof(ProducerPixelCode), 1);
    auto commands = VillageFrame(block, tableBlock, written);
    const auto frame = [&] {
        std::memset(block.At(VillageColorOffset), static_cast<int>(Kept), ColorLayout().Bytes());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the frame submission failed");
        AgcDriverWaitIdle_nid_postfix();
    };
    const auto describeExpectations = [](const Counters& from, const Counters& to, std::uint32_t frames) {
        const auto expectations = to.device.expectations - from.device.expectations;
        const auto hits = to.device.expectationHits - from.device.expectationHits;
        const auto microseconds = expectations != 0u ? static_cast<double>(to.device.expectationNs - from.device.expectationNs) / 1000.0 / static_cast<double>(expectations) : 0.0;
        char line[256];
        std::snprintf(line, sizeof(line), "%llu expectations, %.2f us each, %.1f%% from the memo; per frame %.1f miss records, %.1f inserted", static_cast<unsigned long long>(expectations), microseconds, expectations != 0u ? 100.0 * static_cast<double>(hits) / static_cast<double>(expectations) : 0.0, static_cast<double>(to.device.drained - from.device.drained) / frames, static_cast<double>(to.device.inserted - from.device.inserted) / frames);
        return std::string(line);
    };
    const auto cold = Read();
    frame();
    const auto first = Read();
    std::printf("village: cold frame: %s\n", describeExpectations(cold, first, 1u).c_str());
    for (std::uint32_t warm = 1; warm < VillageWarmup; ++warm) frame();
    const auto before = Read();
    std::printf("village: the cold frame drained %llu miss records and dropped %llu; the %u warm-up frames drained %llu and dropped %llu, found %llu distinct misses, inserted %llu and fell back %llu times\n", static_cast<unsigned long long>(first.device.drained - cold.device.drained), static_cast<unsigned long long>(first.device.dropped - cold.device.dropped), VillageWarmup, static_cast<unsigned long long>(before.device.drained - cold.device.drained), static_cast<unsigned long long>(before.device.dropped - cold.device.dropped), static_cast<unsigned long long>(before.device.distinct - cold.device.distinct), static_cast<unsigned long long>(before.device.inserted - cold.device.inserted), static_cast<unsigned long long>(before.device.fallbacks - cold.device.fallbacks));
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t measured = 0; measured < VillageFrames; ++measured) frame();
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const auto after = Read();
    std::printf("village: steady frames: %s\n", describeExpectations(before, after, VillageFrames).c_str());
    std::printf("village: %u frames of %u indirect draws (%u GPU-written textures) after %u warm-up frames: %.2f ms per frame, %.1f us per draw (wall clock, submit to idle)%s, %llu entries re-checked, %llu given a new texture\n", VillageFrames, VillageDraws, written, VillageWarmup, elapsed / VillageFrames, elapsed * 1000.0 / (static_cast<double>(VillageFrames) * VillageDraws), Describe(before, after).c_str(), static_cast<unsigned long long>(after.device.checked - before.device.checked), static_cast<unsigned long long>(after.device.refreshed - before.device.refreshed));
    std::fflush(stdout);
    if (written != 0u) {
        auto verify = VillageFrame(block, tableBlock, written, 1u);
        std::memset(block.At(VillageColorOffset), static_cast<int>(Kept), ColorLayout().Bytes());
        Packet packet{verify.data(), static_cast<std::uint32_t>(verify.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the verification submission failed");
        AgcDriverWaitIdle_nid_postfix();
        std::vector<std::byte> stored(ColorLayout().Bytes());
        AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block.At(VillageColorOffset))), stored);
        RequireDrawn(stored, {0.5f, 1.0f, 0.0f, 1.0f}, "village: a draw sampling a GPU-written texture through the table");
        std::printf("village: a draw sampling a GPU-written texture through the table read the producer's texels\n");
    }
    std::uint32_t frames = 0;
    const auto profiled = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - profiled).count() < seconds) {
        frame();
        ++frames;
    }
    std::string peaks;
    for (const auto& totals : DeviceImageTable::DeviceTotals()) {
        for (std::size_t kind = 0; kind < totals.peakTextures.size(); ++kind) {
            if (totals.peakTextures[kind] != 0u) peaks += " " + std::to_string(kind) + ":" + std::to_string(totals.peakTextures[kind]);
        }
    }
    std::printf("village: %u more frames in %.0f s for the profile windows; peak heap textures by kind:%s (of %u per kind)\n", frames, seconds, peaks.empty() ? " none" : peaks.c_str(), ShaderRecompiler::RuntimeAbi::GpuImageHeapSlots);
    std::fflush(stdout);
    return 0;
}

}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool expecting = std::getenv("APS5_NO_DEVICE_IMAGE_EXPECT") == nullptr;
    const bool lazy = mode != "--village-bench" && argc > 3 && std::string(argv[3]) == "lazy";
#ifdef _WIN32
    if (mode != "--village-bench" && !lazy) _putenv_s("APS5_EAGER_WRITEBACK", "1");
    if (mode == "--heap-cap") _putenv_s("APS5_HEAP_SLOT_CAP", "2");
#else
    if (mode != "--village-bench" && !lazy) setenv("APS5_EAGER_WRITEBACK", "1", 1);
    if (mode == "--heap-cap") setenv("APS5_HEAP_SLOT_CAP", "2", 1);
#endif
    try {
        bool served = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            served = device->DeviceImagePath();
        }
        const bool requested = AgcDriver::Graphics::GpuPathEnabled();
        if (requested && !served) {
            std::printf("skipped, the device does not serve the device image path\n");
            return VulkanTestSkipped;
        }
        GuestBlock block;
        try {
            if (mode == "--village-bench") {
                const auto result = VillageBench(block, argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 0u, argc > 3 ? std::stod(argv[3]) : 0.0);
                AgcDriverShutdown_nid_postfix();
                return result;
            }
            FillGuest(block);
            FillFresh(block);
            Harness harness{block, requested || mode == "--expect-device-path", expecting};
            const std::string parts = argc > 2 ? argv[2] : mode == "--heap-cap" ? "f" : mode == "--misses" || (harness.device && !expecting) ? "jkm" : harness.device ? "abcdeghimn" : "abcdeghijkmn";
            std::printf("device image path %s, %s\n", requested ? "on" : "off", harness.device ? "expecting the device table" : "expecting the CPU table");
            Registered(ComputeHeader, ComputeCode.data(), sizeof(ComputeCode), 0);
            Registered(OtherComputeHeader, OtherComputeCode.data(), sizeof(OtherComputeCode), 0);
            Registered(ExecClearHeader, ExecClearCode.data(), sizeof(ExecClearCode), 0);
            Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
            Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
            Registered(DirectPixelHeader, DirectPixelCode.data(), sizeof(DirectPixelCode), 1);
            Registered(LargePixelHeader, LargePixelCode.data(), sizeof(LargePixelCode), 1);
            Registered(ProducerHeader, ProducerPixelCode.data(), sizeof(ProducerPixelCode), 1);
            Registered(OtherProducerHeader, OtherProducerPixelCode.data(), sizeof(OtherProducerPixelCode), 1);
            Registered(LaneKeyComputeHeader, LaneKeyComputeCode.data(), sizeof(LaneKeyComputeCode), 0);
            Registered(LaneKeyPixelHeader, LaneKeyPixelCode.data(), sizeof(LaneKeyPixelCode), 1);
            if (mode == "--heap-cap") {
                if (parts.find('f') != std::string::npos) FallsBackFromWarmCache(harness);
                if (parts.find('j') != std::string::npos) Deduplicates(harness, true);
                if (parts.find('l') != std::string::npos) ComputeMeetsFullHeap(harness);
            } else if (mode == "--one-shot") {
                OneShot(harness);
            } else {
                if (parts.find('a') != std::string::npos) ComputeTable(harness);
                if (parts.find('b') != std::string::npos) Draws(harness);
                if (parts.find('c') != std::string::npos) Freshness(harness);
                if (parts.find('d') != std::string::npos) Lanes(harness);
                if (parts.find('e') != std::string::npos) Faults(harness);
                if (parts.find('g') != std::string::npos) Rechecks(harness);
                if (parts.find('h') != std::string::npos) GpuWrittenTexture(harness);
                if (parts.find('i') != std::string::npos) StaleRecord(harness);
                if (parts.find('j') != std::string::npos) Deduplicates(harness, false);
                if (parts.find('k') != std::string::npos) Streams(harness);
                if (parts.find('m') != std::string::npos) UnrecordedDraw(harness);
                if (parts.find('n') != std::string::npos) Unexpected(harness);
            }
        } catch (const std::exception& error) {
            std::cerr << "device image path harness failed: " << error.what() << '\n';
            AgcDriverShutdown_nid_postfix();
            return 1;
        }
        AgcDriverShutdown_nid_postfix();
        std::cout << "device image path harness passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "device image path harness failed: " << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
