#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/transport/Operation.hpp>

namespace lux::render
{
    struct LUX_OP(lane = reply, name = "lux.render.rd3.reply.v1") RuntimeReply
    {
        std::uint64_t frame_serial{};
        std::uint32_t byte_count{};
        std::array<std::byte, 224> bytes{};
        std::uint32_t index{}, generation{}, scene_index{}, scene_generation{};
    };

    struct LUX_OP(lane = program, kind = blob, name = "lux.render.rd3.view_pass.v1") RuntimeViewPublication
    {
        std::uint64_t owner{}, revision{};
        std::uint32_t scene_index{}, scene_generation{}, index{}, generation{}, pass{};
        LUX_OP_BLOB() BlobRef scalars;
    };

    enum class ERuntimeControl : std::uint32_t
    {
        CREATE_SCENE,
        CREATE_TARGET,
        CREATE_VIEW,
        REMOVE_SCENE,
        REMOVE_TARGET,
        REMOVE_VIEW,
        REMOVE_EFFECT,
        QUERY_GRAPH,
        SET_VIEW_ACTIVE,
        READBACK,
        READBACK_TARGET
    };

    struct LUX_OP(
        lane = control,
        kind = resource,
        reply = "lux::render::RuntimeReply",
        name = "lux.render.rd3.control.v1"
    ) RuntimeControl
    {
        std::uint64_t owner{}, offset{};
        std::uint32_t index{}, generation{}, scene_index{}, scene_generation{};
        std::uint32_t effect_index{}, effect_generation{}, target_index{}, target_generation{};
        std::uint32_t first{}, second{}, bytes{};
        ERuntimeControl operation{};
    };
} // namespace lux::render
