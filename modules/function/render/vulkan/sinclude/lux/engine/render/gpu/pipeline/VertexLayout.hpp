#pragma once
#include <lux/engine/function/render/client/core/VertexLayoutTypes.hpp>  // VertexLayoutId
#include <cstdint>
#include <lux/engine/gapi/vk/vk.hpp>

namespace lux::render
{
    // VertexLayoutId is defined in core/VertexLayoutTypes.hpp
    // to allow lightweight consumers to avoid pulling in gapi/vk/vk.hpp.

    enum class EVertexSemantic : std::uint8_t
    {
        POSITION = 0,
        NORMAL = 1,
        TANGENT = 2,
        UV0 = 3,
        UV1 = 4,
        BITANGENT = 5,
        COLOR0 = 6,
        POINT_SIZE = 7,
        CUSTOM0 = 8,
        CUSTOM1 = 9,
        CUSTOM2 = 10,
        CUSTOM3 = 11,
        CUSTOM4 = 12,
        CUSTOM5 = 13,
        CUSTOM6 = 14,
        CUSTOM7 = 15,
        CUSTOM8 = 16,
        BONE_IDS = 17,      ///< per-vertex bone indices (skinning input)
        BONE_WEIGHTS = 18   ///< per-vertex bone weights (skinning input)
    };

	template<EVertexSemantic> struct vertex_layout_type;
    template<> struct vertex_layout_type<EVertexSemantic::POSITION>  { using type = float[3]; };
    template<> struct vertex_layout_type<EVertexSemantic::NORMAL>    { using type = float[3]; };
    template<> struct vertex_layout_type<EVertexSemantic::TANGENT>   { using type = float[3]; };
    template<> struct vertex_layout_type<EVertexSemantic::UV0>       { using type = float[2]; };
    template<> struct vertex_layout_type<EVertexSemantic::UV1>       { using type = float[2]; };
    template<> struct vertex_layout_type<EVertexSemantic::BITANGENT> { using type = float[3]; };
    template<> struct vertex_layout_type<EVertexSemantic::COLOR0>    { using type = float[4]; };
    template<> struct vertex_layout_type<EVertexSemantic::POINT_SIZE>{ using type = float; };

    struct VertexAttribute
    {
        EVertexSemantic     semantic;
        VkFormat            format;
        uint32_t            offset;
    };

    struct VertexLayout
    {
        uint32_t                     stride;
        std::vector<VertexAttribute> attributes;

        /// Stride expressed in floats (the bindless vertex pool is a flat float SSBO).
        [[nodiscard]] uint32_t floatStride() const { return stride / 4u; }
    };
}