#include "../tools/ComputePipelineFixture.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t Captures = 8;
constexpr std::uint32_t Chains = 16;
constexpr std::uint32_t Constants = 64;
constexpr std::uint32_t WordLimit = 40000;
constexpr std::uint32_t StepWords = Chains * 15u;
constexpr double ToleranceMiB = 64.0;
constexpr double ToleranceShare = 0.25;

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

class Module {
public:
    void Op(std::uint32_t opcode, std::initializer_list<std::uint32_t> operands) {
        words.push_back((static_cast<std::uint32_t>(operands.size() + 1u) << 16u) | opcode);
        words.insert(words.end(), operands.begin(), operands.end());
    }

    std::uint32_t Id() { return next++; }
    std::size_t Size() const { return words.size(); }

    std::vector<std::uint32_t> Finish() {
        words[3] = next;
        return words;
    }

private:
    std::vector<std::uint32_t> words{0x07230203u, 0x00010000u, 0u, 0u, 0u};
    std::uint32_t next = 1;
};

std::uint32_t Mix(std::uint64_t& state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(state >> 32u) | 1u;
}

std::vector<std::uint32_t> PaddedSpirv(std::uint64_t seed, std::uint32_t steps) {
    Module module;
    const auto voidType = module.Id();
    const auto functionType = module.Id();
    const auto main = module.Id();
    const auto entry = module.Id();
    const auto uint = module.Id();
    const auto spec = module.Id();
    const auto runtimeArray = module.Id();
    const auto block = module.Id();
    const auto blockPointer = module.Id();
    const auto uintPointer = module.Id();
    const auto variable = module.Id();
    const auto zero = module.Id();
    module.Op(17, {1u});
    module.Op(14, {0u, 1u});
    module.Op(15, {5u, main, 0x6e69616du, 0u});
    module.Op(16, {main, 17u, 1u, 1u, 1u});
    module.Op(71, {spec, 1u, 7u});
    module.Op(71, {runtimeArray, 6u, 4u});
    module.Op(71, {block, 3u});
    module.Op(72, {block, 0u, 35u, 0u});
    module.Op(71, {variable, 34u, 0u});
    module.Op(71, {variable, 33u, 3u});
    module.Op(19, {voidType});
    module.Op(33, {functionType, voidType});
    module.Op(21, {uint, 32u, 0u});
    module.Op(50, {uint, spec, 1u});
    module.Op(29, {runtimeArray, uint});
    module.Op(30, {block, runtimeArray});
    module.Op(32, {blockPointer, 2u, block});
    module.Op(32, {uintPointer, 2u, uint});
    module.Op(59, {blockPointer, variable, 2u});
    module.Op(43, {uint, zero, 0u});
    std::uint64_t state = seed;
    std::vector<std::uint32_t> constants(Constants);
    for (auto& constant : constants) {
        constant = module.Id();
        module.Op(43, {uint, constant, Mix(state)});
    }
    module.Op(54, {voidType, main, 0u, functionType});
    module.Op(248, {entry});
    const auto pointer = module.Id();
    module.Op(65, {uintPointer, pointer, variable, zero, zero});
    const auto loaded = module.Id();
    module.Op(61, {uint, loaded, pointer});
    std::vector<std::uint32_t> chains(Chains);
    for (std::uint32_t chain = 0; chain < Chains; ++chain) {
        chains[chain] = module.Id();
        module.Op(128, {uint, chains[chain], chain == 0u ? spec : loaded, constants[chain % Constants]});
    }
    for (std::uint32_t step = 0; step < steps; ++step) {
        for (std::uint32_t chain = 0; chain < Chains; ++chain) {
            const auto product = module.Id();
            module.Op(132, {uint, product, chains[chain], constants[Mix(state) % Constants]});
            const auto mixed = module.Id();
            module.Op(198, {uint, mixed, product, chains[(chain + 1u) % Chains]});
            const auto sum = module.Id();
            module.Op(128, {uint, sum, mixed, constants[Mix(state) % Constants]});
            chains[chain] = sum;
        }
    }
    auto total = chains[0];
    for (std::uint32_t chain = 1; chain < Chains; ++chain) {
        const auto sum = module.Id();
        module.Op(128, {uint, sum, total, chains[chain]});
        total = sum;
    }
    module.Op(62, {pointer, total});
    module.Op(253, {});
    module.Op(56, {});
    return module.Finish();
}

std::uint32_t Steps() {
    const auto overhead = PaddedSpirv(0u, 0u).size();
    return static_cast<std::uint32_t>((WordLimit - 1000u - overhead) / StepWords);
}

struct Run {
    int exit = -1;
    std::string output;
    std::size_t creations = 0;
    double withDevice = 0.0;
    bool budgetFound = false;
    unsigned long long budget = 0;
    std::string budgetLine;
    bool peakFound = false;
    std::size_t peakCreations = 0;
    double peakPrivate = 0.0;
};

Run Replay(const std::string& tool, const std::string& arguments) {
    Run run;
    const auto command = tool + " " + arguments + " 2>&1";
    auto* pipe = _popen(command.c_str(), "r");
    if (pipe == nullptr) throw std::runtime_error("cannot start the replay tool");
    char buffer[4096];
    for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), pipe)) != 0;) run.output.append(buffer, read);
    run.exit = _pclose(pipe);
    static const std::regex device(R"(\[sequence\] private [0-9.]+ MiB before the device, ([0-9.]+) MiB with it)");
    static const std::regex created(R"(\[sequence\] thread [0-9]+ capture [0-9]+: )");
    static const std::regex budget(R"(\[sequence\] budget ([0-9]+) bytes[^\n]*)");
    static const std::regex peak(R"(\[sequence\] peak ([0-9]+) creations at once, peak private ([0-9.]+) MiB)");
    std::smatch match;
    if (std::regex_search(run.output, match, device)) run.withDevice = std::stod(match[1].str());
    run.creations = static_cast<std::size_t>(std::distance(std::sregex_iterator(run.output.begin(), run.output.end(), created), std::sregex_iterator()));
    if (std::regex_search(run.output, match, budget)) {
        run.budgetFound = true;
        run.budget = std::stoull(match[1].str());
        run.budgetLine = match[0].str();
    }
    if (std::regex_search(run.output, match, peak)) {
        run.peakFound = true;
        run.peakCreations = std::stoul(match[1].str());
        run.peakPrivate = std::stod(match[2].str());
    }
    return run;
}

bool NoDevice(const Run& run) {
    for (const auto* absent : {"Vulkan loader: ", "no Vulkan 1.1 graphics and compute queue", "vkCreateInstance: Vulkan result -9"}) {
        if (run.output.find(absent) != std::string::npos) return std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr;
    }
    return false;
}

bool Completed(const Run& run, const std::string& what) {
    const bool done = run.exit == 0 && run.creations == Captures && run.peakFound && run.budgetFound;
    Expect(done, what + ": the replay did not create every capture and report its budget and peak (exit " + std::to_string(run.exit) + ", " + std::to_string(run.creations) + " of " + std::to_string(Captures) + " creations):\n" + run.output);
    return done;
}

std::string Arguments(std::uint32_t threads, const std::string& options, const std::vector<std::filesystem::path>& captures) {
    std::string arguments = "--sequence " + std::to_string(threads) + " 0" + options;
    for (const auto& capture : captures) arguments += " " + capture.string();
    return arguments;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: agc_compute_pipeline_budget_tests REPLAY_TOOL\n");
        return 2;
    }
    const std::string tool = std::filesystem::path(argv[1]).make_preferred().string();
    const auto nonce = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("aps5-pipeline-budget-" + std::to_string(nonce));
    int result = 0;
    try {
        if (tool.find(' ') != std::string::npos || root.string().find(' ') != std::string::npos) throw std::runtime_error("the replay tool and the temporary directory must have paths without spaces");
        const auto steps = Steps();
        std::vector<std::vector<std::filesystem::path>> sets(3);
        std::size_t largest = 0;
        for (std::uint32_t set = 0; set < sets.size(); ++set) {
            for (std::uint32_t index = 0; index < Captures; ++index) {
                auto capture = AgcDriver::ComputePipelineFixture();
                capture.spirv = PaddedSpirv(nonce ^ (std::uint64_t{set + 1u} << 40u) ^ (std::uint64_t{index + 1u} << 48u), steps);
                if (capture.spirv.size() >= WordLimit) throw std::runtime_error("a padded module reached the word limit");
                largest = std::max(largest, capture.spirv.size());
                sets[set].push_back(capture.Write(root / std::to_string(set)).make_preferred());
            }
        }
        std::printf("compute pipeline budget: %u captures per run of up to %zu words\n", Captures, largest);
        const auto single = Replay(tool, Arguments(1u, "", sets[0]));
        if (single.exit != 0 && NoDevice(single)) {
            std::printf("skipped, no usable Vulkan device\n");
            std::filesystem::remove_all(root);
            return 77;
        }
        const auto budgeted = Replay(tool, Arguments(4u, " --budget-creations 1", sets[1]));
        const auto open = Replay(tool, Arguments(4u, "", sets[2]));
        if (Completed(single, "one thread") && Completed(budgeted, "four threads with a budget of one creation") && Completed(open, "four threads with the default budget")) {
            const auto singleGrowth = single.peakPrivate - single.withDevice;
            const auto budgetedGrowth = budgeted.peakPrivate - budgeted.withDevice;
            const auto allowed = singleGrowth * (1.0 + ToleranceShare) + ToleranceMiB;
            std::printf("compute pipeline budget: one thread peak %zu creations, %.1f MiB over the device; four threads under one creation's budget peak %zu, %.1f MiB; four threads under the default budget peak %zu, %.1f MiB\n", single.peakCreations, singleGrowth, budgeted.peakCreations, budgetedGrowth, open.peakCreations, open.peakPrivate - open.withDevice);
            Expect(single.peakCreations == 1u, "one thread: the replay reported " + std::to_string(single.peakCreations) + " creations at once");
            Expect(budgeted.budgetLine.find(": 1 x the largest estimate " + std::to_string(budgeted.budget) + " bytes (" + std::to_string(largest) + " words)") != std::string::npos, "four threads with a budget of one creation: the budget is not one estimate of the largest capture: " + budgeted.budgetLine);
            Expect(budgeted.peakCreations == 1u, "four threads with a budget of one creation: " + std::to_string(budgeted.peakCreations) + " creations ran at once, expected one at a time");
            Expect(budgetedGrowth <= allowed, "four threads with a budget of one creation: the peak private memory grew " + std::to_string(budgetedGrowth) + " MiB over the device, the one-thread run's " + std::to_string(singleGrowth) + " MiB, allowed " + std::to_string(allowed) + " MiB");
            Expect(open.budgetLine.find("(default)") != std::string::npos, "four threads without a budget: the budget is not the default: " + open.budgetLine);
            Expect(open.peakCreations > 1u, "four threads with the default budget: " + std::to_string(open.peakCreations) + " creations ran at once, so these captures cannot show the gate holding creations back");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: compute pipeline budget: %s\n", error.what());
        ++failures;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (failures != 0) {
        std::fprintf(stderr, "%d compute pipeline budget checks failed\n", failures);
        result = 1;
    } else {
        std::printf("compute pipeline budget tests passed\n");
    }
    return result;
}
