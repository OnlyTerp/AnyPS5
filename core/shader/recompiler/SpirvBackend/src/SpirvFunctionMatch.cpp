#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#undef SPV_ENABLE_UTILITY_CODE
#include "SpirvBackend/SpirvFunctionMatch.hpp"
#include <unordered_map>

namespace ShaderRecompiler {
namespace {

struct ResultLayout {
    std::size_t type = 0;
    std::size_t result = 0;
};

spv::Op opcodeOf(std::uint32_t word) {
    return static_cast<spv::Op>(word & spv::OpCodeMask);
}

ResultLayout resultLayout(spv::Op op) {
    bool hasResult = false;
    bool hasType = false;
    spv::HasResultAndType(op, &hasResult, &hasType);
    return {hasType ? 1u : 0u, hasResult ? (hasType ? 2u : 1u) : 0u};
}

bool allIdOperands(spv::Op op) {
    switch (op) {
    case spv::OpUndef:
    case spv::OpCopyObject:
    case spv::OpCopyLogical:
    case spv::OpConvertFToU:
    case spv::OpConvertFToS:
    case spv::OpConvertSToF:
    case spv::OpConvertUToF:
    case spv::OpUConvert:
    case spv::OpSConvert:
    case spv::OpFConvert:
    case spv::OpQuantizeToF16:
    case spv::OpConvertPtrToU:
    case spv::OpConvertUToPtr:
    case spv::OpBitcast:
    case spv::OpSNegate:
    case spv::OpFNegate:
    case spv::OpIAdd:
    case spv::OpFAdd:
    case spv::OpISub:
    case spv::OpFSub:
    case spv::OpIMul:
    case spv::OpFMul:
    case spv::OpUDiv:
    case spv::OpSDiv:
    case spv::OpFDiv:
    case spv::OpUMod:
    case spv::OpSRem:
    case spv::OpSMod:
    case spv::OpFRem:
    case spv::OpFMod:
    case spv::OpVectorTimesScalar:
    case spv::OpDot:
    case spv::OpIAddCarry:
    case spv::OpISubBorrow:
    case spv::OpUMulExtended:
    case spv::OpSMulExtended:
    case spv::OpAny:
    case spv::OpAll:
    case spv::OpIsNan:
    case spv::OpIsInf:
    case spv::OpIsFinite:
    case spv::OpIsNormal:
    case spv::OpSignBitSet:
    case spv::OpOrdered:
    case spv::OpUnordered:
    case spv::OpLogicalEqual:
    case spv::OpLogicalNotEqual:
    case spv::OpLogicalOr:
    case spv::OpLogicalAnd:
    case spv::OpLogicalNot:
    case spv::OpSelect:
    case spv::OpIEqual:
    case spv::OpINotEqual:
    case spv::OpUGreaterThan:
    case spv::OpSGreaterThan:
    case spv::OpUGreaterThanEqual:
    case spv::OpSGreaterThanEqual:
    case spv::OpULessThan:
    case spv::OpSLessThan:
    case spv::OpULessThanEqual:
    case spv::OpSLessThanEqual:
    case spv::OpFOrdEqual:
    case spv::OpFUnordEqual:
    case spv::OpFOrdNotEqual:
    case spv::OpFUnordNotEqual:
    case spv::OpFOrdLessThan:
    case spv::OpFUnordLessThan:
    case spv::OpFOrdGreaterThan:
    case spv::OpFUnordGreaterThan:
    case spv::OpFOrdLessThanEqual:
    case spv::OpFUnordLessThanEqual:
    case spv::OpFOrdGreaterThanEqual:
    case spv::OpFUnordGreaterThanEqual:
    case spv::OpShiftRightLogical:
    case spv::OpShiftRightArithmetic:
    case spv::OpShiftLeftLogical:
    case spv::OpBitwiseOr:
    case spv::OpBitwiseXor:
    case spv::OpBitwiseAnd:
    case spv::OpNot:
    case spv::OpBitFieldInsert:
    case spv::OpBitFieldSExtract:
    case spv::OpBitFieldUExtract:
    case spv::OpBitReverse:
    case spv::OpBitCount:
    case spv::OpDPdx:
    case spv::OpDPdy:
    case spv::OpFwidth:
    case spv::OpDPdxFine:
    case spv::OpDPdyFine:
    case spv::OpFwidthFine:
    case spv::OpDPdxCoarse:
    case spv::OpDPdyCoarse:
    case spv::OpFwidthCoarse:
    case spv::OpAccessChain:
    case spv::OpInBoundsAccessChain:
    case spv::OpPtrAccessChain:
    case spv::OpInBoundsPtrAccessChain:
    case spv::OpPtrEqual:
    case spv::OpPtrNotEqual:
    case spv::OpPtrDiff:
    case spv::OpPhi:
    case spv::OpCompositeConstruct:
    case spv::OpVectorExtractDynamic:
    case spv::OpVectorInsertDynamic:
    case spv::OpSampledImage:
    case spv::OpImage:
    case spv::OpImageQuerySizeLod:
    case spv::OpImageQuerySize:
    case spv::OpImageQueryLod:
    case spv::OpImageQueryLevels:
    case spv::OpImageQuerySamples:
    case spv::OpImageTexelPointer:
    case spv::OpFunctionCall:
    case spv::OpFunctionParameter:
    case spv::OpLabel:
    case spv::OpBranch:
    case spv::OpReturn:
    case spv::OpReturnValue:
    case spv::OpUnreachable:
    case spv::OpKill:
    case spv::OpTerminateInvocation:
    case spv::OpDemoteToHelperInvocation:
    case spv::OpIsHelperInvocationEXT:
    case spv::OpAtomicLoad:
    case spv::OpAtomicStore:
    case spv::OpAtomicExchange:
    case spv::OpAtomicCompareExchange:
    case spv::OpAtomicIIncrement:
    case spv::OpAtomicIDecrement:
    case spv::OpAtomicIAdd:
    case spv::OpAtomicISub:
    case spv::OpAtomicSMin:
    case spv::OpAtomicUMin:
    case spv::OpAtomicSMax:
    case spv::OpAtomicUMax:
    case spv::OpAtomicAnd:
    case spv::OpAtomicOr:
    case spv::OpAtomicXor:
    case spv::OpAtomicFAddEXT:
    case spv::OpAtomicFMinEXT:
    case spv::OpAtomicFMaxEXT:
    case spv::OpControlBarrier:
    case spv::OpMemoryBarrier:
    case spv::OpReadClockKHR:
    case spv::OpGroupNonUniformElect:
    case spv::OpGroupNonUniformAll:
    case spv::OpGroupNonUniformAny:
    case spv::OpGroupNonUniformAllEqual:
    case spv::OpGroupNonUniformBroadcast:
    case spv::OpGroupNonUniformBroadcastFirst:
    case spv::OpGroupNonUniformBallot:
    case spv::OpGroupNonUniformInverseBallot:
    case spv::OpGroupNonUniformBallotBitExtract:
    case spv::OpGroupNonUniformBallotFindLSB:
    case spv::OpGroupNonUniformBallotFindMSB:
    case spv::OpGroupNonUniformShuffle:
    case spv::OpGroupNonUniformShuffleXor:
    case spv::OpGroupNonUniformShuffleUp:
    case spv::OpGroupNonUniformShuffleDown:
    case spv::OpGroupNonUniformQuadBroadcast:
    case spv::OpGroupNonUniformQuadSwap:
        return true;
    default:
        return false;
    }
}

bool idOperand(std::span<const std::uint32_t> instruction, std::size_t index, bool switchPairs) {
    const auto op = opcodeOf(instruction[0]);
    const auto layout = resultLayout(op);
    if (index == layout.type || index == layout.result) return true;
    if (allIdOperands(op)) return true;
    switch (op) {
    case spv::OpCompositeExtract:
        return index == 3u;
    case spv::OpCompositeInsert:
    case spv::OpVectorShuffle:
        return index == 3u || index == 4u;
    case spv::OpExtInst:
        return index == 3u || index >= 5u;
    case spv::OpLoad:
        return index == 3u;
    case spv::OpStore:
        return index == 1u || index == 2u;
    case spv::OpSelectionMerge:
        return index == 1u;
    case spv::OpLoopMerge:
        return index == 1u || index == 2u;
    case spv::OpBranchConditional:
        return index <= 3u;
    case spv::OpSwitch:
        return index == 1u || index == 2u || (switchPairs && index >= 4u && index % 2u == 0u);
    case spv::OpVariable:
        return index == 4u;
    case spv::OpImageSampleImplicitLod:
    case spv::OpImageSampleExplicitLod:
    case spv::OpImageFetch:
    case spv::OpImageRead:
        return index == 3u || index == 4u || index >= 6u;
    case spv::OpImageSampleDrefImplicitLod:
    case spv::OpImageSampleDrefExplicitLod:
    case spv::OpImageGather:
    case spv::OpImageDrefGather:
        return (index >= 3u && index <= 5u) || index >= 7u;
    case spv::OpImageWrite:
        return index <= 3u || index >= 5u;
    case spv::OpGroupNonUniformBallotBitCount:
    case spv::OpGroupNonUniformIAdd:
    case spv::OpGroupNonUniformFAdd:
    case spv::OpGroupNonUniformIMul:
    case spv::OpGroupNonUniformFMul:
    case spv::OpGroupNonUniformSMin:
    case spv::OpGroupNonUniformUMin:
    case spv::OpGroupNonUniformFMin:
    case spv::OpGroupNonUniformSMax:
    case spv::OpGroupNonUniformUMax:
    case spv::OpGroupNonUniformFMax:
    case spv::OpGroupNonUniformBitwiseAnd:
    case spv::OpGroupNonUniformBitwiseOr:
    case spv::OpGroupNonUniformBitwiseXor:
    case spv::OpGroupNonUniformLogicalAnd:
    case spv::OpGroupNonUniformLogicalOr:
    case spv::OpGroupNonUniformLogicalXor:
        return index == 3u || index >= 5u;
    default:
        return false;
    }
}

template<typename TVisitor>
bool forEachInstruction(std::span<const std::uint32_t> words, const TVisitor& visit) {
    for (std::size_t cursor = 0; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        if (count == 0u || count > words.size() - cursor) return false;
        visit(words.subspan(cursor, count));
        cursor += count;
    }
    return true;
}

struct Locals {
    std::unordered_map<std::uint32_t, std::uint32_t> index;
    std::unordered_map<std::uint32_t, std::uint32_t> type;
};

Locals indexLocals(const SpirvFunctionBody& body) {
    Locals locals;
    for (std::size_t parameter = 0; parameter < body.parameters.size(); ++parameter) {
        locals.index.emplace(body.parameters[parameter], static_cast<std::uint32_t>(locals.index.size()));
        locals.type.emplace(body.parameters[parameter], body.parameterTypes[parameter]);
    }
    static_cast<void>(forEachInstruction(body.words, [&](std::span<const std::uint32_t> instruction) {
        const auto layout = resultLayout(opcodeOf(instruction[0]));
        if (layout.result == 0u || layout.result >= instruction.size()) return;
        locals.index.emplace(instruction[layout.result], static_cast<std::uint32_t>(locals.index.size()));
        if (layout.type != 0u) locals.type.emplace(instruction[layout.result], instruction[layout.type]);
    }));
    return locals;
}

bool switchPairsOf(std::span<const std::uint32_t> instruction, const Locals& locals, std::uint32_t switchSelectorType) {
    if (opcodeOf(instruction[0]) != spv::OpSwitch || instruction.size() < 3u) return false;
    const auto found = locals.type.find(instruction[1]);
    return found != locals.type.end() && found->second == switchSelectorType && (instruction.size() - 3u) % 2u == 0u;
}

bool sameOperand(std::uint32_t first, std::uint32_t second, const Locals& firstLocals, const Locals& secondLocals) {
    const auto firstLocal = firstLocals.index.find(first);
    const auto secondLocal = secondLocals.index.find(second);
    if (firstLocal == firstLocals.index.end() && secondLocal == secondLocals.index.end()) return first == second;
    return firstLocal != firstLocals.index.end() && secondLocal != secondLocals.index.end() && firstLocal->second == secondLocal->second;
}

template<typename TIdOperand>
bool sameWords(std::span<const std::uint32_t> first, std::span<const std::uint32_t> second, const Locals& firstLocals, const Locals& secondLocals, const TIdOperand& isId) {
    if (first.size() != second.size()) return false;
    for (std::size_t cursor = 0; cursor < first.size();) {
        const auto count = first[cursor] >> spv::WordCountShift;
        if (count == 0u || count > first.size() - cursor || first[cursor] != second[cursor]) return false;
        const auto left = first.subspan(cursor, count);
        const auto right = second.subspan(cursor, count);
        const bool leftPairs = switchPairsOf(left, firstLocals, isId.selectorType);
        if (leftPairs != switchPairsOf(right, secondLocals, isId.selectorType)) return false;
        for (std::size_t index = 1; index < count; ++index) {
            if (isId(left, index, leftPairs)) {
                if (!sameOperand(left[index], right[index], firstLocals, secondLocals)) return false;
            } else if (left[index] != right[index]) {
                return false;
            }
        }
        cursor += count;
    }
    return true;
}

struct BodyOperands {
    std::uint32_t selectorType;
    bool operator()(std::span<const std::uint32_t> instruction, std::size_t index, bool switchPairs) const {
        return idOperand(instruction, index, switchPairs);
    }
};

struct AnnotationOperands {
    std::uint32_t selectorType;
    bool operator()(std::span<const std::uint32_t>, std::size_t index, bool) const {
        return index == 1u;
    }
};

}

std::unordered_set<std::uint32_t> SpirvFunctionLocalIds(std::span<const std::uint32_t> parameters, std::span<const std::uint32_t> words) {
    std::unordered_set<std::uint32_t> locals(parameters.begin(), parameters.end());
    static_cast<void>(forEachInstruction(words, [&](std::span<const std::uint32_t> instruction) {
        const auto layout = resultLayout(opcodeOf(instruction[0]));
        if (layout.result != 0u && layout.result < instruction.size()) locals.insert(instruction[layout.result]);
    }));
    return locals;
}

bool SameSpirvFunctionBody(const SpirvFunctionBody& first, const SpirvFunctionBody& second, std::uint32_t switchSelectorType) {
    if (first.resultType != second.resultType || first.parameters.size() != second.parameters.size() || first.parameterTypes != second.parameterTypes || first.parameters.size() != first.parameterTypes.size()) return false;
    if (first.words.size() != second.words.size() || first.annotations.size() != second.annotations.size()) return false;
    const auto firstLocals = indexLocals(first);
    const auto secondLocals = indexLocals(second);
    if (firstLocals.index.size() != secondLocals.index.size()) return false;
    return sameWords(first.words, second.words, firstLocals, secondLocals, BodyOperands{switchSelectorType}) &&
        sameWords(first.annotations, second.annotations, firstLocals, secondLocals, AnnotationOperands{switchSelectorType});
}

}
