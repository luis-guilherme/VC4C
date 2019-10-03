/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "TypeConversions.h"

#include "VectorHelper.h"
#include "operators.h"

using namespace vc4c;
using namespace vc4c::intermediate;
using namespace vc4c::operators;

/*
 * Inserts a bit-cast where the destination element-type is larger than the source element-type, combining multiple
 * elements into a single one.
 *
 * This also means, the source vector has more elements (of smaller type-size) than the destination vector
 */
static NODISCARD InstructionWalker insertCombiningBitcast(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    // the number of source elements to combine in a single destination element
    unsigned sizeFactor = dest.type.getScalarBitCount() / src.type.getScalarBitCount();
    // the number of bits to shift per element
    auto shift = src.type.getScalarBitCount();

    /*
     * By shifting and ANDing whole source vector, we save a few instructions for sources with more than 1 element
     *
     * E.g. short4 -> int2 can be written as
     * (short4 & 0xFFFF) << 0 -> int2 (lower half-words in elements 0 and 2)
     * (short4 & 0xFFFF) << 16 -> int2 (upper half-words in element 1 and 3)
     * -> we only need 2 shifts and 2 ANDs instead of 4 (per element)
     */

    Value truncatedSource = assign(it, src.type, "%bit_cast") =
        src & Value(Literal(src.type.getScalarWidthMask()), TYPE_INT32);

    std::vector<Value> shiftedTruncatedVectors;
    shiftedTruncatedVectors.reserve(sizeFactor);
    for(unsigned i = 0; i < sizeFactor; ++i)
    {
        shiftedTruncatedVectors.emplace_back(
            method.addNewLocal(dest.type.toVectorType(src.type.getVectorWidth()), "%bit_cast"));
        const Value& result = shiftedTruncatedVectors.back();
        assign(it, result) = truncatedSource << Value(Literal(shift * i), TYPE_INT8);
    }

    /*
     * The up to 8 destination elements are now distributed across the shiftedTruncatedVectors (stvs) as follows:
     *
     * Size-factor of 2:
     * stv0[0] | stv1[1], stv0[2] | stv1[3], stv0[4] | stv1[5], ...
     *
     * Size-factor of 4;
     * stv0[0] | stv1[1] | stv2[2] | stv3[3], stv0[4] | stv1[5] | stv2[6] | stv3[7], ...
     *
     * To simplify the assembly of the destination, we rotate the vectors, so their element-numbers align
     */

    std::vector<Value> rotatedVectors;
    rotatedVectors.reserve(shiftedTruncatedVectors.size());
    for(unsigned i = 0; i < shiftedTruncatedVectors.size(); ++i)
    {
        if(i == 0)
            // no need to rotate
            rotatedVectors.emplace_back(shiftedTruncatedVectors.front());
        else
        {
            rotatedVectors.emplace_back(
                method.addNewLocal(dest.type.toVectorType(src.type.getVectorWidth()), "%bit_cast"));
            const Value& result = rotatedVectors.back();
            it = insertVectorRotation(
                it, shiftedTruncatedVectors[i], Value(Literal(i), TYPE_INT8), result, Direction::DOWN);
        }
    }

    /*
     * The up to 8 destination elements are now distributed across the rotatedVectors (rvs) as follows:
     *
     * Size-factor of 2:
     * rv0[0] | rv1[0], rv0[2] | rv1[2], rv0[4] | rv1[4], ...
     *
     * Size-factor of 4;
     * rv0[0] | rv1[0] | rv2[0] | rv3[0], rv0[4] | rv1[4] | rv2[4] | rv3[4], ...
     *
     * In the next step, we OR the separate vectors to a single one
     */
    Value combinedVector = INT_ZERO;
    for(const Value& rv : rotatedVectors)
    {
        Value newCombinedVector = assign(it, dest.type.toVectorType(src.type.getVectorWidth()), "%bit_cast") =
            combinedVector | rv;
        combinedVector = newCombinedVector;
    }

    /*
     * Now, we have the destination elements as follows:
     *
     * Size-factor of 2:
     * cv[0], cv[2], cv[4], cv[6], ...
     *
     * Size-factor of 4;
     * cv[0], cv[4], cv[8], cv[12], ...
     *
     * Finally, we rotate the single elements to fit their position in the destination
     */

    Value destination = method.addNewLocal(dest.type, "%bit_cast");
    // initialize destination with zero so register-allocation finds unconditional assignment
    it.emplace(new MoveOperation(destination, INT_ZERO));
    it.nextInBlock();

    for(unsigned i = 0; i < dest.type.getVectorWidth(); ++i)
    {
        unsigned sourceIndex = i * sizeFactor;

        const Value tmp = method.addNewLocal(dest.type, "%bit_cast");
        // the vector-rotation to element 0 and then to the destination element should be combined by optimization-step
        // #combineVectorRotations
        it = insertVectorExtraction(it, method, combinedVector, Value(Literal(sourceIndex), TYPE_INT8), tmp);
        it = insertVectorInsertion(it, method, destination, Value(Literal(i), TYPE_INT8), tmp);
    }

    it.emplace(new MoveOperation(dest, destination));
    return it;
}

/*
 * Inserts a bit-cast where the destination element-type is smaller than the source element-type, splitting a single
 * element into several ones.
 *
 * This also means, the source vector has less elements (of larger type-size) than the destination vector
 */
static NODISCARD InstructionWalker insertSplittingBitcast(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    // the number of destination elements to extract from a single source element
    unsigned sizeFactor = src.type.getScalarBitCount() / dest.type.getScalarBitCount();
    // the number of bits to shift per element
    auto shift = dest.type.getScalarBitCount();

    /*
     * By shifting and ANDing whole source vector, we save a few instructions for sources with more than 1 element
     *
     * E.g. int2 -> short4 can be written as
     * (int2 >> 0) & 0xFFFF -> short4 (lower half-words)
     * (int2 >> 16) & 0xFFFF -> short4 (upper half-words)
     * -> we only need 2 shifts and 2 ANDs instead of 4 (per element)
     */
    std::vector<Value> shiftedTruncatedVectors;
    shiftedTruncatedVectors.reserve(sizeFactor);
    for(unsigned i = 0; i < sizeFactor; ++i)
    {
        shiftedTruncatedVectors.emplace_back(method.addNewLocal(dest.type, "%bit_cast"));
        const Value& result = shiftedTruncatedVectors.back();
        Value tmp = assign(it, dest.type, "%bit_cast") = as_unsigned{src} >> Value(Literal(shift * i), TYPE_INT8);
        assign(it, result) = tmp & Value(Literal(dest.type.getScalarWidthMask()), TYPE_INT32);
    }

    /*
     * The up to 16 destination elements are now distributed across the shiftedTruncatedVectors (stvs) as follows:
     *
     * Size-factor of 2:
     * stv0[0], stv1[0], stv0[1], stv1[1], stv0[2], ...
     *
     * Size-factor of 4;
     * stv0[0], stv1[0], stv2[0], stv3[0], stv0[1], ...
     *
     * So we need to assemble the destination vector from these vectors
     */

    const Value destination = method.addNewLocal(dest.type, "%bit_cast");
    // initialize destination with zero so register-allocation finds unconditional assignment
    it.emplace(new MoveOperation(destination, INT_ZERO));
    it.nextInBlock();

    for(unsigned i = 0; i < dest.type.getVectorWidth(); ++i)
    {
        const Value& stv = shiftedTruncatedVectors.at(i % shiftedTruncatedVectors.size());
        unsigned sourceElement = static_cast<unsigned>(i / shiftedTruncatedVectors.size());

        // need to fix-up the type to single element for vector insertion to handle it as such
        const Value tmp = method.addNewLocal(dest.type.getElementType(), "%bit_cast");
        // the vector-rotation to element 0 and then to the destination element should be combined by optimization-step
        // #combineVectorRotations
        it = insertVectorExtraction(it, method, stv, Value(Literal(sourceElement), TYPE_INT8), tmp);
        it = insertVectorInsertion(it, method, destination, Value(Literal(i), TYPE_INT8), tmp);
    }

    it.emplace(new MoveOperation(dest, destination));
    return it;
}

InstructionWalker intermediate::insertBitcast(
    InstructionWalker it, Method& method, const Value& src, const Value& dest, InstructionDecorations deco)
{
    if(src.isUndefined())
        it.emplace(new intermediate::MoveOperation(dest, UNDEFINED_VALUE));
    else if(src.isZeroInitializer())
        it.emplace(new intermediate::MoveOperation(dest, INT_ZERO));
    else if(src.type.getVectorWidth() > dest.type.getVectorWidth())
        it = insertCombiningBitcast(it, method, src, dest);
    else if(src.type.getVectorWidth() < dest.type.getVectorWidth())
        it = insertSplittingBitcast(it, method, src, dest);
    else
        // bit-casts with types of same vector-size (and therefore same element-size) are simple moves
        it.emplace((new intermediate::MoveOperation(dest, src))->addDecorations(deco));

    // last step: map destination to source (if bit-cast of pointers)
    if(dest.checkLocal() && src.checkLocal() && dest.type.getPointerType() && src.type.getPointerType())
        // this helps recognizing lifetime-starts of bit-cast stack-allocations
        const_cast<std::pair<Local*, int>&>(dest.local()->reference) = std::make_pair(src.local(), 0);
    it->addDecorations(deco);
    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertZeroExtension(InstructionWalker it, Method& method, const Value& src,
    const Value& dest, bool allowLiteral, const ConditionCode conditional, const SetFlag setFlags)
{
    if(src.type.getScalarBitCount() == 32 && dest.type.getScalarBitCount() <= 32)
    {
        //"extend" to smaller type
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
        switch(dest.type.getScalarBitCount())
        {
        case 8:
            it->setPackMode(PACK_INT_TO_CHAR_TRUNCATE);
            break;
        case 16:
            it->setPackMode(PACK_INT_TO_SHORT_TRUNCATE);
            break;
        case 32:
            // no pack mode
            break;
        default:
            throw CompilationError(
                CompilationStep::GENERAL, "Invalid type-width for zero-extension", dest.type.to_string());
        }
    }
    else if(dest.type.getScalarBitCount() >= 32 && src.type.getScalarBitCount() >= 32)
    {
        // do nothing, is just a move, since we truncate the 64-bit integers anyway
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
    }
    else if(src.checkRegister() &&
        (has_flag(src.reg().file, RegisterFile::PHYSICAL_A) || has_flag(src.reg().file, RegisterFile::ACCUMULATOR)) &&
        src.type.getScalarBitCount() == 8)
    {
        // if we zero-extend from register-file A, use unpack-modes
        // this is applied e.g. for unpacking parameters in code-generation, since the source is UNIFORM
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
        it->setUnpackMode(UNPACK_CHAR_TO_INT_ZEXT);
    }
    else if(allowLiteral)
    {
        it.emplace(new Operation(
            OP_AND, dest, src, Value(Literal(src.type.getScalarWidthMask()), TYPE_INT32), conditional, setFlags));
    }
    else
    {
        const Value tmp = method.addNewLocal(TYPE_INT32, "%zext");
        it.emplace(new LoadImmediate(tmp, Literal(src.type.getScalarWidthMask())));
        it.nextInBlock();
        it.emplace(new Operation(OP_AND, dest, src, tmp, conditional, setFlags));
    }

    it->addDecorations(InstructionDecorations::UNSIGNED_RESULT);
    auto writer = src.getSingleWriter();
    if(writer)
        it->addDecorations(intermediate::forwardDecorations(writer->decoration));
    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertSignExtension(InstructionWalker it, Method& method, const Value& src,
    const Value& dest, bool allowLiteral, const ConditionCode conditional, const SetFlag setFlags)
{
    if(dest.type.getScalarBitCount() >= 32 && src.type.getScalarBitCount() >= 32)
    {
        // do nothing, is just a move, since we truncate the 64-bit integers anyway
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
    }
    else if(src.checkRegister() &&
        (has_flag(src.reg().file, RegisterFile::PHYSICAL_A) || has_flag(src.reg().file, RegisterFile::ACCUMULATOR)) &&
        src.type.getScalarBitCount() == 16)
    {
        // if we sign-extend from register-file A, use unpack-modes
        // this is applied e.g. for unpacking parameters in code-generation, since the source is UNIFORM
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
        it->setUnpackMode(UNPACK_SHORT_TO_INT_SEXT);
    }
    else
    {
        // out = asr(shl(in, bit_diff) bit_diff)
        // where bit_diff is the difference to full 32-bit
        Value widthDiff(Literal(static_cast<int32_t>(32 - src.type.getScalarBitCount())), TYPE_INT8);

        if(!allowLiteral)
        {
            Value tmp = method.addNewLocal(TYPE_INT8, "%sext");
            it.emplace(new LoadImmediate(tmp, widthDiff.literal()));
            it.nextInBlock();
            widthDiff = tmp;
        }

        Value tmp = assign(it, TYPE_INT32, "%sext") = (src << widthDiff, conditional);
        it.emplace(new Operation(OP_ASR, dest, tmp, widthDiff, conditional, setFlags));
        // we shifted the exact same number of bits to the left before, so all N trailing bits are zero
        it->addDecorations(InstructionDecorations::EXACT_OPERATION);
    }

    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertSaturation(
    InstructionWalker it, Method& method, const Value& src, const Value& dest, bool isSigned)
{
    // saturation = clamping to min/max of type
    //-> dest = max(min(src, destType.max), destType.min)
    //-> or via pack-modes

    if(!dest.type.isSimpleType() || dest.type.isFloatingType())
        throw CompilationError(CompilationStep::GENERAL, "Invalid target type for saturation", dest.type.to_string());

    if(auto lit = src.getLiteralValue())
    {
        switch(dest.type.getScalarBitCount())
        {
        case 8:
            return it.emplace((new MoveOperation(dest,
                                   Value(isSigned ? Literal(saturate<int8_t>(lit->signedInt())) :
                                                    Literal(saturate<uint8_t>(lit->unsignedInt())),
                                       dest.type)))
                                  ->addDecorations(isSigned ? InstructionDecorations::NONE :
                                                              InstructionDecorations::UNSIGNED_RESULT));
        case 16:
            return it.emplace((new MoveOperation(dest,
                                   Value(isSigned ? Literal(saturate<int16_t>(lit->signedInt())) :
                                                    Literal(saturate<uint16_t>(lit->unsignedInt())),
                                       dest.type)))
                                  ->addDecorations(isSigned ? InstructionDecorations::NONE :
                                                              InstructionDecorations::UNSIGNED_RESULT));
        case 32:
            return it.emplace((new MoveOperation(dest,
                                   Value(isSigned ? Literal(saturate<int32_t>(lit->signedInt())) :
                                                    Literal(saturate<uint32_t>(lit->unsignedInt())),
                                       dest.type)))
                                  ->addDecorations(isSigned ? InstructionDecorations::NONE :
                                                              InstructionDecorations::UNSIGNED_RESULT));
        default:
            throw CompilationError(
                CompilationStep::GENERAL, "Invalid target type for saturation", dest.type.to_string());
        }
    }
    else // saturation can be easily done via pack-modes
    {
        if(dest.type.getScalarBitCount() == 8 && !isSigned)
            return it.emplace((new MoveOperation(dest, src))
                                  ->setPackMode(PACK_INT_TO_UNSIGNED_CHAR_SATURATE)
                                  ->addDecorations(InstructionDecorations::UNSIGNED_RESULT));
        else if(dest.type.getScalarBitCount() == 16 && isSigned)
            return it.emplace((new MoveOperation(dest, src))->setPackMode(PACK_INT_TO_SIGNED_SHORT_SATURATE));
        // TODO 32-bit saturation cannot be applied as an extra instruction, has to be done in the instruction
        // calculating the overflowing value
        // TODO need to saturate manually
        throw CompilationError(
            CompilationStep::GENERAL, "Saturation to this type is not yet supported", dest.type.to_string());
    }
}

InstructionWalker intermediate::insertTruncate(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    if(dest.type.getScalarBitCount() >= src.type.getScalarBitCount())
        //"truncate" to larger type, simply move
        assign(it, dest) = src;
    else
        assign(it, dest) = src & Value(Literal(dest.type.getScalarWidthMask()), TYPE_INT32);

    return it;
}

InstructionWalker intermediate::insertFloatingPointConversion(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    if(src.type.getScalarBitCount() == dest.type.getScalarBitCount())
        it.emplace(new MoveOperation(dest, src));
    else if(src.type.getScalarBitCount() == 16 && dest.type.getScalarBitCount() == 32)
        it.emplace((new Operation(OP_FMUL, dest, src, OpCode::getRightIdentity(OP_FMUL).value()))
                       ->setUnpackMode(UNPACK_HALF_TO_FLOAT));
    else if(src.type.getScalarBitCount() == 32 && dest.type.getScalarBitCount() == 16)
        it.emplace((new intermediate::Operation(OP_FMUL, dest, src, OpCode::getRightIdentity(OP_FMUL).value()))
                       ->setPackMode(PACK_FLOAT_TO_HALF_TRUNCATE));
    else
        throw CompilationError(CompilationStep::GENERAL, "Unsupported floating-point conversion");
    return it.nextInBlock();
}
