#pragma once
#include <cassert>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/scene/render/RenderBindingDeclarations.hpp>
#include <vector>

namespace fixture
{
    using namespace lux;

    struct ViewObservation final
    {
        std::uint32_t value = 0;
    };

    // External author fixture: uses installed declaration/context contracts only, no builtin implementation.
    class ViewOperation final
    {
    public:
        using SourceComponents = std::tuple<ViewObservation>;
        using ResultComponents = std::tuple<>;
        static constexpr std::string_view name = "test.editor.external_view_operation";
        static constexpr auto scope = scene::ERenderOperationScope::VIEW;
        static constexpr auto policy = scene::ERenderUpdatePolicy::CHANGES;
        inline static std::vector<std::pair<render::ViewId, std::uint32_t>> observations;
        inline static std::size_t created = 0, destroyed = 0, drained = 0;

        static std::span<const render::RenderPayloadContract> inputs() noexcept
        {
            static const auto contracts = []
            {
                render::RenderPayloadContract input;
                input.schema = render::kCameraInput;
                input.use = render::ERenderPayloadUse::FRAME_INPUT;
                input.lifetime = render::ERenderPayloadLifetime::MOVED_VALUE;
                input.setCppType(cxx::typeToken<render::CameraInput>());
                return std::array{input};
            }();
            return contracts;
        }

        static render::RenderResult<std::unique_ptr<ViewOperation>> create(scene::RenderBindingContext& context
        ) noexcept
        {
            assert(!context.view.isNull() && context.view_entity != simulation::ecs::NullEntity);
            return std::unique_ptr<ViewOperation>{new ViewOperation(context)};
        }

        ~ViewOperation() noexcept
        {
            assert(stopped_ && !pending_);
            std::erase_if(observations, [&](const auto& value) noexcept { return value.first == view_; });
            ++destroyed;
        }

        template <class C>
        void sourceChanged(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity,
            scene::ERenderComponentChange change
        ) noexcept
        {
            if (entity == entity_ && change != scene::ERenderComponentChange::DESTROY)
            {
                auto found = std::ranges::find_if(
                    observations,
                    [&](const auto& value) noexcept { return value.first == view_; }
                );
                assert(found != observations.end());
                found->second = registry.get<ViewObservation>(entity).value;
            }
        }

        scene::SceneStageResult maintain(scene::SceneStageContext& context) noexcept
        {
            if (context.stop.stop_requested() && !stopped_)
            {
                stopped_ = pending_ = true;
                // Exercise accepted completion during local View removal, without a second Scope/queue.
                assert(execution_.spawn(
                    stdexec::schedule(execution_.mainScheduler()) | stdexec::then(
                                                                        [this]() noexcept
                                                                        {
                                                                            pending_ = false;
                                                                            ++drained;
                                                                        }
                                                                    )
                ));
            }
            return pending_ ? scene::ESceneProgress::PENDING : scene::ESceneProgress::COMPLETE;
        }

    private:
        explicit ViewOperation(scene::RenderBindingContext& context) noexcept
            : execution_(context.execution), view_(context.view), entity_(context.view_entity)
        {
            assert(std::ranges::none_of(observations, [&](const auto& value) noexcept { return value.first == view_; })
            );
            observations.emplace_back(view_, 0);
            ++created;
        }

        execution::ExecutionRuntime& execution_;
        render::ViewId view_;
        simulation::ecs::Entity entity_;
        bool stopped_ = false, pending_ = false;
    };

    struct StatelessObservation final
    {
        using SourceComponents = std::tuple<ViewObservation>;
        using ResultComponents = std::tuple<>;
        static constexpr std::string_view name = "test.editor.stateless_observation";
        inline static std::size_t signals = 0;

        static auto inputs() noexcept
        {
            return ViewOperation::inputs();
        }

        template <class C>
        void sourceChanged(simulation::ecs::Registry&, simulation::ecs::Entity, scene::ERenderComponentChange) noexcept
        {
            ++signals;
        }
    };

    static_assert(scene::StatelessRenderBinding<StatelessObservation>);

} // namespace fixture
