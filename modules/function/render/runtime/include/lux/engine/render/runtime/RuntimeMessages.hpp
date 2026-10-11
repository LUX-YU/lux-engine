#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/transport/Operation.hpp>

namespace lux::render
{
    // Private transport representation of the F5 fixed authoring consumer.
    // Public methods accept typed RuntimeAddress/GraphPassId, then copy at the
    // actual thread boundary. This is not a DataSchema or a Feature wire SDK.
    struct LUX_OP(lane = reply, name = "lux.render.runtime.reply.v1") RuntimeReply
    {
        std::uint64_t frame_serial{};
        std::uint32_t byte_count{};
        std::array<std::byte, 224> bytes{};
    };

    struct LUX_OP(lane = program, kind = blob, name = "lux.render.runtime.scene_pass.v1") RuntimeScenePublication
    {
        std::uint64_t owner{}, revision{};
        std::uint32_t index{}, generation{}, path{}, pass{};
        LUX_OP_BLOB() BlobRef scalars;
    };

    struct LUX_OP(lane = program, kind = blob, name = "lux.render.runtime.view_pass.v1") RuntimeViewPublication
    {
        std::uint64_t owner{}, revision{};
        std::uint32_t index{}, generation{}, pass{};
        std::array<float, 16> camera{};
        LUX_OP_BLOB() BlobRef scalars;
    };

    enum class ERuntimeControl : std::uint32_t
    {
        RESIZE_TARGET,
        SET_VIEW_ACTIVE,
        REMOVE_VIEW,
        REMOVE_SCENE,
        REMOVE_TARGET,
        SET_VIEW_PATH
    };

    struct LUX_OP(
        lane = control,
        kind = resource,
        reply = "lux::render::RuntimeReply",
        name = "lux.render.runtime.control.v1"
    ) RuntimeControl
    {
        std::uint64_t owner{};
        std::uint32_t index{}, generation{}, first{}, second{};
        ERuntimeControl operation{};
    };

    struct LUX_OP(
        lane = control,
        kind = resource,
        reply = "lux::render::RuntimeReply",
        name = "lux.render.runtime.readback.v1"
    ) RuntimeReadback
    {
        std::uint64_t owner{}, offset{};
        std::uint32_t index{}, generation{}, pass{}, input{}, bytes{};
        bool target_output{};
    };
} // namespace lux::render
