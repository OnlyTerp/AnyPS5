#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Side = 256;
constexpr std::size_t Block = 65536;
constexpr std::size_t SurfaceBytes = 4 * Block;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t TileLinear = 0;
constexpr std::uint32_t TileStandard4KB = 0x05;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Color32FloatInfo = (4u << 2u) | (7u << 8u);
constexpr std::uint32_t TiledAttrib3 = 0x4dc6c000u;
constexpr std::uint32_t Float32Export = 9;
constexpr std::uint32_t Initial = 0x5a5a5a5au;
constexpr std::uint32_t Stored = 0x12345678u;
constexpr std::uint32_t ScissorSide = 64;
constexpr float Drawn = 0.25f;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e040300, 0x7e060280, 0x7e0202ff, Stored, 0xf0200108, 0x00020102, 0xbf810000,
};

alignas(256) std::array<std::uint32_t, 16> PixelCode{};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

class Surface {
public:
    explicit Surface(std::size_t lead = 0) : regionBytes(lead == 0 ? SurfaceBytes : SurfaceBytes + Block) {
#ifdef _WIN32
        region = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(regionBytes, Block));
        if (region != nullptr) GuestArena::GuestArenaCommit_nid_postfix(region, regionBytes, PAGE_READWRITE, regionBytes);
#else
        void* mapped = mmap(nullptr, regionBytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + Block - 1) & ~std::uintptr_t{Block - 1};
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned != begin + Block) munmap(reinterpret_cast<void*>(aligned + regionBytes), begin + Block - aligned);
            region = reinterpret_cast<std::uint8_t*>(aligned);
            if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(region, regionBytes);
        }
#endif
        Require(region != nullptr, "partial image units: cannot allocate the guest surface");
        texels = reinterpret_cast<std::uint32_t*>(region + lead);
        std::fill(texels, texels + SurfaceBytes / 4u, Initial);
        GuestAllocations::Mutation().Add(region, regionBytes, true, true, true);
    }

    ~Surface() {
        GuestAllocations::Mutation().Remove(region);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(region, regionBytes);
        GuestArena::GuestArenaRelease_nid_postfix(region, regionBytes);
#else
        munmap(region, regionBytes);
        if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(region, regionBytes);
#endif
    }

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    std::uint32_t* Texels() const { return texels; }
    std::uint64_t Address() const { return AddressOf(texels); }

private:
    std::size_t regionBytes;
    std::uint8_t* region = nullptr;
    std::uint32_t* texels = nullptr;
};

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint32_t* texels, std::uint32_t swizzleMode) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (swizzleMode << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void StoreInitial(AgcDriver::VulkanDevice& device, const std::uint32_t* texels, std::uint32_t swizzleMode) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels, swizzleMode);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(StoreCode.data()), std::as_bytes(std::span(StoreCode))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{8, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(StoreCode.data()), StoreCode, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(StoreCode.data()));
    device.WaitIdle();
}

void DrawCorner(AgcDriver::VulkanDevice& device, std::uint64_t address) {
    const auto target = device.Target();
    constexpr std::uint32_t waveSize = 64;
    std::size_t at = 0;
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        PixelCode[at++] = 0x7e0002ffu | ((4u + channel) << 17u);
        PixelCode[at++] = std::bit_cast<std::uint32_t>(Drawn);
    }
    PixelCode[at++] = 0xf800180fu;
    PixelCode[at++] = 0x07060504u;
    PixelCode[at++] = 0xbf810000u;
    const std::span<const std::uint32_t> pixelCode(PixelCode.data(), at);
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto triAddress = reinterpret_cast<std::uintptr_t>(Triangle.data());
    const std::array<std::uint32_t, 4> vertexBuffer{static_cast<std::uint32_t>(triAddress), static_cast<std::uint32_t>((triAddress >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(Triangle.size()), 0x01016facu};
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(address >> 8u);
    cx[0x31b] = 0;
    cx[0x31c] = Color32FloatInfo;
    cx[0x31d] = 0;
    cx[0x3b0] = ((Side - 1u) << 14u) | (Side - 1u);
    cx[0x3b8] = (TiledAttrib3 & ~(0x1fu << 14u)) | (TileR64KBX << 14u);
    cx[0x390] = static_cast<std::uint32_t>(address >> 40u);
    const auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    Require(color.format == VK_FORMAT_R32_SFLOAT && color.bytes == SurfaceBytes && color.address == address, "partial image units: the COLOR_32 float target did not decode as 256x256");

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, waveSize, waveSize, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.renderExtent = {Side, Side};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Side), static_cast<float>(Side), -static_cast<float>(Side), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {ScissorSide, ScissorSide}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blends = {blend};
    state.blend = blend;
    state.blendConstants = {};

    const auto codeAddress = reinterpret_cast<std::uintptr_t>(pixelCode.data());
    const AgcDriver::Registers context{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, Float32Export}};
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{codeAddress, std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, codeAddress, pixelCode, 0, {}},
        {waveSize, 0, {}, std::nullopt, AgcDriver::Graphics::DecodePixelStageInfo(context, AgcDriver::Graphics::ExportMappings(state)), std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void TestScissoredTargetUnits(AgcDriver::VulkanDevice& device) {
    Surface surface;
    AgcDriver::GuestMemory::CollectWrites(surface.Address(), SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    DrawCorner(device, surface.Address());
    const auto pending = StorageTexture::FindPending(surface.Address(), SurfaceBytes);
    Require(pending != nullptr, "partial image units: the scissored draw did not leave the target pending");
    const bool untouchedFlushed = StorageTexture::FlushPending(surface.Address() + Block, 3 * Block, nullptr, "untouched target units");
    Require(!untouchedFlushed, "partial image units: a 64x64 scissored draw marked untouched 64 KiB target units pending");
    const auto writeBackBytesBefore = StorageTexture::PartialWriteBackBytes();
    const bool touchedFlushed = StorageTexture::FlushPending(surface.Address(), SurfaceBytes, nullptr, "touched target unit");
    device.WaitIdle();
    const auto storedBytes = StorageTexture::PartialWriteBackBytes() - writeBackBytesBefore;
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(touchedFlushed && storedBytes == Block, "partial image units: flushing the scissored target stored " + std::to_string(storedBytes) + " bytes instead of one 64 KiB unit");
}

void TestSingleUnitPartialUpload(AgcDriver::VulkanDevice& device, std::size_t lead, std::uint32_t swizzleMode, std::size_t writeWordOffset, const char* label) {
    Surface surface(lead);
    AgcDriver::GuestMemory::CollectWrites(surface.Address(), SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard hold(AgcDriver::GuestMemory::GpuMutex());
    StoreInitial(device, surface.Texels(), swizzleMode);
    const auto image = StorageTexture::FindLive(surface.Address(), SurfaceBytes);
    Require(image != nullptr, std::string(label) + ": no live storage image after initial store");
    StorageTexture::FlushAllPending("partial upload fixture setup");
    device.WaitIdle();
    surface.Texels()[writeWordOffset] = 0x33445566u;
    AgcDriver::GuestMemory::CollectWrites(surface.Address(), SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto uploadsBefore = StorageTexture::PartialUploads();
    const auto bytesBefore = StorageTexture::PartialUploadBytes();
    const bool current = image->Refresh();
    device.WaitIdle();
    Require(!current, std::string(label) + ": Refresh reported current after a CPU write");
    const auto uploadsDelta = StorageTexture::PartialUploads() - uploadsBefore;
    const auto bytesDelta = StorageTexture::PartialUploadBytes() - bytesBefore;
    StorageTexture::FlushAllPending("partial upload fixture cleanup");
    device.WaitIdle();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(uploadsDelta == 1 && bytesDelta == Block, std::string(label) + ": partial upload moved " + std::to_string(bytesDelta) + " bytes in " + std::to_string(uploadsDelta) + " partial uploads instead of 65536 bytes in 1 partial upload");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        TestScissoredTargetUnits(*device);
        TestSingleUnitPartialUpload(*device, 0x1000, TileStandard4KB, 2 * (Block / 4u), "unaligned 4KB tiled single-unit surface");
        TestSingleUnitPartialUpload(*device, 0, TileLinear, 2 * (Block / 4u), "linear single-unit surface");
        std::puts("partial image units tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
