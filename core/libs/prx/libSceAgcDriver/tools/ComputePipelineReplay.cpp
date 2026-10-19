#include "ComputePipelineFixture.hpp"
#include "prx/libSceAgcDriver/Execution/include/PipelineCreation.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

struct Options {
    std::filesystem::path source;
    std::filesystem::path cacheIn;
    std::filesystem::path cacheOut;
    std::filesystem::path output;
    std::filesystem::path fixtureDirectory;
    bool fixture = false;
};

Options Parse(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto value = [&](std::filesystem::path& destination) {
            if (!destination.empty() || ++index == argc || *argv[index] == '\0') throw std::runtime_error("missing or repeated option: " + std::string(argument));
            destination = argv[index];
        };
        if (argument == "--cache-in") value(result.cacheIn);
        else if (argument == "--cache-out") value(result.cacheOut);
        else if (argument == "--output") value(result.output);
        else if (argument == "--write-fixture") value(result.fixtureDirectory);
        else if (argument == "--fixture" && !result.fixture) result.fixture = true;
        else if (argument.starts_with("--") || !result.source.empty()) throw std::runtime_error("unknown or repeated argument: " + std::string(argument));
        else result.source = argument;
    }
    const auto sources = static_cast<int>(!result.source.empty()) + static_cast<int>(result.fixture) + static_cast<int>(!result.fixtureDirectory.empty());
    if (sources != 1) throw std::runtime_error("select a capture file, --fixture, or --write-fixture DIRECTORY");
    if (!result.fixtureDirectory.empty() && (!result.cacheIn.empty() || !result.cacheOut.empty() || !result.output.empty())) throw std::runtime_error("--write-fixture cannot be combined with replay options");
    for (const auto& path : {result.cacheOut, result.output}) {
        if (!path.empty() && std::filesystem::exists(path)) throw std::runtime_error("output already exists: " + path.string());
    }
    if (!result.cacheOut.empty() && !result.output.empty() && std::filesystem::absolute(result.cacheOut).lexically_normal() == std::filesystem::absolute(result.output).lexically_normal()) throw std::runtime_error("cache and report must use different paths");
    return result;
}

std::vector<std::byte> ReadCache(const std::filesystem::path& path) {
    if (path.empty()) return {};
    const auto bytes = std::filesystem::file_size(path);
    if (bytes == 0 || bytes > 64u * 1024u * 1024u) throw std::runtime_error("imported pipeline cache must contain 1 byte to 64 MiB");
    std::vector<std::byte> result(static_cast<std::size_t>(bytes));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("could not read complete pipeline cache: " + path.string());
    return result;
}

void WriteBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    if (std::filesystem::exists(path)) throw std::runtime_error("output already exists: " + path.string());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("could not write complete output: " + path.string());
}

std::string Quote(std::string_view text) {
    std::ostringstream result;
    result << '"';
    for (const unsigned char character : text) {
        if (character == '\\' || character == '"') result << '\\' << character;
        else if (character < 0x20u) result << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character);
        else result << character;
    }
    result << '"';
    return result.str();
}

std::string Uuid(std::span<const std::uint8_t> bytes) {
    std::ostringstream result;
    for (const auto byte : bytes) result << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return result.str();
}

template<typename TDevice>
void WriteDevice(std::ostream& output, const TDevice& device) {
    output << "{\"name\":" << Quote(device.deviceName)
           << ",\"vendor_id\":" << device.vendorId << ",\"device_id\":" << device.deviceId
           << ",\"driver_version_raw\":" << device.driverVersion << ",\"api_version_raw\":" << device.apiVersion
           << ",\"pipeline_cache_uuid\":" << Quote(Uuid(device.pipelineCacheUuid)) << '}';
}

std::string Report(const Options& options, const AgcDriver::ComputePipelineCapture& capture, const AgcDriver::ComputePipelineReplayResult& replay) {
    std::ostringstream output;
    output << std::setprecision(12)
           << "{\n  \"schema_version\":1,\n  \"source\":" << Quote(options.fixture ? "synthetic-no-op-compute" : options.source.string())
           << ",\n  \"capture_identity\":" << Quote(capture.Identity())
           << ",\n  \"capture_identity_algorithm\":\"fnv1a64\""
           << ",\n  \"spirv_words\":" << capture.spirv.size()
           << ",\n  \"descriptor_sets\":" << capture.sets.size()
           << ",\n  \"push_constant_ranges\":" << capture.pushConstants.size()
           << ",\n  \"specialization_constants\":" << capture.specialization.size()
           << ",\n  \"required_subgroup_size\":" << capture.requiredSubgroupSize
           << ",\n  \"pipeline_flags\":" << capture.flags
           << ",\n  \"stage_flags\":" << capture.stageFlags
           << ",\n  \"captured_device\":";
    WriteDevice(output, capture);
    output << ",\n  \"replay_device\":";
    WriteDevice(output, replay);
    output << ",\n  \"application_cache_policy\":" << Quote(options.cacheIn.empty() ? "fresh" : "imported")
           << ",\n  \"cache_import_path\":" << Quote(options.cacheIn.string())
           << ",\n  \"cache_import_bytes\":" << replay.initialCacheBytes
           << ",\n  \"cache_export_path\":" << Quote(options.cacheOut.string())
           << ",\n  \"cache_export_bytes\":" << (options.cacheOut.empty() ? 0u : replay.cache.size())
           << ",\n  \"driver_cache_state\":\"uncontrolled\""
           << ",\n  \"dispatch_count\":0,\n  \"pipeline_statistics\":false"
           << ",\n  \"milliseconds\":{\"shader_module\":" << replay.moduleMilliseconds
           << ",\"layout\":" << replay.layoutMilliseconds << ",\"pipeline_create\":" << replay.pipelineMilliseconds
           << ",\"cache\":" << replay.cacheMilliseconds << ",\"replay_total\":" << replay.totalMilliseconds << "}\n}\n";
    return output.str();
}

}

std::size_t PrivateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    return counters.PrivateUsage;
#else
    std::ifstream statm("/proc/self/statm");
    std::size_t pages = 0;
    std::size_t resident = 0;
    statm >> pages >> resident;
    return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
}

int Sequence(int argc, char** argv) {
    using AgcDriver::CreationGate;
    const auto threads = static_cast<std::size_t>(std::stoul(argv[2]));
    const auto hold = std::stoul(argv[3]);
    int first = 4;
    std::size_t budgetCreations = 0;
    if (argc >= 7 && std::string_view(argv[4]) == "--budget-creations") {
        budgetCreations = static_cast<std::size_t>(std::stoul(argv[5]));
        if (budgetCreations == 0) throw std::runtime_error("--budget-creations needs at least one creation");
        first = 6;
    }
    std::vector<AgcDriver::ComputePipelineCapture> captures;
    for (int index = first; index < argc; ++index) captures.push_back(AgcDriver::ComputePipelineCapture::Read(argv[index]));
    std::size_t largest = 0;
    for (const auto& capture : captures) largest = std::max(largest, capture.spirv.size());
    if (budgetCreations != 0) {
        CreationGate::SetBudgetBytes(budgetCreations * CreationGate::EstimateBytes(largest));
        std::printf("[sequence] budget %zu bytes: %zu x the largest estimate %zu bytes (%zu words)\n", CreationGate::BudgetWords() * CreationGate::CompilerBytesPerWord, budgetCreations, CreationGate::EstimateBytes(largest), largest);
    } else {
        std::printf("[sequence] budget %zu bytes (default)\n", CreationGate::BudgetWords() * CreationGate::CompilerBytesPerWord);
    }
    const auto before = PrivateBytes();
    AgcDriver::VulkanDevice device(AgcDriver::VulkanDevice::CompileOnly{});
    const auto opened = PrivateBytes();
    std::printf("[sequence] private %.1f MiB before the device, %.1f MiB with it; %zu captures on %zu threads\n", before / 1048576.0, opened / 1048576.0, captures.size(), threads);
    std::atomic<std::size_t> next{0};
    std::atomic<bool> running{true};
    std::atomic<std::size_t> peakPrivate{PrivateBytes()};
    std::thread sampler([&] {
        while (running.load(std::memory_order_relaxed)) {
            const auto current = PrivateBytes();
            if (current > peakPrivate.load(std::memory_order_relaxed)) peakPrivate.store(current, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::mutex print;
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            const AgcDriver::CreationAhead ahead;
            for (std::size_t index = next++; index < captures.size(); index = next++) {
                const auto replay = device.ReplayComputePipeline(captures[index]);
                std::lock_guard lock(print);
                std::printf("[sequence] thread %zu capture %zu: %zu words, %.0f ms, private after %.1f MiB\n", worker, index, captures[index].spirv.size(), replay.pipelineMilliseconds, PrivateBytes() / 1048576.0);
                std::fflush(stdout);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    running = false;
    sampler.join();
    std::printf("[sequence] peak %zu creations at once, peak private %.1f MiB\n", CreationGate::PeakCreations(), peakPrivate.load() / 1048576.0);
    std::printf("[sequence] all done: private %.1f MiB; holding %lu s\n", PrivateBytes() / 1048576.0, hold);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(hold));
    std::printf("[sequence] after hold: private %.1f MiB\n", PrivateBytes() / 1048576.0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string_view(argv[1]) == "--sequence") return Sequence(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "agc_compute_pipeline_replay CAPTURE|--fixture [--cache-in FILE] [--cache-out FILE] [--output FILE]\n"
                     "agc_compute_pipeline_replay --write-fixture DIRECTORY\n"
                     "Compiles one compute pipeline without dispatch. Fresh application cache does not clear driver caches.\n";
        return 0;
    }
    try {
        const auto options = Parse(argc, argv);
        if (!options.fixtureDirectory.empty()) {
            std::cout << AgcDriver::ComputePipelineFixture().Write(options.fixtureDirectory).string() << '\n';
            return 0;
        }
        const auto* statistics = std::getenv("APS5_PIPELINE_STATS");
        if (statistics != nullptr && std::string_view(statistics) != "0" && *statistics != '\0') throw std::runtime_error("unset APS5_PIPELINE_STATS before benchmarking");
        const auto capture = options.fixture ? AgcDriver::ComputePipelineFixture() : AgcDriver::ComputePipelineCapture::Read(options.source);
        if ((capture.flags & VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR) != 0) throw std::runtime_error("capture requests pipeline statistics; capture again with APS5_PIPELINE_STATS disabled");
        const auto cache = ReadCache(options.cacheIn);
        AgcDriver::VulkanDevice device(AgcDriver::VulkanDevice::CompileOnly{});
        const auto replay = device.ReplayComputePipeline(capture, cache);
        if (!options.cacheOut.empty()) WriteBytes(options.cacheOut, replay.cache);
        const auto report = Report(options, capture, replay);
        if (!options.output.empty()) WriteBytes(options.output, std::as_bytes(std::span(report.data(), report.size())));
        std::cout << report;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "compute pipeline replay: " << error.what() << '\n';
        return 1;
    }
}
