#pragma once

#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <lux/engine/render/resources/transform/Data.hpp>

namespace lux::render
{
    namespace TransformCommands
    {
        struct Create final
        {
            static constexpr std::string_view name = "lux.render.transforms.create";
            using Data = TransformMatrix;

            struct Request final
            {
                ResourceInstanceId resource;
                std::span<const TransformMatrix> matrices;
            };

            struct Response final
            {
                RenderError error{};
                std::vector<TransformId> transforms;
            };

            [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
            {
                return request.resource;
            }
        };

        struct Update final
        {
            static constexpr std::string_view name = "lux.render.transforms.update";
            using Data = TransformRecordUpdate;

            struct Request final
            {
                ResourceInstanceId resource;
                std::span<const TransformRecordUpdate> values;
            };

            using Response = AckResponse;

            [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
            {
                return request.resource;
            }
        };

        struct Release final
        {
            static constexpr std::string_view name = "lux.render.transforms.release";
            using Data = TransformId;
            using Request = TransformId;
            using Response = AckResponse;

            [[nodiscard]] static TransformId resourceId(const Request& request) noexcept { return request; }
        };
    } // namespace TransformCommands

    struct TransformResource final
    {
        using Commands = std::tuple<TransformCommands::Create, TransformCommands::Update, TransformCommands::Release>;
        using PerFrameData = std::tuple<TransformRecordUpdate>;
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            kTransformResourceName,
            0,
            0,
            cxx::make_basic_type_info<TransformBinding>(),
            "Scene transform records shared by Mesh instances.",
            EResourceDomain::SCENE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
