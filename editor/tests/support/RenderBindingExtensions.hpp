#pragma once
#include <cassert>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>
#include <lux/engine/systems/render/RenderAssets.hpp>
#include <lux/engine/systems/render/RenderBindingDeclarations.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/RenderViewComponent.hpp>
#include <vector>

namespace fixture
{
    using namespace lux;

    struct AssetTableProbe
    {
        using Value = unsigned;
        static constexpr std::string_view name = "test.editor.table";
        inline static unsigned destroyed = 0;
        std::unique_ptr<unsigned> dependency;

        explicit AssetTableProbe(unsigned value) : dependency(std::make_unique<unsigned>(value)) {}

        AssetTableProbe(AssetTableProbe&&) noexcept = default;

        ~AssetTableProbe() noexcept
        {
            destroyed += bool(dependency);
        }

        bool compatible(const AssetTableProbe& other) const noexcept
        {
            return *dependency == *other.dependency;
        }

        exec::task<lux::render::RenderResult<unsigned>> load(::lux::engine::render::RenderAssetLoad) noexcept
        {
            co_return *dependency;
        }

        exec::task<lux::render::RenderResult<void>> release(unsigned&) noexcept
        {
            co_return lux::render::RenderResult<void>{};
        }
    };

    struct ViewObservation final
    {
        std::uint32_t value = 0;
    };

    // Native View IDs are Scene-local. Multi-Scene/Play assertions use the full identity.
    struct ViewIdentity final
    {
        render::SceneId scene;
        render::ViewId view;
        bool operator==(const ViewIdentity&) const noexcept = default;

        bool isNull() const noexcept
        {
            return view.isNull();
        }
    };

    class ViewDependency final
    {
    public:
        inline static std::vector<ViewIdentity> alive;

        explicit ViewDependency(ViewIdentity view) noexcept : view_(view)
        {
            alive.push_back(view);
        }

        ~ViewDependency() noexcept
        {
            std::erase(alive, view_);
        }

        bool valid() const noexcept
        {
            return std::ranges::find(alive, view_) != alive.end();
        }

    private:
        ViewIdentity view_;
    };

    class ViewOperation final
    {
    public:
        inline static std::vector<std::pair<ViewIdentity, std::uint32_t>> observations;
        inline static std::size_t created = 0, destroyed = 0, drained = 0;
        inline static ViewIdentity fail_retirement;

        ViewOperation(
            execution::ExecutionRuntime& execution,
            ViewDependency& dependency,
            ViewIdentity view
        ) noexcept
            : execution_(execution), dependency_(dependency), view_(view)
        {
            observations.emplace_back(view, 0);
            ++created;
        }

        ~ViewOperation() noexcept
        {
            assert(stopped_ && dependency_.valid());
            std::erase_if(observations, [&](const auto& value) noexcept { return value.first == view_; });
            ++destroyed;
        }

        void changed(ecs::Registry& registry, ecs::Entity entity) noexcept
        {
            const auto* binding = registry.try_get<::lux::engine::render::detail::RenderViewComponent>(entity);
            if (binding && binding->view == view_.view)
            {
                auto item = std::ranges::find(observations, view_, &decltype(observations)::value_type::first);
                assert(item != observations.end());
                item->second = registry.get<ViewObservation>(entity).value;
            }
        }

        exec::task<scene::SceneCloseResult> close() noexcept
        {
            assert(dependency_.valid());
            if (fail_retirement == view_)
            {
                fail_retirement = {};
                co_return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            co_await stdexec::schedule(execution_.mainScheduler());
            stopped_ = true;
            ++drained;
            co_return scene::SceneCloseResult{};
        }

    private:
        execution::ExecutionRuntime& execution_;
        ViewDependency& dependency_;
        ViewIdentity view_;
        bool stopped_ = false;
    };

    struct StatelessObservation final
    {
        inline static std::size_t signals = 0;

        void changed(ecs::Registry&, ecs::Entity) noexcept
        {
            ++signals;
        }
    };

    template <class T> struct ObserveView final
    {
        using Read = std::tuple<ViewObservation>;
        using Watch = Read;
        using Output = void;
        static constexpr auto created = &T::changed;
        static constexpr auto updated = &T::changed;
        static constexpr auto removed = nullptr;
        static constexpr auto output_removed = nullptr;
    };

    // Test-owned composition wraps the selected capability recipe, not a second Scene feature.
    struct ViewBindings final
    {
        ::lux::engine::render::RenderBindingDeclaration builtin;
        execution::ExecutionRuntime& execution;

        static render::RenderResult<void> install(
            ::lux::engine::render::RenderSystem& system,
            const render::CommandDestination& inputs,
            void* data
        ) noexcept
        {
            auto& self = *static_cast<ViewBindings*>(data);
            auto installed = self.builtin.install(system, inputs);
            if (!installed)
            {
                return installed;
            }
            const auto* view = std::get_if<render::ViewRequest>(&inputs.scope);
            if (!view)
            {
                return {};
            }
            auto& dependency = system.emplaceBinding<ViewDependency>(ViewIdentity{view->scene, view->view});
            auto& operation = system.emplaceBinding<ViewOperation>(
                self.execution,
                dependency,
                ViewIdentity{view->scene, view->view}
            );
            system.addReaction<ObserveView<ViewOperation>>(operation);
            for (unsigned i = 0; i < 2; ++i)
            {
                auto& observer = system.emplaceBinding<StatelessObservation>();
                system.addReaction<ObserveView<StatelessObservation>>(observer);
            }
            scene::SceneCloseParticipant close;
            close.key = "test.view-operation";
            close.object = &operation;
            close.close = +[](void* object, scene::SceneInstanceId) noexcept
            { return static_cast<ViewOperation*>(object)->close(); };
            system.addCloseParticipant(std::move(close));
            return {};
        }
    };
} // namespace fixture
