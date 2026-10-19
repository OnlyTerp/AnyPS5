#include "SpirvBackend/SpirvFunctionMatch.hpp"
#include <spirv/unified1/spirv.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ShaderRecompiler;

namespace {

constexpr std::uint32_t U32 = 1;
constexpr std::uint32_t U32x2 = 2;
constexpr std::uint32_t ConstantA = 3;
constexpr std::uint32_t ConstantB = 4;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void Add(std::vector<std::uint32_t>& words, spv::Op op, std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1u) << spv::WordCountShift) | op);
    words.insert(words.end(), operands);
}

struct Shape {
    std::uint32_t extractIndex = 1;
    std::uint32_t caseLiteral = 1;
    bool constantOperand = false;
    std::uint32_t constant = ConstantA;
    bool swapParameters = false;
    bool decorate = true;
    bool decorateSum = true;
    std::uint32_t secondParameterType = U32;
    bool extractIndexIsLabel = false;
};

SpirvFunctionBody Body(std::uint32_t base, const Shape& shape) {
    const auto id = [&](std::uint32_t local) { return base + local; };
    SpirvFunctionBody body;
    body.resultType = U32;
    body.parameters = {id(0), id(1)};
    body.parameterTypes = {U32, shape.secondParameterType};
    auto& words = body.words;
    Add(words, spv::OpLabel, {id(2)});
    const auto first = shape.swapParameters ? id(1) : id(0);
    const auto second = shape.constantOperand ? shape.constant : shape.swapParameters ? id(0) : id(1);
    Add(words, spv::OpIAdd, {U32, id(3), first, second});
    Add(words, spv::OpCompositeConstruct, {U32x2, id(4), id(3), id(3)});
    Add(words, spv::OpCompositeExtract, {U32, id(5), id(4), shape.extractIndexIsLabel ? id(2) : shape.extractIndex});
    Add(words, spv::OpSelectionMerge, {id(7), spv::SelectionControlMaskNone});
    Add(words, spv::OpSwitch, {id(0), id(7), 0u, id(6), shape.caseLiteral, id(6)});
    Add(words, spv::OpLabel, {id(6)});
    Add(words, spv::OpBranch, {id(7)});
    Add(words, spv::OpLabel, {id(7)});
    Add(words, spv::OpPhi, {U32, id(8), id(5), id(2), id(3), id(6)});
    Add(words, spv::OpReturnValue, {id(8)});
    if (shape.decorate) Add(body.annotations, spv::OpDecorate, {shape.decorateSum ? id(3) : id(4), spv::DecorationNonUniform});
    return body;
}

void Expect(const Shape& left, const Shape& right, bool same, const char* name) {
    Require(SameSpirvFunctionBody(Body(100, left), Body(200, right), U32) == same, std::string(name) + (same ? " compared different" : " compared the same"));
}

}

int main() {
    try {
        Expect({}, {}, true, "two bodies that differ only in their local ids");
        Shape extract;
        extract.extractIndex = 0;
        Expect({}, extract, false, "a different composite extract index");
        Shape switchCase;
        switchCase.caseLiteral = 2;
        Expect({}, switchCase, false, "a different switch case literal");
        Shape constantA;
        constantA.constantOperand = true;
        Shape constantB = constantA;
        constantB.constant = ConstantB;
        Expect(constantA, constantA, true, "two bodies reading the same module constant");
        Expect(constantA, constantB, false, "bodies reading different module constants");
        Expect({}, constantA, false, "a parameter replaced by a module constant");
        Shape swapped;
        swapped.swapParameters = true;
        Expect({}, swapped, false, "swapped parameters");
        Shape undecorated;
        undecorated.decorate = false;
        Expect({}, undecorated, false, "a missing decoration of a local value");
        Shape moved;
        moved.decorateSum = false;
        Expect({}, moved, false, "a decoration moved to another local value");
        Shape parameterType;
        parameterType.secondParameterType = U32x2;
        Expect({}, parameterType, false, "a different parameter type");
        Shape label;
        label.extractIndexIsLabel = true;
        Expect(label, label, false, "literal words that equal local ids");
        const auto body = Body(100, {});
        const auto locals = SpirvFunctionLocalIds(body.parameters, body.words);
        Require(locals.size() == 9u && locals.contains(100u) && locals.contains(108u) && !locals.contains(U32), "local ids are not the parameters and the results of the body");
        std::puts("SPIR-V function match tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
