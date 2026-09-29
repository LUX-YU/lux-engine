#pragma once
#include <lux/engine/editor/scene/ViewportPresentation.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.ops.hpp>

namespace lux::editor::scene
{
    struct HighlightRenderer final
    {
        struct Key final
        {
            lux::scene::SceneInstanceId instance;
            render::ViewHandle view;
            render::FeatureHandle highlight, grid;
            std::uint64_t selection{}, structure{};
            float plane{};
            friend bool operator==(const Key&, const Key&) = default;
        };
        std::optional<Key> desired, prepared, accepted, rejected;
        render::TRenderProgram<> program;
        render::RendererFailure failure;

        render::RenderResult<EOverlaySubmit> update(
            lux::scene::SceneRuntime& runtime,
            lux::scene::SceneInstanceId instance,
            lux::system::SystemInstanceId system,
            lux::scene::RenderResources& resources,
            render::RenderRuntime& renderer,
            lux::scene::RenderResourceId view,
            OverlayConfiguration config
        )
        {
            auto registry = std::as_const(runtime).borrowInstance(instance);
            if (!registry)
                return EOverlaySubmit::NOT_READY;
            auto* scene = lux::scene::RenderSceneState::find(registry->get(), system);
            const auto observation = resources.observeView(view);
            if (!scene || !observation || !observation->handle.isValid())
                return EOverlaySubmit::NOT_READY;
            const auto receipt = resources.sceneReceipt(scene->resource).status();
            if (receipt.state != lux::scene::ESceneResourceState::READY)
                return EOverlaySubmit::NOT_READY;
            const auto highlight = resources.sceneFeature(scene->resource, render::kHighlightDescriptor.type);
            const auto grid = resources.sceneFeature(scene->resource, render::kGrid3DDescriptor.type);
            const Key key{
                instance,
                observation->handle,
                highlight,
                grid,
                config.selection_version,
                config.structure_version,
                config.plane_height
            };
            auto ready = prepare(
                key,
                [&] { return resources.capture(std::array{scene->resource, view}); },
                [&](render::RenderProgramSession::Builder& builder) {
                    if (highlight.isValid())
                    {
                        std::vector<render::ERenderEntityId> targets;
                        const auto& world = registry->get();
                        if (world.valid(config.selection))
                            for (const auto mesh : world.view<simulation::ecs::Mesh3D>())
                            {
                                auto current = mesh;
                                for (std::size_t depth{}; current != simulation::ecs::NullEntity &&
                                                          depth <= world.storage<simulation::ecs::Entity>()->size();
                                     ++depth)
                                {
                                    if (current == config.selection)
                                    {
                                        targets.push_back(static_cast<render::ERenderEntityId>(entt::to_integral(mesh))
                                        );
                                        break;
                                    }
                                    const auto* parent = world.try_get<simulation::ecs::Parent>(current);
                                    current = parent ? parent->entity : simulation::ecs::NullEntity;
                                    if (current != simulation::ecs::NullEntity && !world.valid(current))
                                        break;
                                }
                            }
                        const auto ids = renderer.features().ops<render::HighlightOperationIds>(
                            renderer.features().nameOfType(render::kHighlightDescriptor.type)
                        );
                        render::HighlightReplaceTargetsPayload payload{receipt.scene, highlight, key.view};
                        payload.targets =
                            builder.pushBlob(std::as_bytes(std::span(targets)), alignof(render::ERenderEntityId));
                        builder.push(
                            render::opcode_of_v<render::HighlightReplaceTargetsOp>,
                            ids.id<render::HighlightReplaceTargetsOp>(),
                            payload
                        );
                    }
                    if (grid.isValid())
                    {
                        const auto ids = renderer.features().ops<render::Grid3DOperationIds>(
                            renderer.features().nameOfType(render::kGrid3DDescriptor.type)
                        );
                        render::Grid3DSetParamsPayload payload{receipt.scene, grid, key.view};
                        payload.planeY = key.plane;
                        builder.push(
                            render::opcode_of_v<render::Grid3DSetParamsOp>,
                            ids.id<render::Grid3DSetParamsOp>(),
                            payload
                        );
                    }
                }
            );
            if (!ready)
                return lux::cxx::unexpected(ready.error());
            if (!*ready)
                return accepted == desired ? EOverlaySubmit::UNCHANGED : EOverlaySubmit::NOT_READY;
            return submit([&](auto& input) { return renderer.submit(input); });
        }
        // Shared by the actual viewport path and deterministic transport-fault tests.
        // Templated adapters are synchronous; no callbacks or registry borrows escape preparation.
        template <class Capture, class Encode>
        render::RenderResult<bool> prepare(Key key, Capture&& capture, Encode&& encode)
        {
            desired = key;
            if (accepted == desired)
                return false;
            if (rejected == desired)
                return lux::cxx::unexpected(failure);
            if (prepared == desired)
                return true;
            program.clear_keep_capacity();
            prepared.reset();
            auto captured = capture();
            if (!captured)
                return lux::cxx::unexpected(captured.error());
            render::RenderProgramSession::Builder builder(program);
            encode(builder);
            builder.emplaceAttachment<render::RenderSubmissionState>(
                render::attachment_types::SubmissionState,
                std::move(*captured)
            );
            prepared = desired;
            return true;
        }
        template <class Submit> render::RenderResult<EOverlaySubmit> submit(Submit&& send)
        {
            auto result = send(program);

            if (!result)
            {
                failure = result.error();
                rejected = prepared;
                prepared.reset();
                program.clear_keep_capacity();
                return lux::cxx::unexpected(failure);
            }
            if (*result == render::EFrameSubmit::BACKPRESSURED)
                return EOverlaySubmit::BACKPRESSURED;
            accepted = prepared;
            prepared.reset();
            return EOverlaySubmit::ACCEPTED;
        }
    };
}
