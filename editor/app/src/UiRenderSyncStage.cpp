#include <lux/engine/editor/detail/UiFrame.hpp>
#include <lux/engine/editor/detail/UiRenderSyncStage.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/scene/RenderSystem.hpp>

namespace lux::editor::detail
{
    namespace
    {
        class UiRenderSyncStage final : public lux::scene::RenderSyncStage
        {
        public:
            explicit UiRenderSyncStage(const lux::scene::RenderSyncStageCreateInfo& input)
                : registry_(input.registry), scene_(input.scene), feature_(input.feature_handle),
                  operations_(input.catalog.ops<render::UiRenderOperationIds>("UiRender"))
            {
                constructed_ = registry_.on_construct<UiFrame>().connect<&UiRenderSyncStage::changed>(*this);
                updated_ = registry_.on_update<UiFrame>().connect<&UiRenderSyncStage::changed>(*this);
                destroyed_ = registry_.on_destroy<UiFrame>().connect<&UiRenderSyncStage::removed>(*this);
                for (const auto entity : registry_.view<UiFrame>())
                {
                    changed(registry_, entity);
                }
            }

            bool hasPendingChanges() const noexcept override
            {
                return dirty_;
            }
            void requestFullSync() noexcept override
            {
                dirty_ = true;
            }
            lux::scene::ERenderSyncPrepareResult prepare(render::RenderProgramBuilder<>& builder) noexcept override
            {
                using enum lux::scene::ERenderSyncPrepareResult;
                if (!dirty_)
                {
                    return NO_CHANGES;
                }
                const auto* input = registry_.try_get<UiFrame>(entity_);
                const auto result = input && input->frame
                                        ? lux::ui::appendFrame(builder, operations_, scene_, feature_, input->frame)
                                        : lux::ui::appendClear(builder, operations_, scene_, feature_);
                if (!result)
                {
                    return FAILED;
                }
                return input && input->frame ? PREPARED_FRAME_COMMANDS : PREPARED_COMMANDS;
            }
            void commitPrepared() noexcept override
            {
                dirty_ = false;
            }
            void discardPrepared() noexcept override {}

        private:
            void changed(simulation::ecs::Registry&, simulation::ecs::Entity entity) noexcept
            {
                entity_ = entity;
                dirty_ = true;
            }
            void removed(simulation::ecs::Registry&, simulation::ecs::Entity) noexcept
            {
                entity_ = simulation::ecs::NullEntity;
                dirty_ = true;
            }
            simulation::ecs::Registry& registry_;
            render::RenderSceneId scene_;
            render::FeatureHandle feature_;
            render::UiRenderOperationIds operations_;
            simulation::ecs::Entity entity_{simulation::ecs::NullEntity};
            bool dirty_{true};
            entt::scoped_connection constructed_, updated_, destroyed_;
        };

        lux::cxx::expected<std::unique_ptr<lux::scene::RenderSyncStage>, lux::scene::RenderSyncStageCreateFailure>
        createUiStage(const lux::scene::RenderSyncStageCreateInfo& input) noexcept
        {
            return std::unique_ptr<lux::scene::RenderSyncStage>{new UiRenderSyncStage{input}};
        }
    } // namespace

    lux::scene::RenderFeatureSceneBinding uiRenderFeatureBinding() noexcept
    {
        return {
            system::systemTypeId(lux::scene::RenderSystem::Description.canonical_name),
            render::kUiRenderDescriptor.type,
            {},
            &createUiStage
        };
    }
} // namespace lux::editor::detail
