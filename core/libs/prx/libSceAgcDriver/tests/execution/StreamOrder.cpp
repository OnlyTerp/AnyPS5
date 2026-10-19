#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t RegionBytes = std::size_t{256} << 20u;
constexpr std::size_t TailBytes = 4096;
constexpr std::uint32_t ComputeQueue = 0x20;

class GuestRegion {
public:
    GuestRegion() {
#ifdef _WIN32
        region = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, RegionBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        region = static_cast<std::uint8_t*>(std::aligned_alloc(65536, RegionBytes));
#endif
        Require(region != nullptr, "stream order: cannot allocate the guest region");
        std::memset(region, 0, RegionBytes);
        GuestAllocations::Mutation().Add(region, RegionBytes, true, true, true);
    }

    ~GuestRegion() {
        GuestAllocations::Mutation().Remove(region);
#ifdef _WIN32
        VirtualFree(region, 0, MEM_RELEASE);
#else
        std::free(region);
#endif
    }

    GuestRegion(const GuestRegion&) = delete;
    GuestRegion& operator=(const GuestRegion&) = delete;

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(region)); }

    std::uint32_t TailWord(std::size_t index) const {
        std::uint32_t word = 0;
        std::memcpy(&word, region + RegionBytes - TailBytes + index * 4, 4);
        return word;
    }

    std::uint64_t Tail(std::size_t tail) const { return Address() + RegionBytes - (tail + 1) * TailBytes; }

private:
    std::uint8_t* region = nullptr;
};

std::array<std::uint32_t, 4> Pattern(std::uint32_t word) {
    return {word, word, word, word};
}

constexpr std::array<std::uint32_t, 4> Repeated{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};

}

int main() {
    try {
        GuestRegion region;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        bool imported = true;
        std::thread compute([&] {
            AgcDriver::GuestMemory::TagGpuLockThread(ComputeQueue);
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            imported = device->FillBuffer(region.Address(), RegionBytes - 2 * TailBytes, Pattern(0xaaaaaaaau)) && device->FillBuffer(region.Tail(1), TailBytes, Repeated);
        });
        compute.join();
        if (!imported) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        std::size_t streams = 0;
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            Require(device->FillBuffer(region.Tail(0), TailBytes, Repeated), "stream order: the graphics queue's fill was not recorded on the GPU");
            device->SubmitRecorded(false);
            device->WaitIdle();
            streams = AgcDriver::Graphics::Recorder::Active()->Streams();
        }
        for (std::size_t index = 0; index < TailBytes / 4; ++index) {
            if (region.TailWord(index) == Repeated[index % 4]) continue;
            std::array<char, 256> message{};
            std::snprintf(message.data(), message.size(), "stream order: word %zu of the fill that reuses another queue's pattern buffer holds 0x%08x, not 0x%08x (%zu recorder streams)", index, region.TailWord(index), Repeated[index % 4], streams);
            Require(false, message.data());
        }
        std::printf("stream order tests passed (%zu recorder streams)\n", streams);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
