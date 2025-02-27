#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <nihstro/inline_assembly.h>
#include "video_core/shader/shader_interpreter.h"
#if CITRA_ARCH(x86_64)
#include "video_core/shader/shader_jit_x64_compiler.h"
#elif CITRA_ARCH(arm64)
#include "video_core/shader/shader_jit_a64_compiler.h"
#endif

using JitShader = Pica::Shader::JitShader;
using ShaderInterpreter = Pica::Shader::InterpreterEngine;

using DestRegister = nihstro::DestRegister;
using OpCode = nihstro::OpCode;
using SourceRegister = nihstro::SourceRegister;
using Type = nihstro::InlineAsm::Type;

static constexpr Common::Vec4f vec4_inf = Common::Vec4f::AssignToAll(INFINITY);
static constexpr Common::Vec4f vec4_nan = Common::Vec4f::AssignToAll(NAN);
static constexpr Common::Vec4f vec4_one = Common::Vec4f::AssignToAll(1.0f);
static constexpr Common::Vec4f vec4_zero = Common::Vec4f::AssignToAll(0.0f);

static std::unique_ptr<Pica::Shader::ShaderSetup> CompileShaderSetup(
    std::initializer_list<nihstro::InlineAsm> code) {
    const auto shbin = nihstro::InlineAsm::CompileToRawBinary(code);

    auto shader = std::make_unique<Pica::Shader::ShaderSetup>();

    std::transform(shbin.program.begin(), shbin.program.end(), shader->program_code.begin(),
                   [](const auto& x) { return x.hex; });
    std::transform(shbin.swizzle_table.begin(), shbin.swizzle_table.end(),
                   shader->swizzle_data.begin(), [](const auto& x) { return x.hex; });

    return shader;
}

class ShaderTest {
public:
    explicit ShaderTest(std::initializer_list<nihstro::InlineAsm> code)
        : shader_setup(CompileShaderSetup(code)) {
        shader_jit.Compile(&shader_setup->program_code, &shader_setup->swizzle_data);
    }

    Common::Vec4f Run(std::span<const Common::Vec4f> inputs) {
        Pica::Shader::UnitState shader_unit;
        RunJit(shader_unit, inputs);
        return {shader_unit.registers.output[0].x.ToFloat32(),
                shader_unit.registers.output[0].y.ToFloat32(),
                shader_unit.registers.output[0].z.ToFloat32(),
                shader_unit.registers.output[0].w.ToFloat32()};
    }

    Common::Vec4f Run(std::initializer_list<float> inputs) {
        std::vector<Common::Vec4f> input_vecs;
        for (const float& input : inputs) {
            input_vecs.emplace_back(input, 0.0f, 0.0f, 0.0f);
        }
        return Run(input_vecs);
    }

    Common::Vec4f Run(float input) {
        return Run({input});
    }

    Common::Vec4f Run(std::initializer_list<Common::Vec4f> inputs) {
        return Run(std::vector<Common::Vec4f>{inputs});
    }

    void RunJit(Pica::Shader::UnitState& shader_unit, std::span<const Common::Vec4f> inputs) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const Common::Vec4f& input = inputs[i];
            shader_unit.registers.input[i].x = Pica::f24::FromFloat32(input.x);
            shader_unit.registers.input[i].y = Pica::f24::FromFloat32(input.y);
            shader_unit.registers.input[i].z = Pica::f24::FromFloat32(input.z);
            shader_unit.registers.input[i].w = Pica::f24::FromFloat32(input.w);
        }
        shader_unit.registers.temporary.fill(
            Common::Vec4<Pica::f24>::AssignToAll(Pica::f24::Zero()));
        shader_jit.Run(*shader_setup, shader_unit, 0);
    }

    void RunJit(Pica::Shader::UnitState& shader_unit, float input) {
        const Common::Vec4f input_vec(input, 0, 0, 0);
        RunJit(shader_unit, {&input_vec, 1});
    }

    void RunInterpreter(Pica::Shader::UnitState& shader_unit,
                        std::span<const Common::Vec4f> inputs) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const Common::Vec4f& input = inputs[i];
            shader_unit.registers.input[i].x = Pica::f24::FromFloat32(input.x);
            shader_unit.registers.input[i].y = Pica::f24::FromFloat32(input.y);
            shader_unit.registers.input[i].z = Pica::f24::FromFloat32(input.z);
            shader_unit.registers.input[i].w = Pica::f24::FromFloat32(input.w);
        }
        shader_unit.registers.temporary.fill(
            Common::Vec4<Pica::f24>::AssignToAll(Pica::f24::Zero()));
        shader_interpreter.Run(*shader_setup, shader_unit);
    }

    void RunInterpreter(Pica::Shader::UnitState& shader_unit, float input) {
        const Common::Vec4f input_vec(input, 0, 0, 0);
        RunInterpreter(shader_unit, {&input_vec, 1});
    }

public:
    JitShader shader_jit;
    ShaderInterpreter shader_interpreter;
    std::unique_ptr<Pica::Shader::ShaderSetup> shader_setup;
};

/*
TEST_CASE("ADD", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::ADD, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({+1.0f, -1.0f}).x == +0.0f);
    REQUIRE(shader.Run({+0.0f, -0.0f}).x == -0.0f);
    REQUIRE(std::isnan(shader.Run({+INFINITY, -INFINITY}).x));
    REQUIRE(std::isinf(shader.Run({INFINITY, +1.0f}).x));
    REQUIRE(std::isinf(shader.Run({INFINITY, -1.0f}).x));
}

TEST_CASE("DP3", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::DP3, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({vec4_inf, vec4_zero}).x == 0.0f);
    REQUIRE(std::isnan(shader.Run({vec4_nan, vec4_zero}).x));

    REQUIRE(shader.Run({vec4_one, vec4_one}).x == 3.0f);
}

TEST_CASE("DP4", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::DP4, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({vec4_inf, vec4_zero}).x == 0.0f);
    REQUIRE(std::isnan(shader.Run({vec4_nan, vec4_zero}).x));

    REQUIRE(shader.Run({vec4_one, vec4_one}).x == 4.0f);
}

TEST_CASE("DPH", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::DPH, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({vec4_inf, vec4_zero}).x == 0.0f);
    REQUIRE(std::isnan(shader.Run({vec4_nan, vec4_zero}).x));

    REQUIRE(shader.Run({vec4_one, vec4_one}).x == 4.0f);
    REQUIRE(shader.Run({vec4_zero, vec4_one}).x == 1.0f);
}

TEST_CASE("LG2", "[video_core][shader][shader_jit]") {
    const auto sh_input = SourceRegister::MakeInput(0);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::LG2, sh_output, sh_input},
        {OpCode::Id::END},
    });

    REQUIRE(std::isnan(shader.Run(NAN).x));
    REQUIRE(std::isnan(shader.Run(-1.f).x));
    REQUIRE(std::isinf(shader.Run(0.f).x));
    REQUIRE(shader.Run(4.f).x == Catch::Approx(2.f));
    REQUIRE(shader.Run(64.f).x == Catch::Approx(6.f));
    REQUIRE(shader.Run(1.e24f).x == Catch::Approx(79.7262742773f));
}

TEST_CASE("EX2", "[video_core][shader][shader_jit]") {
    const auto sh_input = SourceRegister::MakeInput(0);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::EX2, sh_output, sh_input},
        {OpCode::Id::END},
    });

    REQUIRE(std::isnan(shader.Run(NAN).x));
    REQUIRE(shader.Run(-800.f).x == Catch::Approx(0.f));
    REQUIRE(shader.Run(0.f).x == Catch::Approx(1.f));
    REQUIRE(shader.Run(2.f).x == Catch::Approx(4.f));
    REQUIRE(shader.Run(6.f).x == Catch::Approx(64.f));
    REQUIRE(shader.Run(79.7262742773f).x == Catch::Approx(1.e24f));
    REQUIRE(std::isinf(shader.Run(800.f).x));
}

TEST_CASE("MUL", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::MUL, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({+1.0f, -1.0f}).x == -1.0f);
    REQUIRE(shader.Run({-1.0f, +1.0f}).x == -1.0f);

    REQUIRE(shader.Run({INFINITY, 0.0f}).x == 0.0f);
    REQUIRE(std::isnan(shader.Run({NAN, 0.0f}).x));
    REQUIRE(shader.Run({+INFINITY, +INFINITY}).x == INFINITY);
    REQUIRE(shader.Run({+INFINITY, -INFINITY}).x == -INFINITY);
}

TEST_CASE("SGE", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::SGE, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({INFINITY, 0.0f}).x == 1.0f);
    REQUIRE(shader.Run({0.0f, INFINITY}).x == 0.0f);
    REQUIRE(shader.Run({NAN, 0.0f}).x == 0.0f);
    REQUIRE(shader.Run({0.0f, NAN}).x == 0.0f);
    REQUIRE(shader.Run({+INFINITY, +INFINITY}).x == 1.0f);
    REQUIRE(shader.Run({+INFINITY, -INFINITY}).x == 1.0f);
    REQUIRE(shader.Run({-INFINITY, +INFINITY}).x == 0.0f);
    REQUIRE(shader.Run({+1.0f, -1.0f}).x == 1.0f);
    REQUIRE(shader.Run({-1.0f, +1.0f}).x == 0.0f);
}

TEST_CASE("SLT", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::SLT, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({INFINITY, 0.0f}).x == 0.0f);
    REQUIRE(shader.Run({0.0f, INFINITY}).x == 1.0f);
    REQUIRE(shader.Run({NAN, 0.0f}).x == 0.0f);
    REQUIRE(shader.Run({0.0f, NAN}).x == 0.0f);
    REQUIRE(shader.Run({+INFINITY, +INFINITY}).x == 0.0f);
    REQUIRE(shader.Run({+INFINITY, -INFINITY}).x == 0.0f);
    REQUIRE(shader.Run({-INFINITY, +INFINITY}).x == 1.0f);
    REQUIRE(shader.Run({+1.0f, -1.0f}).x == 0.0f);
    REQUIRE(shader.Run({-1.0f, +1.0f}).x == 1.0f);
}

TEST_CASE("FLR", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::FLR, sh_output, sh_input1},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({0.5}).x == 0.0f);
    REQUIRE(shader.Run({-0.5}).x == -1.0f);
    REQUIRE(shader.Run({1.5}).x == 1.0f);
    REQUIRE(shader.Run({-1.5}).x == -2.0f);
    REQUIRE(std::isnan(shader.Run({NAN}).x));
    REQUIRE(std::isinf(shader.Run({INFINITY}).x));
}

TEST_CASE("MAX", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::MAX, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({1.0f, 0.0f}).x == 1.0f);
    REQUIRE(shader.Run({0.0f, 1.0f}).x == 1.0f);
    REQUIRE(shader.Run({0.0f, +INFINITY}).x == +INFINITY);
    // REQUIRE(shader.Run({0.0f, -INFINITY}).x == -INFINITY); // TODO: 3dbrew says this is -INFINITY
    REQUIRE(std::isnan(shader.Run({0.0f, NAN}).x));
    REQUIRE(shader.Run({NAN, 0.0f}).x == 0.0f);
    REQUIRE(shader.Run({-INFINITY, +INFINITY}).x == +INFINITY);
}

TEST_CASE("MIN", "[video_core][shader][shader_jit]") {
    const auto sh_input1 = SourceRegister::MakeInput(0);
    const auto sh_input2 = SourceRegister::MakeInput(1);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::MIN, sh_output, sh_input1, sh_input2},
        {OpCode::Id::END},
    });

    REQUIRE(shader.Run({1.0f, 0.0f}).x == 0.0f);
    REQUIRE(shader.Run({0.0f, 1.0f}).x == 0.0f);
    REQUIRE(shader.Run({0.0f, +INFINITY}).x == 0.0f);
    REQUIRE(shader.Run({0.0f, -INFINITY}).x == -INFINITY);
    REQUIRE(std::isnan(shader.Run({0.0f, NAN}).x));
    REQUIRE(shader.Run({NAN, 0.0f}).x == 0.0f);
    REQUIRE(shader.Run({-INFINITY, +INFINITY}).x == -INFINITY);
}

TEST_CASE("RCP", "[video_core][shader][shader_jit]") {
    const auto sh_input = SourceRegister::MakeInput(0);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::RCP, sh_output, sh_input},
        {OpCode::Id::END},
    });

    // REQUIRE(shader.Run({-0.0f}).x == INFINITY); // Violates IEEE
    REQUIRE(shader.Run({0.0f}).x == INFINITY);
    REQUIRE(shader.Run({INFINITY}).x == 0.0f);
    REQUIRE(std::isnan(shader.Run({NAN}).x));

    REQUIRE(shader.Run({16.0f}).x == Catch::Approx(0.0625f).margin(0.001f));
    REQUIRE(shader.Run({8.0f}).x == Catch::Approx(0.125f).margin(0.001f));
    REQUIRE(shader.Run({4.0f}).x == Catch::Approx(0.25f).margin(0.001f));
    REQUIRE(shader.Run({2.0f}).x == Catch::Approx(0.5f).margin(0.001f));
    REQUIRE(shader.Run({1.0f}).x == Catch::Approx(1.0f).margin(0.001f));
    REQUIRE(shader.Run({0.5f}).x == Catch::Approx(2.0f).margin(0.001f));
    REQUIRE(shader.Run({0.25f}).x == Catch::Approx(4.0f).margin(0.001f));
    REQUIRE(shader.Run({0.125f}).x == Catch::Approx(8.0f).margin(0.002f));
    REQUIRE(shader.Run({0.0625f}).x == Catch::Approx(16.0f).margin(0.004f));
}

TEST_CASE("RSQ", "[video_core][shader][shader_jit]") {
    const auto sh_input = SourceRegister::MakeInput(0);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        {OpCode::Id::RSQ, sh_output, sh_input},
        {OpCode::Id::END},
    });

    // REQUIRE(shader.Run({-0.0f}).x == INFINITY); // Violates IEEE
    REQUIRE(std::isnan(shader.Run({-2.0f}).x));
    REQUIRE(shader.Run({INFINITY}).x == 0.0f);
    REQUIRE(std::isnan(shader.Run({-INFINITY}).x));
    REQUIRE(std::isnan(shader.Run({NAN}).x));

    REQUIRE(shader.Run({16.0f}).x == Catch::Approx(0.25f).margin(0.001f));
    REQUIRE(shader.Run({8.0f}).x == Catch::Approx(1.0f / std::sqrt(8.0f)).margin(0.001f));
    REQUIRE(shader.Run({4.0f}).x == Catch::Approx(0.5f).margin(0.001f));
    REQUIRE(shader.Run({2.0f}).x == Catch::Approx(1.0f / std::sqrt(2.0f)).margin(0.001f));
    REQUIRE(shader.Run({1.0f}).x == Catch::Approx(1.0f).margin(0.001f));
    REQUIRE(shader.Run({0.5f}).x == Catch::Approx(1.0f / std::sqrt(0.5f)).margin(0.001f));
    REQUIRE(shader.Run({0.25f}).x == Catch::Approx(2.0f).margin(0.001f));
    REQUIRE(shader.Run({0.125f}).x == Catch::Approx(1.0 / std::sqrt(0.125)).margin(0.002f));
    REQUIRE(shader.Run({0.0625f}).x == Catch::Approx(4.0f).margin(0.004f));
}

TEST_CASE("Address Register Offset", "[video_core][shader][shader_jit]") {
    const auto sh_input = SourceRegister::MakeInput(0);
    const auto sh_c40 = SourceRegister::MakeFloat(40);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader = ShaderTest({
        // mova a0.x, sh_input.x
        {OpCode::Id::MOVA, DestRegister{}, "x", sh_input, "x", SourceRegister{}, "",
         nihstro::InlineAsm::RelativeAddress::A1},
        // mov sh_output.xyzw, c40[a0.x].xyzw
        {OpCode::Id::MOV, sh_output, "xyzw", sh_c40, "xyzw", SourceRegister{}, "",
         nihstro::InlineAsm::RelativeAddress::A1},
        {OpCode::Id::END},
    });

    // Prepare shader uniforms
    const bool inverted = true;
    std::array<Common::Vec4f, 96> f_uniforms;
    for (u32 i = 0; i < 0x80; i++) {
        if (i >= 0x00 && i < 0x60) {
            const u32 base = inverted ? (0x60 - i) : i;
            const auto color = (base * 2.f) / 255.0f;
            const auto color_f24 = Pica::f24::FromFloat32(color);
            shader.shader_setup->uniforms.f[i] = {color_f24, color_f24, color_f24,
                                                  Pica::f24::One()};
            f_uniforms[i] = {color, color, color, 1.f};
        } else if (i >= 0x60 && i < 0x70) {
            const u8 color = static_cast<u8>((i - 0x60) * 0x10);
            shader.shader_setup->uniforms.i[i - 0x60] = {color, color, color, 255};
        } else if (i >= 0x70 && i < 0x80) {
            shader.shader_setup->uniforms.b[i - 0x70] = i >= 0x78;
        }
    }

    REQUIRE(shader.Run(0.f) == f_uniforms[40]);
    REQUIRE(shader.Run(13.f) == f_uniforms[53]);
    REQUIRE(shader.Run(50.f) == f_uniforms[90]);
    REQUIRE(shader.Run(60.f) == vec4_one);
    REQUIRE(shader.Run(74.f) == vec4_one);
    REQUIRE(shader.Run(87.f) == vec4_one);
    REQUIRE(shader.Run(88.f) == f_uniforms[0]);
    REQUIRE(shader.Run(128.f) == f_uniforms[40]);
    REQUIRE(shader.Run(-40.f) == f_uniforms[0]);
    REQUIRE(shader.Run(-42.f) == vec4_one);
    REQUIRE(shader.Run(-70.f) == vec4_one);
    REQUIRE(shader.Run(-73.f) == f_uniforms[95]);
    REQUIRE(shader.Run(-127.f) == f_uniforms[41]);
    REQUIRE(shader.Run(-129.f) == f_uniforms[40]);
}

// TODO: Requires fix from https://github.com/neobrain/nihstro/issues/68
// TEST_CASE("MAD", "[video_core][shader][shader_jit]") {
//     const auto sh_input1 = SourceRegister::MakeInput(0);
//     const auto sh_input2 = SourceRegister::MakeInput(1);
//     const auto sh_input3 = SourceRegister::MakeInput(2);
//     const auto sh_output = DestRegister::MakeOutput(0);

//     auto shader = ShaderTest({
//         {OpCode::Id::MAD, sh_output, sh_input1, sh_input2, sh_input3},
//         {OpCode::Id::END},
//     });

//     REQUIRE(shader.Run({vec4_inf, vec4_zero, vec4_zero}).x == 0.0f);
//     REQUIRE(std::isnan(shader.Run({vec4_nan, vec4_zero, vec4_zero}).x));

//     REQUIRE(shader.Run({vec4_one, vec4_one, vec4_one}).x == 2.0f);
// }

TEST_CASE("Nested Loop", "[video_core][shader][shader_jit]") {
    const auto sh_input = SourceRegister::MakeInput(0);
    const auto sh_temp = SourceRegister::MakeTemporary(0);
    const auto sh_output = DestRegister::MakeOutput(0);

    auto shader_test = ShaderTest({
        // clang-format off
        {OpCode::Id::MOV, sh_temp, sh_input},
        {OpCode::Id::LOOP, 0},
            {OpCode::Id::LOOP, 1},
                {OpCode::Id::ADD, sh_temp, sh_temp, sh_input},
            {Type::EndLoop},
        {Type::EndLoop},
        {OpCode::Id::MOV, sh_output, sh_temp},
        {OpCode::Id::END},
        // clang-format on
    });

    {
        shader_test.shader_setup->uniforms.i[0] = {4, 0, 1, 0};
        shader_test.shader_setup->uniforms.i[1] = {4, 0, 1, 0};
        Common::Vec4<u8> loop_parms{shader_test.shader_setup->uniforms.i[0]};

        const int expected_aL = loop_parms[1] + ((loop_parms[0] + 1) * loop_parms[2]);
        const float input = 1.0f;
        const float expected_out = (((shader_test.shader_setup->uniforms.i[0][0] + 1) *
                                     (shader_test.shader_setup->uniforms.i[1][0] + 1)) *
                                    input) +
                                   input;

        Pica::Shader::UnitState shader_unit_jit;
        shader_test.RunJit(shader_unit_jit, input);

        REQUIRE(shader_unit_jit.address_registers[2] == expected_aL);
        REQUIRE(shader_unit_jit.registers.output[0].x.ToFloat32() == Catch::Approx(expected_out));
    }
    {
        shader_test.shader_setup->uniforms.i[0] = {9, 0, 2, 0};
        shader_test.shader_setup->uniforms.i[1] = {7, 0, 1, 0};

        const Common::Vec4<u8> loop_parms{shader_test.shader_setup->uniforms.i[0]};
        const int expected_aL = loop_parms[1] + ((loop_parms[0] + 1) * loop_parms[2]);
        const float input = 1.0f;
        const float expected_out = (((shader_test.shader_setup->uniforms.i[0][0] + 1) *
                                     (shader_test.shader_setup->uniforms.i[1][0] + 1)) *
                                    input) +
                                   input;
        Pica::Shader::UnitState shader_unit_jit;
        shader_test.RunJit(shader_unit_jit, input);

        REQUIRE(shader_unit_jit.address_registers[2] == expected_aL);
        REQUIRE(shader_unit_jit.registers.output[0].x.ToFloat32() == Catch::Approx(expected_out));
    }
}
 */

#endif
