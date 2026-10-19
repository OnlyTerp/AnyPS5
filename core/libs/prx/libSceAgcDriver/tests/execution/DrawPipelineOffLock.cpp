#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using AgcDriver::Graphics::PipelineLibraryCounters;
using AgcDriver::Graphics::Require;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 64;
constexpr std::uint32_t SetContextReg = 0x69u;
constexpr std::uint32_t SetShReg = 0x76u;
constexpr std::uint32_t SetUconfigReg = 0x79u;
constexpr std::uint32_t DrawIndexAuto = 0x2du;
constexpr std::size_t PreRasterization = 1;
constexpr std::size_t FragmentShader = 2;
constexpr std::size_t ColorBytes = Width * Height * 4;
constexpr std::size_t Block = 65536;

alignas(256) constexpr std::array<std::uint32_t, 6> WarmVertexCode{0xe0382000u, 0x80020005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> VertexCode{0xe0382000u, 0x80020005u, 0xbf8c3f70u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> WarmPixelCode{0x7e0002f2u, 0x7e0202f2u, 0x7e0402f2u, 0x7e0602f2u, 0xf800080fu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{0x7e0002f2u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf800080fu, 0x03020100u, 0xbf810000u};

std::byte* const ColorMemory = [] {
#ifdef _WIN32
    auto* ptr = static_cast<std::byte*>(VirtualAlloc(nullptr, 2 * Block, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* ptr = static_cast<std::byte*>(std::aligned_alloc(Block, 2 * Block));
#endif
    Require(ptr != nullptr, "cannot allocate color target memory");
    return ptr;
}();
const std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};
std::array<std::uint32_t, 1> EndOfPipe{};

struct RegisteredShader {
    Shader shader;
    ShaderUserData userData;
};
RegisteredShader WarmVertex{};
RegisteredShader WarmPixel{};
RegisteredShader Vertex{};
RegisteredShader Pixel{};

const std::vector<std::pair<std::uint32_t, std::uint32_t>> ContextBaseline{
    {0x2d5, 0x2000},
    {0x1b6, 0}, {0x200, 0}, {0x203, 0x800},
    {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
    {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
    {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
    {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
    {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
    {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
    {0x31b, 0}, {0x31d, 0}, {0x3b8, 0x9000000}, {0x1e0, 0},
    {0xc, 0},
    {0x81, 0x80000000}, {0x90, 0x80000000}, {0x94, 0x80000000},
    {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0x113, std::bit_cast<std::uint32_t>(1.0f)},
    {0x207, 0x00400000},
};

void Register(RegisteredShader& registered, const void* code, std::uint32_t bytes, std::uint8_t type) {
    registered.shader.file_header = 0x34333231u;
    registered.shader.version = 0x18u;
    registered.shader.user_data = &registered.userData;
    registered.shader.code = code;
    registered.shader.header_size = sizeof(RegisteredShader);
    registered.shader.shader_size = bytes;
    registered.shader.type = type;
    AgcDriverRegisterShader_nid_postfix(&registered.shader);
}

void SetRegister(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    words.push_back(0xc0010000u | (opcode << 8u));
    words.push_back(offset);
    words.push_back(value);
}

std::vector<std::uint32_t> Frame(const void* vertexCode, const void* pixelCode) {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(vertexCode);
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(pixelCode);
    std::vector<std::uint32_t> words;
    for (const auto& [offset, value] : ContextBaseline) SetRegister(words, SetContextReg, offset, value);
    SetRegister(words, SetContextReg, 0x318, static_cast<std::uint32_t>(colorAddress >> 8u));
    SetRegister(words, SetContextReg, 0x390, static_cast<std::uint32_t>(colorAddress >> 40u));
    SetRegister(words, SetContextReg, 0x3b0, ((Width - 1u) << 14u) | (Height - 1u));
    SetRegister(words, SetContextReg, 0x10f, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x110, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x111, std::bit_cast<std::uint32_t>(-(Height / 2.0f)));
    SetRegister(words, SetContextReg, 0x112, std::bit_cast<std::uint32_t>(Height / 2.0f));
    const auto extent = (Height << 16u) | Width;
    SetRegister(words, SetContextReg, 0xd, extent);
    SetRegister(words, SetContextReg, 0x82, extent);
    SetRegister(words, SetContextReg, 0x91, extent);
    SetRegister(words, SetContextReg, 0x95, extent);
    SetRegister(words, SetUconfigReg, 0x242, 4u);
    SetRegister(words, SetShReg, 0x008, static_cast<std::uint32_t>(pixelAddress >> 8u));
    SetRegister(words, SetShReg, 0x009, static_cast<std::uint32_t>(pixelAddress >> 40u));
    SetRegister(words, SetShReg, 0x00b, 0u);
    SetRegister(words, SetShReg, 0x0c8, static_cast<std::uint32_t>(vertexAddress >> 8u));
    SetRegister(words, SetShReg, 0x0c9, static_cast<std::uint32_t>(vertexAddress >> 40u));
    SetRegister(words, SetShReg, 0x08b, 4u << 1u);
    const auto triangle = reinterpret_cast<std::uintptr_t>(Triangle.data());
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(triangle), static_cast<std::uint32_t>((triangle >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(Triangle.size()), 0x01016facu};
    for (std::uint32_t i = 0; i < descriptor.size(); ++i) SetRegister(words, SetShReg, 0x08c + i, descriptor[i]);
    words.insert(words.end(), {0xc0000000u | (1u << 16u) | (DrawIndexAuto << 8u), static_cast<std::uint32_t>(Triangle.size()), 2u});
    const auto labelAddress = reinterpret_cast<std::uintptr_t>(EndOfPipe.data());
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(labelAddress), static_cast<std::uint32_t>(static_cast<std::uint64_t>(labelAddress) >> 32u), 1u, 0u, 0u});
    return words;
}

void Draw(const void* vertexCode, const void* pixelCode, std::array<std::uint8_t, 4> color, const std::string& what) {
    std::fill_n(ColorMemory, ColorBytes, std::byte{0});
    EndOfPipe[0] = 0u;
    auto words = Frame(vertexCode, pixelCode);
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, what + ": the DCB submission failed");
    AgcDriverWaitIdle_nid_postfix();
    Require(EndOfPipe[0] == 1u, what + ": the end-of-pipe label was not written");
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    for (std::size_t index = 0; index < colors.size(); index += 4u) {
        for (std::size_t channel = 0; channel < 4u; ++channel) {
            const auto value = std::to_integer<std::uint8_t>(colors[index + channel]);
            Require(value == color[channel], what + ": pixel " + std::to_string(index / 4u) + " channel " + std::to_string(channel) + " is " + std::to_string(value) + ", expected " + std::to_string(color[channel]));
        }
    }
}

PipelineLibraryCounters Counters() {
    VkDevice device = VK_NULL_HANDLE;
    {
        const std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        Require(recorder != nullptr, "the driver opened no device");
        device = recorder->Device();
    }
    return AgcDriver::Graphics::PipelineLibraryCountersOf(device);
}

std::string Parts(const PipelineLibraryCounters& before, const PipelineLibraryCounters& after, bool locked) {
    std::string text;
    for (std::size_t part = 0; part < 4u; ++part) {
        const auto delta = locked ? after.builtUnderLock[part] - before.builtUnderLock[part] : after.built[part] - before.built[part];
        text += (part == 0u ? "" : "/") + std::to_string(delta);
    }
    return text;
}

int Run() {
    Register(WarmVertex, WarmVertexCode.data(), sizeof(WarmVertexCode), 2);
    Register(WarmPixel, WarmPixelCode.data(), sizeof(WarmPixelCode), 1);
    Register(Vertex, VertexCode.data(), sizeof(VertexCode), 2);
    Register(Pixel, PixelCode.data(), sizeof(PixelCode), 1);
    Draw(WarmVertexCode.data(), WarmPixelCode.data(), {255u, 255u, 255u, 255u}, "the warm-up draw");
    const auto warm = Counters();
    if (warm.linked == 0u) {
        std::puts("skipped, the device links no pipelines from graphics pipeline libraries");
        return VulkanTestSkipped;
    }
    Draw(VertexCode.data(), PixelCode.data(), {255u, 0u, 0u, 255u}, "the first draw of a new program pair");
    const auto first = Counters();
    Require(first.built[PreRasterization] == warm.built[PreRasterization] + 1u && first.built[FragmentShader] == warm.built[FragmentShader] + 1u, "the first draw of a new program pair built " + Parts(warm, first, false) + " library parts (vertex input/pre-rasterization/fragment shader/fragment output), expected one pre-rasterization and one fragment-shader part");
    Require(first.builtUnderLock == warm.builtUnderLock, "the first draw of a new program pair built " + Parts(warm, first, true) + " library parts while holding the GPU lock");
    Draw(VertexCode.data(), PixelCode.data(), {255u, 0u, 0u, 255u}, "a second identical draw");
    const auto second = Counters();
    Require(second.built == first.built && second.builtUnderLock == first.builtUnderLock, "a second identical draw built " + Parts(first, second, false) + " library parts, " + Parts(first, second, true) + " of them under the GPU lock");
    std::puts("draw pipeline off-lock tests passed");
    return 0;
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const auto result = Run();
        AgcDriverShutdown_nid_postfix();
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception&) {
        }
        return 1;
    }
}
