#pragma once
/**
 * @file GpuLayout.hpp
 * @brief Compile-time check that a C++ mirror struct is laid out the way the
 *        shader's copy of it is.
 *
 * The problem this closes: a GPU mirror struct exists twice — once in C++, once
 * in GLSL — and nothing checks that the two agree. `sizeof` assertions do not
 * close it. They pin one number on one side, so they miss:
 *
 *   - **field reordering** — swap two same-sized members and sizeof is unchanged
 *     while every shader read after the swap returns the wrong member;
 *   - **an alignment mismatch that happens to land on the same total** —
 *     AreaLightGPU used a 16-byte-aligned vec2 where std430 wants 8, which put
 *     every field from that point on at a different offset. Both sides were
 *     divisible by 16, so `static_assert(sizeof % 16 == 0)` passed on both;
 *   - **a truncated copy absorbed by array-stride rounding** — the ShadowSliceGPU
 *     copy that lost its last three fields still rounded to a 128-byte stride,
 *     so indexing kept working and nothing failed to compile.
 *
 * What this does instead: the author states each field's GLSL type once, and a
 * constexpr walk applies the std140/std430 rules to derive where each field must
 * sit. One `static_assert` per field compares that against `offsetof`, so a
 * mismatch is a compile error that names the field.
 *
 * Division of labour — nobody computes anything twice:
 *   - the **C++ compiler** produces the C++ offsets (`offsetof`), so this stays
 *     honest about whatever the real toolchain does with alignment attributes;
 *   - **glslang** produces the GLSL offsets, from the same field list;
 *   - this header only asserts the two derivations agree.
 *
 * The field list is written by hand today. It is also exactly what the comm-ops
 * generator would emit from `LUX_GPU_FIELD(glsl=...)` annotations, which is the
 * next increment — the hand-written form pins the target shape first (same
 * approach the hand-written PassParams prototype took).
 */

#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::render::gpulayout
{
    /// The GLSL types the engine's mirror structs actually use. Deliberately not
    /// the whole GLSL type system — an unlisted type is a compile error, which is
    /// the right answer for a type nobody has thought about the layout of.
    ///
    /// `Mat3` is absent on purpose: C++ gives it 36 bytes and both GLSL layouts
    /// give it 48 (three vec3 slots), which is the single easiest way to get this
    /// wrong. No mirror struct uses one.
    enum class EGlsl : std::uint8_t
    {
        Float,
        Int,
        Uint,
        Vec2,
        IVec2,
        UVec2,
        Vec3,
        IVec3,
        UVec3,
        Vec4,
        IVec4,
        UVec4,
        Mat4,
    };

    enum class EStd : std::uint8_t
    {
        Std140,
        Std430
    };

    struct Field
    {
        EGlsl type{EGlsl::Float};
        std::uint32_t count{1}; ///< array length; 1 = plain field
    };

    [[nodiscard]] constexpr std::uint32_t baseAlign(EGlsl t) noexcept
    {
        switch (t)
        {
        case EGlsl::Float:
        case EGlsl::Int:
        case EGlsl::Uint:
            return 4;
        case EGlsl::Vec2:
        case EGlsl::IVec2:
        case EGlsl::UVec2:
            return 8;
        default:
            return 16;
        }
    }

    [[nodiscard]] constexpr std::uint32_t baseSize(EGlsl t) noexcept
    {
        switch (t)
        {
        case EGlsl::Float:
        case EGlsl::Int:
        case EGlsl::Uint:
            return 4;
        case EGlsl::Vec2:
        case EGlsl::IVec2:
        case EGlsl::UVec2:
            return 8;
        case EGlsl::Vec3:
        case EGlsl::IVec3:
        case EGlsl::UVec3:
            return 12;
        case EGlsl::Vec4:
        case EGlsl::IVec4:
        case EGlsl::UVec4:
            return 16;
        case EGlsl::Mat4:
            return 64;
        }
        return 0;
    }

    [[nodiscard]] constexpr std::uint32_t roundUp(std::uint32_t v, std::uint32_t a) noexcept
    {
        return a == 0 ? v : ((v + a - 1) / a) * a;
    }

    /// Array stride. This is where std140 differs and where the 4x size surprises
    /// come from: std140 rounds every array element up to 16 bytes, so a
    /// `float w[4]` is 16 bytes in C++ and in std430, but **64** in std140.
    [[nodiscard]] constexpr std::uint32_t arrayStride(EGlsl t, EStd s) noexcept
    {
        const std::uint32_t a = baseAlign(t);
        const std::uint32_t stride = roundUp(baseSize(t), a);
        return s == EStd::Std140 ? roundUp(stride, 16u) : stride;
    }

    /// Offsets every field must sit at, derived from the field list alone.
    template <std::size_t N>
    [[nodiscard]] constexpr std::array<std::uint32_t, N> offsets(const std::array<Field, N>& fields, EStd s) noexcept
    {
        std::array<std::uint32_t, N> out{};
        std::uint32_t cursor = 0;
        for (std::size_t i = 0; i < N; ++i)
        {
            const auto& f = fields[i];
            std::uint32_t align = baseAlign(f.type);
            if (f.count > 1 && s == EStd::Std140)
                align = roundUp(align, 16u);
            cursor = roundUp(cursor, align);
            out[i] = cursor;
            cursor += (f.count > 1) ? arrayStride(f.type, s) * f.count : baseSize(f.type);
        }
        return out;
    }

    /// Total size the block occupies, including tail padding.
    template <std::size_t N>
    [[nodiscard]] constexpr std::uint32_t sizeOf(const std::array<Field, N>& fields, EStd s) noexcept
    {
        std::uint32_t cursor = 0;
        std::uint32_t maxAlign = 1;
        for (std::size_t i = 0; i < N; ++i)
        {
            const auto& f = fields[i];
            std::uint32_t align = baseAlign(f.type);
            if (f.count > 1 && s == EStd::Std140)
                align = roundUp(align, 16u);
            if (align > maxAlign)
                maxAlign = align;
            cursor = roundUp(cursor, align);
            cursor += (f.count > 1) ? arrayStride(f.type, s) * f.count : baseSize(f.type);
        }
        if (s == EStd::Std140)
            maxAlign = roundUp(maxAlign, 16u);
        return roundUp(cursor, maxAlign);
    }

} // namespace lux::render::gpulayout

// ---------------------------------------------------------------------------
//  Authoring
//
//  Write the field list once as an X-macro — GLSL type, C++ member name, and
//  (for arrays) the element count — then verify it against the C++ struct:
//
//      #define LUX_VIEW_GPU_FIELDS(X, PAD)  \
//          X(Mat4,  view,        1)         \
//          X(Vec3,  cam_pos,     1)         \
//          PAD(Uint, cam_pos_w,  1)         \
//          X(Float, splits,      8)
//
//  PAD takes a name too — it has to be unique within the list (it becomes an
//  enumerator), and naming the padding says what hole it fills.
//
//      LUX_GPU_VERIFY(ViewGpuData, Std430, LUX_VIEW_GPU_FIELDS)
//
//  The list describes the SHADER's block, field for field. `X` is a field that
//  also exists as a C++ member (its offset gets asserted); `PAD` is one that
//  only exists on the shader side — a GLSL `vec3 c; uint _pad;` pair is a single
//  16-byte member in C++, so the pad has no member to point at. Padding still
//  has to appear in the list because it moves everything after it.
//
//  Field indices are derived by the compiler (the list is expanded once into an
//  enum), so inserting a field in the middle needs no renumbering.
//
//  The emitted assertions are one per named field plus one for the total size.
//  The per-field form is what makes a failure readable: it names the member that
//  moved instead of reporting that some total no longer matches.
// ---------------------------------------------------------------------------

#define LUX_GPU_L_SPEC_(TYPE, NAME, COUNT)                                                                             \
    ::lux::render::gpulayout::Field{::lux::render::gpulayout::EGlsl::TYPE, COUNT},
#define LUX_GPU_L_SPEC_PAD_(TYPE, NAME, COUNT)                                                                         \
    ::lux::render::gpulayout::Field{::lux::render::gpulayout::EGlsl::TYPE, COUNT},

#define LUX_GPU_L_IDX_(TYPE, NAME, COUNT) idx_##NAME,
#define LUX_GPU_L_IDX_PAD_(TYPE, NAME, COUNT) idx_##NAME,

#define LUX_GPU_L_CHECK_(TYPE, NAME, COUNT)                                                                            \
    static_assert(                                                                                                     \
        offsetof(LUX_GPU_L_STRUCT_, NAME) == want[idx_##NAME],                                                         \
        "GPU mirror layout: member '" #NAME "' (declared to the shader as " #TYPE                                      \
        ") sits at a different byte offset than the shader's copy of this struct "                                     \
        "puts it. Either the C++ field order or alignment changed, or " #TYPE                                          \
        " is the wrong GLSL type for it. NOTE: the reported LINE is the "                                              \
        "LUX_GPU_VERIFY invocation — the member name in this message is what "                                         \
        "identifies the field.");
#define LUX_GPU_L_CHECK_PAD_(TYPE, NAME, COUNT) /* shader-only padding: no C++ member to assert against */

/// Verify that @p STRUCT is laid out as @p STD says a block with @p FIELDS is.
/// Place at namespace scope, after the struct definition.
#define LUX_GPU_VERIFY(STRUCT, STD, FIELDS)                                                                            \
    namespace gpu_layout_check_##STRUCT                                                                                \
    {                                                                                                                  \
        using LUX_GPU_L_STRUCT_ = STRUCT;                                                                              \
        enum : std::size_t                                                                                             \
        {                                                                                                              \
            FIELDS(LUX_GPU_L_IDX_, LUX_GPU_L_IDX_PAD_)                                                                 \
        };                                                                                                             \
        inline constexpr auto spec = std::array{FIELDS(LUX_GPU_L_SPEC_, LUX_GPU_L_SPEC_PAD_)};                         \
        inline constexpr auto want = ::lux::render::gpulayout::offsets(spec, ::lux::render::gpulayout::EStd::STD);     \
        FIELDS(LUX_GPU_L_CHECK_, LUX_GPU_L_CHECK_PAD_)                                                                 \
        static_assert(                                                                                                 \
            sizeof(STRUCT) == ::lux::render::gpulayout::sizeOf(spec, ::lux::render::gpulayout::EStd::STD),             \
            "GPU mirror layout: the C++ struct's total size differs from what the "                                    \
            "shader's block occupies. A truncated or extra field is the usual "                                        \
            "cause — note that a truncation smaller than the struct alignment can "                                    \
            "still produce a matching ARRAY STRIDE, which is how such a copy "                                         \
            "survives unnoticed until one more field is added.");                                                      \
    }
