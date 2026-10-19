#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <span>
#include <string>

namespace {

using AgcDriver::Graphics::PipelineLibraryCounters;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Extent = 64u;
constexpr std::size_t PreRasterization = 1;
constexpr std::size_t FragmentShader = 2;

alignas(4096) std::array<std::byte, Extent * Extent * 4u> Pixels{};
const std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 5> RedPixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 5> GreenPixelCode{0x7e000280u, 0x7e0202f2u, 0xf800180fu, 0x01000100u, 0xbf810000u};

struct Shaders {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult pixel;
    std::uint32_t pixelPush = 0;

    std::array<AgcDriver::Graphics::CompiledShader, 2> Stages() const {
        return {{{ShaderStage::Vertex, &vertex, 0u}, {ShaderStage::Fragment, &pixel, pixelPush}}};
    }
};

std::array<std::uint32_t, 4> VertexBuffer() {
    const auto address = reinterpret_cast<std::uintptr_t>(Triangle.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(Triangle.size()), 0x01016facu};
}

Shaders Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> pixelCode) {
    const auto target = device.Target();
    Shaders shaders;
    const auto userData = VertexBuffer();
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    shaders.vertex = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(shaders.vertex.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.targetOutputMode[0] = 9u;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(pixelCode.data()), std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    shaders.pixel = ShaderRecompiler::Recompile(fragment);
    shaders.pixelPush = PixelPushOffset(vertexPush, target);
    return shaders;
}

AgcDriver::Graphics::State DrawState() {
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Extent, Extent}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Extent, Extent};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0.0f, static_cast<float>(Extent), static_cast<float>(Extent), -static_cast<float>(Extent), 0.0f, 1.0f};
    state.scissor = {{0, 0}, {Extent, Extent}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15u;
    state.blends = {state.blend};
    return state;
}

void DrawLocked(AgcDriver::VulkanDevice& device, const AgcDriver::Graphics::State& state, const Shaders& shaders, std::array<std::uint8_t, 4> color, const std::string& what) {
    Pixels.fill(std::byte{0});
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto stages = shaders.Stages();
    {
        const std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        device.Draw(state, {0u, static_cast<std::uint32_t>(Triangle.size()), 0u, 1u, 0u, false}, stages);
    }
    device.WaitIdle();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Pixels.data()), Pixels.size());
    for (std::size_t index = 0; index < Pixels.size(); index += 4u) {
        for (std::size_t channel = 0; channel < 4u; ++channel) {
            const auto value = std::to_integer<std::uint8_t>(Pixels[index + channel]);
            Require(value == color[channel], what + ": pixel " + std::to_string(index / 4u) + " channel " + std::to_string(channel) + " is " + std::to_string(value) + ", expected " + std::to_string(color[channel]));
        }
    }
}

std::string Parts(const PipelineLibraryCounters& before, const PipelineLibraryCounters& after, bool locked) {
    std::string text;
    for (std::size_t part = 0; part < 4u; ++part) {
        const auto delta = locked ? after.builtUnderLock[part] - before.builtUnderLock[part] : after.built[part] - before.built[part];
        text += (part == 0u ? "" : "/") + std::to_string(delta);
    }
    return text;
}

int Run(AgcDriver::VulkanDevice& device) {
    const auto counters = [&] { return AgcDriver::Graphics::PipelineLibraryCountersOf(device.Device()); };
    const auto state = DrawState();
    const auto red = Compile(device, RedPixelCode);
    const auto redStages = red.Stages();

    const auto before = counters();
    if (!device.PrepareDrawPipelines(state, redStages)) {
        std::puts("skipped, the device builds no graphics pipeline libraries");
        return VulkanTestSkipped;
    }
    const auto prepared = counters();
    Require(prepared.built[PreRasterization] == before.built[PreRasterization] + 1u && prepared.built[FragmentShader] == before.built[FragmentShader] + 1u, "preparing a new shader pair built " + Parts(before, prepared, false) + " library parts (vertex input/pre-rasterization/fragment shader/fragment output), expected one pre-rasterization and one fragment-shader part");
    Require(prepared.builtUnderLock == before.builtUnderLock, "preparing a new shader pair outside the GPU lock counted " + Parts(before, prepared, true) + " parts as built under the lock");
    Require(prepared.linked == before.linked, "preparing a shader pair linked a pipeline");

    DrawLocked(device, state, red, {255u, 0u, 0u, 255u}, "the draw of a prepared shader pair");
    const auto drawn = counters();
    Require(drawn.built[PreRasterization] == prepared.built[PreRasterization] && drawn.built[FragmentShader] == prepared.built[FragmentShader], "the draw of a prepared shader pair built " + Parts(prepared, drawn, false) + " library parts, expected no shader part");
    Require(drawn.builtUnderLock[PreRasterization] == prepared.builtUnderLock[PreRasterization] && drawn.builtUnderLock[FragmentShader] == prepared.builtUnderLock[FragmentShader], "the draw of a prepared shader pair built " + Parts(prepared, drawn, true) + " parts under the GPU lock, expected no shader part");
    Require(drawn.linked == prepared.linked + 1u, "the draw of a prepared shader pair linked " + std::to_string(drawn.linked - prepared.linked) + " pipelines, expected one");

    Require(device.PrepareDrawPipelines(state, redStages), "preparing a shader pair whose parts exist reported no library path");
    const auto again = counters();
    Require(again.built == drawn.built && again.builtUnderLock == drawn.builtUnderLock && again.linked == drawn.linked, "preparing a shader pair whose parts exist built " + Parts(drawn, again, false) + " parts and linked " + std::to_string(again.linked - drawn.linked) + " pipelines");

    const auto green = Compile(device, GreenPixelCode);
    DrawLocked(device, state, green, {0u, 255u, 0u, 255u}, "the draw of an unprepared pixel shader");
    const auto unprepared = counters();
    Require(unprepared.builtUnderLock[FragmentShader] == again.builtUnderLock[FragmentShader] + 1u && unprepared.builtUnderLock[PreRasterization] == again.builtUnderLock[PreRasterization], "the locked draw of a new pixel shader with a known vertex shader built " + Parts(again, unprepared, true) + " parts under the GPU lock, expected one fragment-shader part");
    std::puts("draw pipeline preparation tests passed");
    return 0;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        return Run(*device);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
