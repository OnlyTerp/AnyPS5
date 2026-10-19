#ifndef CORE_SHADER_RECOMPILER_SPIRVFUNCTIONMATCH_HPP
#define CORE_SHADER_RECOMPILER_SPIRVFUNCTIONMATCH_HPP

#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace ShaderRecompiler {

struct SpirvFunctionBody {
    std::uint32_t resultType = 0;
    std::vector<std::uint32_t> parameters;
    std::vector<std::uint32_t> parameterTypes;
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> annotations;
};

[[nodiscard]] std::unordered_set<std::uint32_t> SpirvFunctionLocalIds(std::span<const std::uint32_t> parameters, std::span<const std::uint32_t> words);
[[nodiscard]] bool SameSpirvFunctionBody(const SpirvFunctionBody& first, const SpirvFunctionBody& second, std::uint32_t switchSelectorType);

}

#endif
