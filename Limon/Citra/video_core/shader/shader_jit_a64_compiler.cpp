//
//  shader_jit_a64_compiler.cpp
//  Limon
//
//  Created by Jarrod Norwell on 10/21/23.
//

#include "common/arch.h"
#if CITRA_ARCH(arm64)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nihstro/shader_bytecode.h>
#include "common/aarch64/cpu_detect.h"
#include "common/aarch64/oaknut_abi.h"
#include "common/aarch64/oaknut_util.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/vector_math.h"
#include "video_core/pica_state.h"
#include "video_core/pica_types.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_jit_a64_compiler.h"

using namespace Common::A64;
using namespace oaknut;
using namespace oaknut::util;

using nihstro::DestRegister;
using nihstro::RegisterType;

namespace Pica::Shader {

typedef void (JitShader::*JitFunction)(Instruction instr);

const JitFunction instr_table[64] = {
    &JitShader::Compile_ADD,    // add
    &JitShader::Compile_DP3,    // dp3
    &JitShader::Compile_DP4,    // dp4
    &JitShader::Compile_DPH,    // dph
    nullptr,                    // unknown
    &JitShader::Compile_EX2,    // ex2
    &JitShader::Compile_LG2,    // lg2
    nullptr,                    // unknown
    &JitShader::Compile_MUL,    // mul
    &JitShader::Compile_SGE,    // sge
    &JitShader::Compile_SLT,    // slt
    &JitShader::Compile_FLR,    // flr
    &JitShader::Compile_MAX,    // max
    &JitShader::Compile_MIN,    // min
    &JitShader::Compile_RCP,    // rcp
    &JitShader::Compile_RSQ,    // rsq
    nullptr,                    // unknown
    nullptr,                    // unknown
    &JitShader::Compile_MOVA,   // mova
    &JitShader::Compile_MOV,    // mov
    nullptr,                    // unknown
    nullptr,                    // unknown
    nullptr,                    // unknown
    nullptr,                    // unknown
    &JitShader::Compile_DPH,    // dphi
    nullptr,                    // unknown
    &JitShader::Compile_SGE,    // sgei
    &JitShader::Compile_SLT,    // slti
    nullptr,                    // unknown
    nullptr,                    // unknown
    nullptr,                    // unknown
    nullptr,                    // unknown
    nullptr,                    // unknown
    &JitShader::Compile_NOP,    // nop
    &JitShader::Compile_END,    // end
    &JitShader::Compile_BREAKC, // breakc
    &JitShader::Compile_CALL,   // call
    &JitShader::Compile_CALLC,  // callc
    &JitShader::Compile_CALLU,  // callu
    &JitShader::Compile_IF,     // ifu
    &JitShader::Compile_IF,     // ifc
    &JitShader::Compile_LOOP,   // loop
    &JitShader::Compile_EMIT,   // emit
    &JitShader::Compile_SETE,   // sete
    &JitShader::Compile_JMP,    // jmpc
    &JitShader::Compile_JMP,    // jmpu
    &JitShader::Compile_CMP,    // cmp
    &JitShader::Compile_CMP,    // cmp
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // madi
    &JitShader::Compile_MAD,    // mad
    &JitShader::Compile_MAD,    // mad
    &JitShader::Compile_MAD,    // mad
    &JitShader::Compile_MAD,    // mad
    &JitShader::Compile_MAD,    // mad
    &JitShader::Compile_MAD,    // mad
    &JitShader::Compile_MAD,    // mad
    &JitShader::Compile_MAD,    // mad
};

// The following is used to alias some commonly used registers. Generally, RAX-RDX and QReg0-QReg3
// can be used as scratch registers within a compiler function. The other registers have designated
// purposes, as documented below:

/// Pointer to the uniform memory
constexpr XReg UNIFORMS = X9;
/// The two 32-bit VS address offset registers set by the MOVA instruction
constexpr XReg ADDROFFS_REG_0 = X10;
constexpr XReg ADDROFFS_REG_1 = X11;
/// VS loop count register (Multiplied by 16)
constexpr WReg LOOPCOUNT_REG = W12;
/// Current VS loop iteration number (we could probably use LOOPCOUNT_REG, but this quicker)
constexpr WReg LOOPCOUNT = W6;
/// Number to increment LOOPCOUNT_REG by on each loop iteration (Multiplied by 16)
constexpr WReg LOOPINC = W7;
/// Result of the previous CMP instruction for the X-component comparison
constexpr XReg COND0 = X13;
/// Result of the previous CMP instruction for the Y-component comparison
constexpr XReg COND1 = X14;
/// Pointer to the UnitState instance for the current VS unit
constexpr XReg STATE = X15;
/// Scratch registers
constexpr XReg XSCRATCH0 = X4;
constexpr XReg XSCRATCH1 = X5;
constexpr QReg VSCRATCH0 = Q0;
constexpr QReg VSCRATCH1 = Q4;
constexpr QReg VSCRATCH2 = Q15;
/// Loaded with the first swizzled source register, otherwise can be used as a scratch register
constexpr QReg SRC1 = Q1;
/// Loaded with the second swizzled source register, otherwise can be used as a scratch register
constexpr QReg SRC2 = Q2;
/// Loaded with the third swizzled source register, otherwise can be used as a scratch register
constexpr QReg SRC3 = Q3;
/// Constant vector of [1.0f, 1.0f, 1.0f, 1.0f], used to efficiently set a vector to one
constexpr QReg ONE = Q14;

// State registers that must not be modified by external functions calls
// Scratch registers, e.g., SRC1 and VSCRATCH0, have to be saved on the side if needed
static const std::bitset<64> persistent_regs = BuildRegSet({
    // Pointers to register blocks
    UNIFORMS,
    STATE,
    // Cached registers
    ADDROFFS_REG_0,
    ADDROFFS_REG_1,
    LOOPCOUNT_REG,
    COND0,
    COND1,
    // Constants
    ONE,
    // Loop variables
    LOOPCOUNT,
    LOOPINC,
});

/// Raw constant for the source register selector that indicates no swizzling is performed
static const u8 NO_SRC_REG_SWIZZLE = 0x1b;
/// Raw constant for the destination register enable mask that indicates all components are enabled
static const u8 NO_DEST_REG_MASK = 0xf;

static void LogCritical(const char* msg) {
    LOG_CRITICAL(HW_GPU, "{}", msg);
}

void JitShader::Compile_Assert(bool condition, const char* msg) {}

/**
 * Loads and swizzles a source register into the specified QReg register.
 * @param instr VS instruction, used for determining how to load the source register
 * @param src_num Number indicating which source register to load (1 = src1, 2 = src2, 3 = src3)
 * @param src_reg SourceRegister object corresponding to the source register to load
 * @param dest Destination QReg register to store the loaded, swizzled source register
 */
void JitShader::Compile_SwizzleSrc(Instruction instr, unsigned src_num, SourceRegister src_reg,
                                   QReg dest) {
    XReg src_ptr = XZR;
    std::size_t src_offset;
    switch (src_reg.GetRegisterType()) {
    case RegisterType::FloatUniform:
        src_ptr = UNIFORMS;
        src_offset = Uniforms::GetFloatUniformOffset(src_reg.GetIndex());
        break;
    case RegisterType::Input:
        src_ptr = STATE;
        src_offset = UnitState::InputOffset(src_reg.GetIndex());
        break;
    case RegisterType::Temporary:
        src_ptr = STATE;
        src_offset = UnitState::TemporaryOffset(src_reg.GetIndex());
        break;
    default:
        UNREACHABLE_MSG("Encountered unknown source register type: {}", src_reg.GetRegisterType());
        break;
    }

    s32 src_offset_disp = (s32)src_offset;
    ASSERT_MSG(src_offset == static_cast<std::size_t>(src_offset_disp),
               "Source register offset too large for int type");

    u32 operand_desc_id;

    const bool is_inverted =
        (0 != (instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed));

    u32 address_register_index;
    u32 offset_src;

    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD ||
        instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI) {
        operand_desc_id = instr.mad.operand_desc_id;
        offset_src = is_inverted ? 3 : 2;
        address_register_index = instr.mad.address_register_index;
    } else {
        operand_desc_id = instr.common.operand_desc_id;
        offset_src = is_inverted ? 2 : 1;
        address_register_index = instr.common.address_register_index;
    }

    if (src_reg.GetRegisterType() == RegisterType::FloatUniform && src_num == offset_src &&
        address_register_index != 0) {
        XReg address_reg = XZR;
        switch (address_register_index) {
        case 1:
            address_reg = ADDROFFS_REG_0;
            break;
        case 2:
            address_reg = ADDROFFS_REG_1;
            break;
        case 3:
            address_reg = LOOPCOUNT_REG.toX();
            break;
        default:
            UNREACHABLE();
            break;
        }

        // s32 offset = address_reg >= -128 && address_reg <= 127 ? address_reg : 0;
        // u32 index = (src_reg.GetIndex() + offset) & 0x7f;

        // First we add 128 to address_reg so the first comparison is turned to
        // address_reg >= 0 && address_reg < 256

        // offset = ((address_reg + 128) < 256) ? address_reg : 0
        ADD(XSCRATCH1.toW(), address_reg.toW(), 128);
        CMP(XSCRATCH1.toW(), 256);
        CSEL(XSCRATCH0.toW(), address_reg.toW(), WZR, Cond::LO);

        // index = (src_reg.GetIndex() + offset) & 0x7f;
        ADD(XSCRATCH0.toW(), XSCRATCH0.toW(), src_reg.GetIndex());
        AND(XSCRATCH0.toW(), XSCRATCH0.toW(), 0x7f);

        // index > 95 ? vec4(1.0) : uniforms.f[index];
        MOV(dest.B16(), ONE.B16());
        CMP(XSCRATCH0.toW(), 95);
        Label load_end;
        B(Cond::GT, load_end);
        LDR(dest, src_ptr, XSCRATCH0, IndexExt::LSL, 4);
        l(load_end);
    } else {
        // Load the source
        LDR(dest, src_ptr, src_offset_disp);
    }

    SwizzlePattern swiz = {(*swizzle_data)[operand_desc_id]};

    // Generate instructions for source register swizzling as needed
    u8 sel = swiz.GetRawSelector(src_num);
    if (sel != NO_SRC_REG_SWIZZLE) {
        // Selector component order needs to be reversed for the SHUFPS instruction
        sel = ((sel & 0xc0) >> 6) | ((sel & 3) << 6) | ((sel & 0xc) << 2) | ((sel & 0x30) >> 2);

        const int table[] = {((sel & 0xc0) >> 6), ((sel & 0x03) << 6), ((sel & 0x0c) << 2),
                             ((sel & 0x30) >> 2)};

        // Generate table-vector
        MOV(XSCRATCH0.toW(), u32(0x03'02'01'00u + (table[0] * 0x04'04'04'04u)));
        MOV(VSCRATCH0.Selem()[0], XSCRATCH0.toW());

        MOV(XSCRATCH0.toW(), u32(0x03'02'01'00u + (table[1] * 0x04'04'04'04u)));
        MOV(VSCRATCH0.Selem()[1], XSCRATCH0.toW());

        MOV(XSCRATCH0.toW(), u32(0x03'02'01'00u + (table[2] * 0x04'04'04'04u)));
        MOV(VSCRATCH0.Selem()[2], XSCRATCH0.toW());

        MOV(XSCRATCH0.toW(), u32(0x03'02'01'00u + (table[3] * 0x04'04'04'04u)));
        MOV(VSCRATCH0.Selem()[3], XSCRATCH0.toW());

        TBL(dest.B16(), List{dest.B16()}, VSCRATCH0.B16());
    }

    // If the source register should be negated, flip the negative bit using XOR
    const bool negate[] = {swiz.negate_src1, swiz.negate_src2, swiz.negate_src3};
    if (negate[src_num - 1]) {
        FNEG(dest.S4(), dest.S4());
    }
}

void JitShader::Compile_DestEnable(Instruction instr, QReg src) {
    DestRegister dest;
    u32 operand_desc_id;
    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD ||
        instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI) {
        operand_desc_id = instr.mad.operand_desc_id;
        dest = instr.mad.dest.Value();
    } else {
        operand_desc_id = instr.common.operand_desc_id;
        dest = instr.common.dest.Value();
    }

    SwizzlePattern swiz = {(*swizzle_data)[operand_desc_id]};

    std::size_t dest_offset_disp;
    switch (dest.GetRegisterType()) {
    case RegisterType::Output:
        dest_offset_disp = UnitState::OutputOffset(dest.GetIndex());
        break;
    case RegisterType::Temporary:
        dest_offset_disp = UnitState::TemporaryOffset(dest.GetIndex());
        break;
    default:
        UNREACHABLE_MSG("Encountered unknown destination register type: {}",
                        dest.GetRegisterType());
        break;
    }

    // If all components are enabled, write the result to the destination register
    if (swiz.dest_mask == NO_DEST_REG_MASK) {
        // Store dest back to memory
        STR(src, STATE, dest_offset_disp);

    } else {
        // Not all components are enabled, so mask the result when storing to the destination
        // register...
        LDR(VSCRATCH0, STATE, dest_offset_disp);

        const u32 mask = ((swiz.dest_mask & 1) ? 0xFF'00'00'00 : 0) |
                         ((swiz.dest_mask & 2) ? 0x00'FF'00'00 : 0) |
                         ((swiz.dest_mask & 4) ? 0x00'00'FF'00 : 0) |
                         ((swiz.dest_mask & 8) ? 0x00'00'00'FF : 0);

        MOV(XSCRATCH0.toW(), mask);
        MOV(VSCRATCH2.Selem()[0], XSCRATCH0.toW());

        // Widen 8->16
        ZIP1(VSCRATCH2.B16(), VSCRATCH2.B16(), VSCRATCH2.B16());
        // Widen 16->32
        ZIP1(VSCRATCH2.H8(), VSCRATCH2.H8(), VSCRATCH2.H8());

        // Select between src and dst using mask
        BSL(VSCRATCH2.B16(), VSCRATCH0.B16(), src.B16());

        // Store dest back to memory
        STR(VSCRATCH2, STATE, dest_offset_disp);
    }
}

void JitShader::Compile_SanitizedMul(QReg src1, QReg src2, QReg scratch0, QReg scratch1) {
    // 0 * inf and inf * 0 in the PICA should return 0 instead of NaN. This can be implemented by
    // checking for NaNs before and after the multiplication.  If the multiplication result is NaN
    // where neither source was, this NaN was generated by a 0 * inf multiplication, and so the
    // result should be transformed to 0 to match PICA fp rules.
    FMULX(VSCRATCH0.S4(), src1.S4(), src2.S4());
    FMUL(src1.S4(), src1.S4(), src2.S4());
    CMEQ(VSCRATCH0.S4(), VSCRATCH0.S4(), src1.S4());
    AND(src1.B16(), src1.B16(), VSCRATCH0.B16());
}

void JitShader::Compile_EvaluateCondition(Instruction instr) {
    // Note: NXOR is used below to check for equality
    switch (instr.flow_control.op) {
    case Instruction::FlowControlType::Or:
        MOV(XSCRATCH0, (instr.flow_control.refx.Value() ^ 1));
        MOV(XSCRATCH1, (instr.flow_control.refy.Value() ^ 1));
        EOR(XSCRATCH0, XSCRATCH0, COND0);
        EOR(XSCRATCH1, XSCRATCH1, COND1);
        ORR(XSCRATCH0, XSCRATCH0, XSCRATCH1);
        break;

    case Instruction::FlowControlType::And:
        MOV(XSCRATCH0, (instr.flow_control.refx.Value() ^ 1));
        MOV(XSCRATCH1, (instr.flow_control.refy.Value() ^ 1));
        EOR(XSCRATCH0, XSCRATCH0, COND0);
        EOR(XSCRATCH1, XSCRATCH1, COND1);
        AND(XSCRATCH0, XSCRATCH0, XSCRATCH1);
        break;

    case Instruction::FlowControlType::JustX:
        MOV(XSCRATCH0, (instr.flow_control.refx.Value() ^ 1));
        EOR(XSCRATCH0, XSCRATCH0, COND0);
        break;

    case Instruction::FlowControlType::JustY:
        MOV(XSCRATCH0, (instr.flow_control.refy.Value() ^ 1));
        EOR(XSCRATCH0, XSCRATCH0, COND1);
        break;
    }
    CMP(XSCRATCH0, 0);
}

void JitShader::Compile_UniformCondition(Instruction instr) {
    const std::size_t offset = Uniforms::GetBoolUniformOffset(instr.flow_control.bool_uniform_id);
    LDRB(XSCRATCH0.toW(), UNIFORMS, offset);
    CMP(XSCRATCH0.toW(), 0);
}

std::bitset<64> JitShader::PersistentCallerSavedRegs() {
    return persistent_regs & ABI_ALL_CALLER_SAVED;
}

void JitShader::Compile_ADD(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    FADD(SRC1.S4(), SRC1.S4(), SRC2.S4());
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_DP3(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    Compile_SanitizedMul(SRC1, SRC2, VSCRATCH0, VSCRATCH1);

    // Set last element to 0.0
    MOV(SRC1.Selem()[3], WZR);

    FADDP(SRC1.S4(), SRC1.S4(), SRC1.S4());
    FADDP(SRC1.toS(), SRC1.toD().S2());
    DUP(SRC1.S4(), SRC1.Selem()[0]);

    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_DP4(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    Compile_SanitizedMul(SRC1, SRC2, VSCRATCH0, VSCRATCH1);

    FADDP(SRC1.S4(), SRC1.S4(), SRC1.S4());
    FADDP(SRC1.toS(), SRC1.toD().S2());
    DUP(SRC1.S4(), SRC1.Selem()[0]);

    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_DPH(Instruction instr) {
    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::DPHI) {
        Compile_SwizzleSrc(instr, 1, instr.common.src1i, SRC1);
        Compile_SwizzleSrc(instr, 2, instr.common.src2i, SRC2);
    } else {
        Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
        Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    }

    // Set 4th component to 1.0
    MOV(SRC1.Selem()[3], ONE.Selem()[0]);

    Compile_SanitizedMul(SRC1, SRC2, VSCRATCH0, VSCRATCH1);

    FADDP(SRC1.S4(), SRC1.S4(), SRC1.S4());
    FADDP(SRC1.toS(), SRC1.toD().S2());
    DUP(SRC1.S4(), SRC1.Selem()[0]);

    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_EX2(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    BL(exp2_subroutine);
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_LG2(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    BL(log2_subroutine);
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_MUL(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    Compile_SanitizedMul(SRC1, SRC2, VSCRATCH0, VSCRATCH1);
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_SGE(Instruction instr) {
    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::SGEI) {
        Compile_SwizzleSrc(instr, 1, instr.common.src1i, SRC1);
        Compile_SwizzleSrc(instr, 2, instr.common.src2i, SRC2);
    } else {
        Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
        Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    }

    FCMGE(SRC2.S4(), SRC1.S4(), SRC2.S4());
    AND(SRC2.B16(), SRC2.B16(), ONE.B16());

    Compile_DestEnable(instr, SRC2);
}

void JitShader::Compile_SLT(Instruction instr) {
    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::SLTI) {
        Compile_SwizzleSrc(instr, 1, instr.common.src1i, SRC1);
        Compile_SwizzleSrc(instr, 2, instr.common.src2i, SRC2);
    } else {
        Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
        Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);
    }

    FCMGT(SRC1.S4(), SRC2.S4(), SRC1.S4());
    AND(SRC1.B16(), SRC1.B16(), ONE.B16());

    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_FLR(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    FRINTM(SRC1.S4(), SRC1.S4());
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_MAX(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    // VSCRATCH0 = Ordinal(SRC1, SRC2)
    FCMEQ(VSCRATCH0.S4(), SRC1.S4(), SRC1.S4());
    FCMEQ(VSCRATCH1.S4(), SRC2.S4(), SRC2.S4());
    AND(VSCRATCH0.B16(), VSCRATCH0.B16(), VSCRATCH1.B16());

    FMAX(SRC1.S4(), SRC1.S4(), SRC2.S4());

    // In the case of NaN, pick SRC2
    BIF(SRC1.B16(), SRC2.B16(), VSCRATCH0.B16());

    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_MIN(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    // VSCRATCH0 = Ordinal(SRC1, SRC2)
    FCMEQ(VSCRATCH0.S4(), SRC1.S4(), SRC1.S4());
    FCMEQ(VSCRATCH1.S4(), SRC2.S4(), SRC2.S4());
    AND(VSCRATCH0.B16(), VSCRATCH0.B16(), VSCRATCH1.B16());

    FMIN(SRC1.S4(), SRC1.S4(), SRC2.S4());

    // In the case of NaN, pick SRC2
    BIF(SRC1.B16(), SRC2.B16(), VSCRATCH0.B16());

    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_MOVA(Instruction instr) {
    SwizzlePattern swiz = {(*swizzle_data)[instr.common.operand_desc_id]};

    if (!swiz.DestComponentEnabled(0) && !swiz.DestComponentEnabled(1)) {
        return;
    }

    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);

    // Convert floats to integers using truncation (only care about X and Y components)
    FCVTZS(SRC1.S4(), SRC1.S4());

    // Get result
    MOV(XSCRATCH0, SRC1.Delem()[0]);

    // Handle destination enable
    if (swiz.DestComponentEnabled(0) && swiz.DestComponentEnabled(1)) {
        // Move and sign-extend low 32 bits
        SXTW(ADDROFFS_REG_0, XSCRATCH0.toW());

        // Move and sign-extend high 32 bits
        LSR(XSCRATCH0, XSCRATCH0, 32);
        SXTW(ADDROFFS_REG_1, XSCRATCH0.toW());
    } else {
        if (swiz.DestComponentEnabled(0)) {
            // Move and sign-extend low 32 bits
            SXTW(ADDROFFS_REG_0, XSCRATCH0.toW());
        } else if (swiz.DestComponentEnabled(1)) {
            // Move and sign-extend high 32 bits
            LSR(XSCRATCH0, XSCRATCH0, 32);
            SXTW(ADDROFFS_REG_1, XSCRATCH0.toW());
        }
    }
}

void JitShader::Compile_MOV(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_RCP(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);

    // FRECPE can be pretty inaccurate
    // FRECPE(1.0f) = 0.99805f != 1.0f
    // FRECPE(SRC1.S4(), SRC1.S4());
    // Just do an exact 1.0f / N
    FDIV(SRC1.toS(), ONE.toS(), SRC1.toS());

    DUP(SRC1.S4(), SRC1.Selem()[0]); // XYWZ -> XXXX
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_RSQ(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);

    // FRSQRTE can be pretty inaccurate
    // FRSQRTE(8.0f) = 0.35254f != 0.3535533845
    // FRSQRTE(SRC1.S4(), SRC1.S4());
    // Just do an exact 1.0f / sqrt(N)
    FSQRT(SRC1.toS(), SRC1.toS());
    FDIV(SRC1.toS(), ONE.toS(), SRC1.toS());

    DUP(SRC1.S4(), SRC1.Selem()[0]); // XYWZ -> XXXX
    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_NOP(Instruction instr) {}

void JitShader::Compile_END(Instruction instr) {
    // Save conditional code
    STRB(COND0.toW(), STATE, u32(offsetof(UnitState, conditional_code[0])));
    STRB(COND1.toW(), STATE, u32(offsetof(UnitState, conditional_code[1])));

    // Save address/loop registers
    STR(ADDROFFS_REG_0.toW(), STATE, u32(offsetof(UnitState, address_registers[0])));
    STR(ADDROFFS_REG_1.toW(), STATE, u32(offsetof(UnitState, address_registers[1])));
    STR(LOOPCOUNT_REG.toW(), STATE, u32(offsetof(UnitState, address_registers[2])));

    ABI_PopRegisters(*this, ABI_ALL_CALLEE_SAVED, 16);
    RET();
}

void JitShader::Compile_BREAKC(Instruction instr) {
    Compile_Assert(loop_depth, "BREAKC must be inside a LOOP");
    if (loop_depth) {
        Compile_EvaluateCondition(instr);
        ASSERT(!loop_break_labels.empty());
        B(Cond::NE, loop_break_labels.back());
    }
}

void JitShader::Compile_CALL(Instruction instr) {
    // Push offset of the return
    MOV(XSCRATCH0, instr.flow_control.dest_offset + instr.flow_control.num_instructions);
    STR(XSCRATCH0, SP, POST_INDEXED, -16);

    // Call the subroutine
    BL(instruction_labels[instr.flow_control.dest_offset]);

    // Skip over the return offset that's on the stack
    ADD(SP, SP, 16);
}

void JitShader::Compile_CALLC(Instruction instr) {
    Compile_EvaluateCondition(instr);
    Label b;
    B(Cond::EQ, b);
    Compile_CALL(instr);
    l(b);
}

void JitShader::Compile_CALLU(Instruction instr) {
    Compile_UniformCondition(instr);
    Label b;
    B(Cond::NE, b);
    Compile_CALL(instr);
    l(b);
}

void JitShader::Compile_CMP(Instruction instr) {
    using Op = Instruction::Common::CompareOpType::Op;
    Op op_x = instr.common.compare_op.x;
    Op op_y = instr.common.compare_op.y;

    Compile_SwizzleSrc(instr, 1, instr.common.src1, SRC1);
    Compile_SwizzleSrc(instr, 2, instr.common.src2, SRC2);

    static constexpr Cond cmp[] = {Cond::EQ, Cond::NE, Cond::LT, Cond::LE, Cond::GT, Cond::GE};

    MOV(X1, 1);

    // Compare X-component
    FCMP(SRC1.toS(), SRC2.toS());
    CSET(COND0, cmp[op_x]);

    // Compare Y-component
    MOV(VSCRATCH0.toS(), SRC1.Selem()[1]);
    MOV(VSCRATCH1.toS(), SRC2.Selem()[1]);
    FCMP(VSCRATCH0.toS(), VSCRATCH1.toS());
    CSET(COND1, cmp[op_y]);
}

void JitShader::Compile_MAD(Instruction instr) {
    Compile_SwizzleSrc(instr, 1, instr.mad.src1, SRC1);

    if (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI) {
        Compile_SwizzleSrc(instr, 2, instr.mad.src2i, SRC2);
        Compile_SwizzleSrc(instr, 3, instr.mad.src3i, SRC3);
    } else {
        Compile_SwizzleSrc(instr, 2, instr.mad.src2, SRC2);
        Compile_SwizzleSrc(instr, 3, instr.mad.src3, SRC3);
    }

    Compile_SanitizedMul(SRC1, SRC2, VSCRATCH0, VSCRATCH1);
    FADD(SRC1.S4(), SRC1.S4(), SRC3.S4());

    Compile_DestEnable(instr, SRC1);
}

void JitShader::Compile_IF(Instruction instr) {
    Compile_Assert(instr.flow_control.dest_offset >= program_counter,
                   "Backwards if-statements not supported");
    Label l_else, l_endif;

    // Evaluate the "IF" condition
    if (instr.opcode.Value() == OpCode::Id::IFU) {
        Compile_UniformCondition(instr);
    } else if (instr.opcode.Value() == OpCode::Id::IFC) {
        Compile_EvaluateCondition(instr);
    }
    B(Cond::EQ, l_else);

    // Compile the code that corresponds to the condition evaluating as true
    Compile_Block(instr.flow_control.dest_offset);

    // If there isn't an "ELSE" condition, we are done here
    if (instr.flow_control.num_instructions == 0) {
        l(l_else);
        return;
    }

    B(l_endif);

    l(l_else);
    // This code corresponds to the "ELSE" condition
    // Comple the code that corresponds to the condition evaluating as false
    Compile_Block(instr.flow_control.dest_offset + instr.flow_control.num_instructions);

    l(l_endif);
}

void JitShader::Compile_LOOP(Instruction instr) {
    Compile_Assert(instr.flow_control.dest_offset >= program_counter,
                   "Backwards loops not supported");
    Compile_Assert(loop_depth < 1, "Nested loops may not be supported");
    if (loop_depth++) {
        const auto loop_save_regs = BuildRegSet({LOOPCOUNT_REG, LOOPINC, LOOPCOUNT});
        ABI_PushRegisters(*this, loop_save_regs);
    }

    // This decodes the fields from the integer uniform at index instr.flow_control.int_uniform_id
    const std::size_t offset = Uniforms::GetIntUniformOffset(instr.flow_control.int_uniform_id);
    LDR(LOOPCOUNT, UNIFORMS, offset);
    LSR(LOOPCOUNT_REG, LOOPCOUNT, 8);
    AND(LOOPCOUNT_REG, LOOPCOUNT_REG, 0xFF); // Y-component is the start
    LSR(LOOPINC, LOOPCOUNT, 16);
    AND(LOOPINC, LOOPINC, 0xFF);  // Z-component is the incrementer
    UXTB(LOOPCOUNT, LOOPCOUNT);   // X-component is iteration count
    ADD(LOOPCOUNT, LOOPCOUNT, 1); // Iteration count is X-component + 1

    Label l_loop_start;
    l(l_loop_start);

    loop_break_labels.emplace_back(oaknut::Label());
    Compile_Block(instr.flow_control.dest_offset + 1);
    ADD(LOOPCOUNT_REG, LOOPCOUNT_REG, LOOPINC); // Increment LOOPCOUNT_REG by Z-component
    SUB(LOOPCOUNT, LOOPCOUNT, 1);               // Increment loop count by 1
    CMP(LOOPCOUNT, 0);
    B(Cond::NE, l_loop_start); // Loop if not equal

    l(loop_break_labels.back());
    loop_break_labels.pop_back();

    if (--loop_depth) {
        const auto loop_save_regs = BuildRegSet({LOOPCOUNT_REG, LOOPINC, LOOPCOUNT});
        ABI_PopRegisters(*this, loop_save_regs);
    }
}

void JitShader::Compile_JMP(Instruction instr) {
    if (instr.opcode.Value() == OpCode::Id::JMPC) {
        Compile_EvaluateCondition(instr);
    } else if (instr.opcode.Value() == OpCode::Id::JMPU) {
        Compile_UniformCondition(instr);
    } else {
        UNREACHABLE();
    }

    const bool inverted_condition =
        (instr.opcode.Value() == OpCode::Id::JMPU) && (instr.flow_control.num_instructions & 1);

    Label& b = instruction_labels[instr.flow_control.dest_offset];
    if (inverted_condition) {
        B(Cond::EQ, b);
    } else {
        B(Cond::NE, b);
    }
}

static void Emit(GSEmitter* emitter, Common::Vec4<f24> (*output)[16]) {
    emitter->Emit(*output);
}

void JitShader::Compile_EMIT(Instruction instr) {
    Label have_emitter, end;

    LDR(XSCRATCH0, STATE, u32(offsetof(UnitState, emitter_ptr)));
    CMP(XSCRATCH0, 0);
    B(Cond::NE, have_emitter);

    ABI_PushRegisters(*this, PersistentCallerSavedRegs());
    MOVP2R(ABI_PARAM1, reinterpret_cast<const void*>("Execute EMIT on VS"));
    CallFarFunction(*this, LogCritical);
    ABI_PopRegisters(*this, PersistentCallerSavedRegs());
    B(end);

    l(have_emitter);
    ABI_PushRegisters(*this, PersistentCallerSavedRegs());
    MOV(ABI_PARAM1, XSCRATCH0);
    MOV(ABI_PARAM2, STATE);
    ADD(ABI_PARAM2, ABI_PARAM2, u32(offsetof(UnitState, registers.output)));
    CallFarFunction(*this, Emit);
    ABI_PopRegisters(*this, PersistentCallerSavedRegs());
    l(end);
}

void JitShader::Compile_SETE(Instruction instr) {
    Label have_emitter, end;

    LDR(XSCRATCH0, STATE, u32(offsetof(UnitState, emitter_ptr)));

    CMP(XSCRATCH0, 0);
    B(Cond::NE, have_emitter);

    ABI_PushRegisters(*this, PersistentCallerSavedRegs());
    MOVP2R(ABI_PARAM1, reinterpret_cast<const void*>("Execute SETEMIT on VS"));
    CallFarFunction(*this, LogCritical);
    ABI_PopRegisters(*this, PersistentCallerSavedRegs());
    B(end);

    l(have_emitter);

    MOV(XSCRATCH1.toW(), instr.setemit.vertex_id);
    STRB(XSCRATCH1.toW(), XSCRATCH0, u32(offsetof(GSEmitter, vertex_id)));
    MOV(XSCRATCH1.toW(), instr.setemit.prim_emit);
    STRB(XSCRATCH1.toW(), XSCRATCH0, u32(offsetof(GSEmitter, prim_emit)));
    MOV(XSCRATCH1.toW(), instr.setemit.winding);
    STRB(XSCRATCH1.toW(), XSCRATCH0, u32(offsetof(GSEmitter, winding)));

    l(end);
}

void JitShader::Compile_Block(unsigned end) {
    while (program_counter < end) {
        Compile_NextInstr();
    }
}

void JitShader::Compile_Return() {
    // Peek return offset on the stack and check if we're at that offset
    LDR(XSCRATCH0, SP, 16);
    CMP(XSCRATCH0.toW(), program_counter);

    // If so, jump back to before CALL
    Label b;
    B(Cond::NE, b);
    RET();
    l(b);
}

void JitShader::Compile_NextInstr() {
    if (std::binary_search(return_offsets.begin(), return_offsets.end(), program_counter)) {
        Compile_Return();
    }

    l(instruction_labels[program_counter]);

    Instruction instr = {(*program_code)[program_counter++]};

    OpCode::Id opcode = instr.opcode.Value();
    auto instr_func = instr_table[static_cast<unsigned>(opcode)];

    if (instr_func) {
        // JIT the instruction!
        ((*this).*instr_func)(instr);
    } else {
        // Unhandled instruction
        LOG_CRITICAL(HW_GPU, "Unhandled instruction: 0x{:02x} (0x{:08x})",
                     static_cast<u32>(instr.opcode.Value().EffectiveOpCode()), instr.hex);
    }
}

void JitShader::FindReturnOffsets() {
    return_offsets.clear();

    for (std::size_t offset = 0; offset < program_code->size(); ++offset) {
        Instruction instr = {(*program_code)[offset]};

        switch (instr.opcode.Value()) {
        case OpCode::Id::CALL:
        case OpCode::Id::CALLC:
        case OpCode::Id::CALLU:
            return_offsets.push_back(instr.flow_control.dest_offset +
                                     instr.flow_control.num_instructions);
            break;
        default:
            break;
        }
    }

    // Sort for efficient binary search later
    std::sort(return_offsets.begin(), return_offsets.end());
}

void JitShader::Compile(const std::array<u32, MAX_PROGRAM_CODE_LENGTH>* program_code_,
                        const std::array<u32, MAX_SWIZZLE_DATA_LENGTH>* swizzle_data_) {
    program_code = program_code_;
    swizzle_data = swizzle_data_;

    // Reset flow control state
    program = (CompiledShader*)current_address();
    program_counter = 0;
    loop_depth = 0;
    instruction_labels.fill(Label());

    // Find all `CALL` instructions and identify return locations
    FindReturnOffsets();

    // The stack pointer is 8 modulo 16 at the entry of a procedure
    // We reserve 16 bytes and assign a dummy value to the first 8 bytes, to catch any potential
    // return checks (see Compile_Return) that happen in shader main routine.
    ABI_PushRegisters(*this, ABI_ALL_CALLEE_SAVED, 16);
    MVN(XSCRATCH0, XZR);
    STR(XSCRATCH0, SP, 8);

    MOV(UNIFORMS, ABI_PARAM1);
    MOV(STATE, ABI_PARAM2);

    // Load address/loop registers
    LDR(ADDROFFS_REG_0.toW(), STATE, u32(offsetof(UnitState, address_registers[0])));
    LDR(ADDROFFS_REG_1.toW(), STATE, u32(offsetof(UnitState, address_registers[1])));
    LDR(LOOPCOUNT_REG.toW(), STATE, u32(offsetof(UnitState, address_registers[2])));

    //// Load conditional code
    LDRB(COND0.toW(), STATE, u32(offsetof(UnitState, conditional_code[0])));
    LDRB(COND1.toW(), STATE, u32(offsetof(UnitState, conditional_code[1])));

    // Used to set a register to one
    static const float one[] = {1.f, 1.f, 1.f, 1.f};
    MOVP2R(XSCRATCH0, one);
    LDR(ONE, XSCRATCH0);

    // Jump to start of the shader program
    BR(ABI_PARAM3);

    // Compile entire program
    Compile_Block(static_cast<unsigned>(program_code->size()));

    // Free memory that's no longer needed
    program_code = nullptr;
    swizzle_data = nullptr;
    return_offsets.clear();
    return_offsets.shrink_to_fit();

    // Memory is ready to execute
    protect();
    invalidate_all();

    const size_t code_size =
        current_address() - reinterpret_cast<uintptr_t>(oaknut::CodeBlock::ptr());

    ASSERT_MSG(code_size <= MAX_SHADER_SIZE, "Compiled a shader that exceeds the allocated size!");
    LOG_DEBUG(HW_GPU, "Compiled shader size={}", code_size);
}

JitShader::JitShader()
    : oaknut::CodeBlock(MAX_SHADER_SIZE), oaknut::CodeGenerator(oaknut::CodeBlock::ptr()) {
    unprotect();
    CompilePrelude();
}

void JitShader::CompilePrelude() {
    log2_subroutine = CompilePrelude_Log2();
    exp2_subroutine = CompilePrelude_Exp2();
}

oaknut::Label JitShader::CompilePrelude_Log2() {
    oaknut::Label subroutine;

    // We perform this approximation first performaing
    // a range reduction into the range [1.0, 2.0). A minimax
    // polynomial which was fit for the function log2(x) / (x - 1)
    // is then evaluated. We multiply the result by (x - 1) then
    // restore the result into the appropriate range.

    // Coefficients for the minimax polynomial.
    // f(x) computes approximately log2(x) / (x - 1).
    // f(x) = c4 + x * (c3 + x * (c2 + x * (c1 + x * c0)).
    oaknut::Label c0;
    align(16);
    l(c0);
    dw(0x3d74552f);

    align(16);
    oaknut::Label c14;
    l(c14);
    dw(0xbeee7397);
    dw(0x3fbd96dd);
    dw(0xc02153f6);
    dw(0x4038d96c);

    align(16);
    oaknut::Label negative_infinity_vector;
    l(negative_infinity_vector);
    dw(0xff800000);
    dw(0xff800000);
    dw(0xff800000);
    dw(0xff800000);
    oaknut::Label default_qnan_vector;
    l(default_qnan_vector);
    dw(0x7fc00000);
    dw(0x7fc00000);
    dw(0x7fc00000);
    dw(0x7fc00000);

    oaknut::Label input_is_nan, input_is_zero, input_out_of_range;

    align(16);
    l(input_out_of_range);
    B(Cond::EQ, input_is_zero);
    MOVP2R(XSCRATCH0, default_qnan_vector.ptr<void*>());
    LDR(SRC1, XSCRATCH0);
    RET();

    l(input_is_zero);
    MOVP2R(XSCRATCH0, negative_infinity_vector.ptr<void*>());
    LDR(SRC1, XSCRATCH0);
    RET();

    align(16);
    l(subroutine);

    // Here we handle edge cases: input in {NaN, 0, -Inf, Negative}.
    // Ordinal(n) ? 0xFFFFFFFF : 0x0
    FCMEQ(VSCRATCH0.toS(), SRC1.toS(), SRC1.toS());
    MOV(XSCRATCH0.toW(), VSCRATCH0.Selem()[0]);
    CMP(XSCRATCH0.toW(), 0);
    B(Cond::EQ, input_is_nan); // SRC1 == NaN

    // (0.0 >= n) ? 0xFFFFFFFF : 0x0
    MOV(XSCRATCH0.toW(), SRC1.Selem()[0]);
    CMP(XSCRATCH0.toW(), 0);
    B(Cond::LE, input_out_of_range); // SRC1 <= 0.0

    // Split input: SRC1=MANT[1,2) VSCRATCH1=Exponent
    MOV(XSCRATCH0.toW(), SRC1.Selem()[0]);
    MOV(XSCRATCH1.toW(), XSCRATCH0.toW());
    AND(XSCRATCH0.toW(), XSCRATCH0.toW(), 0x7f800000);
    AND(XSCRATCH1.toW(), XSCRATCH1.toW(), 0x007fffff);
    ORR(XSCRATCH1.toW(), XSCRATCH1.toW(), 0x3f800000);
    MOV(SRC1.Selem()[0], XSCRATCH1.toW());
    //  SRC1 now contains the mantissa of the input.
    LSR(XSCRATCH0.toW(), XSCRATCH0.toW(), 23);
    SUB(XSCRATCH0.toW(), XSCRATCH0.toW(), 0x7F);
    MOV(VSCRATCH1.Selem()[0], XSCRATCH0.toW());
    UCVTF(VSCRATCH1.toS(), VSCRATCH1.toS()); // cvtsi2ss(VSCRATCH1, eax);
    // VSCRATCH1 now contains the exponent of the input.

    MOVP2R(XSCRATCH0, c0.ptr<void*>());
    LDR(XSCRATCH0.toW(), XSCRATCH0);
    MOV(VSCRATCH0.Selem()[0], XSCRATCH0.toW());

    // Complete computation of polynomial
    // Load C1,C2,C3,C4 into a single scratch register
    const QReg C14 = SRC2;
    MOVP2R(XSCRATCH0, c14.ptr<void*>());
    LDR(C14, XSCRATCH0);
    FMUL(VSCRATCH0.toS(), VSCRATCH0.toS(), SRC1.toS());
    FMLA(VSCRATCH0.toS(), ONE.toS(), C14.Selem()[0]);
    FMUL(VSCRATCH0.toS(), VSCRATCH0.toS(), SRC1.toS());
    FMLA(VSCRATCH0.toS(), ONE.toS(), C14.Selem()[1]);
    FMUL(VSCRATCH0.toS(), VSCRATCH0.toS(), SRC1.toS());
    FMLA(VSCRATCH0.toS(), ONE.toS(), C14.Selem()[2]);
    FMUL(VSCRATCH0.toS(), VSCRATCH0.toS(), SRC1.toS());

    FSUB(SRC1.toS(), SRC1.toS(), ONE.toS());
    FMLA(VSCRATCH0.toS(), ONE.toS(), C14.Selem()[3]);

    FMUL(VSCRATCH0.toS(), VSCRATCH0.toS(), SRC1.toS());
    FADD(VSCRATCH1.toS(), VSCRATCH0.toS(), VSCRATCH1.toS());

    // Duplicate result across vector
    MOV(SRC1.Selem()[0], VSCRATCH1.Selem()[0]);
    l(input_is_nan);
    DUP(SRC1.S4(), SRC1.Selem()[0]);

    RET();

    return subroutine;
}

oaknut::Label JitShader::CompilePrelude_Exp2() {
    oaknut::Label subroutine;

    // SSE does not have a exp instruction, thus we must
    // approximate. We perform this approximation first performaing
    // a range reduction into the range [-0.5, 0.5). A minimax
    // polynomial which was fit for the function exp2(x) is then
    // evaluated. We then restore the result into the appropriate
    // range.

    align(16);
    const void* input_max = (const void*)current_address();
    dw(0x43010000);
    const void* input_min = (const void*)current_address();
    dw(0xc2fdffff);
    const void* c0 = (const void*)current_address();
    dw(0x3c5dbe69);
    const void* half = (const void*)current_address();
    dw(0x3f000000);
    const void* c1 = (const void*)current_address();
    dw(0x3d5509f9);
    const void* c2 = (const void*)current_address();
    dw(0x3e773cc5);
    const void* c3 = (const void*)current_address();
    dw(0x3f3168b3);
    const void* c4 = (const void*)current_address();
    dw(0x3f800016);

    oaknut::Label ret_label;

    align(16);
    l(subroutine);

    // Handle edge cases
    FCMP(SRC1.toS(), SRC1.toS());
    B(Cond::NE, ret_label); // branch if NaN

    // Decompose input:
    // VSCRATCH0=2^round(input)
    // SRC1=input-round(input) [-0.5, 0.5)
    // Clamp to maximum range since we shift the value directly into the exponent.
    MOVP2R(XSCRATCH0, input_max);
    LDR(VSCRATCH0.toS(), XSCRATCH0);
    FMIN(SRC1.toS(), SRC1.toS(), VSCRATCH0.toS());

    MOVP2R(XSCRATCH0, input_min);
    LDR(VSCRATCH0.toS(), XSCRATCH0);
    FMAX(SRC1.toS(), SRC1.toS(), VSCRATCH0.toS());

    MOVP2R(XSCRATCH0, half);
    LDR(VSCRATCH0.toS(), XSCRATCH0);
    FSUB(VSCRATCH0.toS(), SRC1.toS(), VSCRATCH0.toS());

    FCVTNS(VSCRATCH0.toS(), VSCRATCH0.toS());
    MOV(XSCRATCH0.toW(), VSCRATCH0.Selem()[0]);
    SCVTF(VSCRATCH0.toS(), XSCRATCH0.toW());

    // VSCRATCH0 now contains input rounded to the nearest integer.
    ADD(XSCRATCH0.toW(), XSCRATCH0.toW(), 0x7F);
    FSUB(SRC1.toS(), SRC1.toS(), VSCRATCH0.toS());
    // SRC1 contains input - round(input), which is in [-0.5, 0.5).
    LSL(XSCRATCH0.toW(), XSCRATCH0.toW(), 23);
    MOV(VSCRATCH0.Selem()[0], XSCRATCH0.toW());
    // VSCRATCH0 contains 2^(round(input)).

    // Complete computation of polynomial.
    ADR(XSCRATCH1, c0);
    LDR(VSCRATCH1.toS(), XSCRATCH1);
    FMUL(VSCRATCH1.toS(), SRC1.toS(), VSCRATCH1.toS());

    ADR(XSCRATCH1, c1);
    LDR(VSCRATCH2.toS(), XSCRATCH1);
    FADD(VSCRATCH1.toS(), VSCRATCH1.toS(), VSCRATCH2.toS());
    FMUL(VSCRATCH1.toS(), VSCRATCH1.toS(), SRC1.toS());

    ADR(XSCRATCH1, c2);
    LDR(VSCRATCH2.toS(), XSCRATCH1);
    FADD(VSCRATCH1.toS(), VSCRATCH1.toS(), VSCRATCH2.toS());
    FMUL(VSCRATCH1.toS(), VSCRATCH1.toS(), SRC1.toS());

    ADR(XSCRATCH1, c3);
    LDR(VSCRATCH2.toS(), XSCRATCH1);
    FADD(VSCRATCH1.toS(), VSCRATCH1.toS(), VSCRATCH2.toS());
    FMUL(SRC1.toS(), VSCRATCH1.toS(), SRC1.toS());

    ADR(XSCRATCH1, c4);
    LDR(VSCRATCH2.toS(), XSCRATCH1);
    FADD(SRC1.toS(), VSCRATCH2.toS(), SRC1.toS());

    FMUL(SRC1.toS(), SRC1.toS(), VSCRATCH0.toS());

    // Duplicate result across vector
    l(ret_label);
    DUP(SRC1.S4(), SRC1.Selem()[0]);

    RET();

    return subroutine;
}

} // namespace Pica::Shader

#endif // CITRA_ARCH(arm64)
