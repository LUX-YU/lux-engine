#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace lux::scene
{
    namespace
    {
        using simulation::ecs::Entity;
        using simulation::ecs::Registry;
        using simulation::ecs::WorldTransform3D;

        class CameraExtraction final : public RenderSyncStage
        {
          public:
            explicit CameraExtraction(const RenderSyncStageCreateInfo &info)
                : registry_(info.registry), scene_(info.scene), page_size_(info.coordinate_page_size),
                  origin_(info.scene_origin_page),
                  operations_(info.catalog.ops<render::ViewCameraOperationIds>(info.catalog.nameOfType(info.feature))),
                  camera_added_(registry_.on_construct<Camera>().connect<&CameraExtraction::cameraChanged>(*this)),
                  camera_changed_(registry_.on_update<Camera>().connect<&CameraExtraction::cameraChanged>(*this)),
                  camera_removed_(registry_.on_destroy<Camera>().connect<&CameraExtraction::cameraChanged>(*this)),
                  transform_added_(
                      registry_.on_construct<WorldTransform3D>().connect<&CameraExtraction::poseChanged>(*this)),
                  transform_changed_(
                      registry_.on_update<WorldTransform3D>().connect<&CameraExtraction::poseChanged>(*this)),
                  transform_removed_(
                      registry_.on_destroy<WorldTransform3D>().connect<&CameraExtraction::poseChanged>(*this))
            {
            }

            bool hasPendingChanges() const noexcept override
            {
                return dirty_;
            }
            void requestFullSync() noexcept override
            {
                dirty_ = true;
            }

            ERenderSyncPrepareResult prepare(render::RenderProgramBuilder<> &builder) noexcept override
            {
                if (!dirty_)
                {
                    return ERenderSyncPrepareResult::NO_CHANGES;
                }

                prepared_.clear();
                updates_.clear();
                removals_.clear();
                prepared_revision_ = revision_;
                for (const Entity entity : registry_.view<const Camera, const WorldTransform3D>())
                {
                    const auto &camera = registry_.get<Camera>(entity);
                    if (camera.view.isNull())
                    {
                        continue;
                    }
                    if (std::ranges::find(prepared_, camera.view) != prepared_.end())
                    {
                        return ERenderSyncPrepareResult::FAILED;
                    }

                    const auto &world = registry_.get<WorldTransform3D>(entity).value;
                    const auto view = cameraView(world, world.translation());
                    const auto projection = cameraProjection(camera);
                    if (!view || !projection)
                    {
                        return ERenderSyncPrepareResult::FAILED;
                    }

                    render::ViewCameraUpdatePayload update{};
                    update.scene_id = scene_;
                    update.view = camera.view;
                    update.coordinate_page_size = static_cast<float>(page_size_);
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        const long double position = world.translation()[axis];
                        const long double page = std::floor(position / page_size_);
                        const long double delta = page - static_cast<long double>(origin_[axis]);
                        if (delta < std::numeric_limits<std::int32_t>::min() ||
                            delta > std::numeric_limits<std::int32_t>::max())
                        {
                            return ERenderSyncPrepareResult::FAILED;
                        }
                        update.render_origin.page_delta[axis] = static_cast<std::int32_t>(delta);
                        update.render_origin.local[axis] = static_cast<float>(position - page * page_size_);
                    }
                    for (std::size_t index = 0; index < 16; ++index)
                    {
                        update.view_matrix[index] = static_cast<float>(view->data()[index]);
                        update.proj_matrix[index] = static_cast<float>(projection->data()[index]);
                        if (!std::isfinite(update.view_matrix[index]) || !std::isfinite(update.proj_matrix[index]))
                        {
                            return ERenderSyncPrepareResult::FAILED;
                        }
                    }
                    prepared_.push_back(camera.view);
                    updates_.push_back(update);
                }

                for (const auto view : published_)
                {
                    if (std::ranges::find(prepared_, view) == prepared_.end())
                    {
                        removals_.push_back({scene_, view});
                    }
                }
                if (!removals_.empty())
                {
                    auto output = builder.appendBulk<render::ViewCameraRemovePayload>(
                        operations_.id<render::ViewCameraRemoveOp>(), removals_.size());
                    if (output.size() != removals_.size())
                    {
                        return ERenderSyncPrepareResult::FAILED;
                    }
                    std::ranges::copy(removals_, output.begin());
                }
                if (!updates_.empty())
                {
                    auto output = builder.appendBulk<render::ViewCameraUpdatePayload>(
                        operations_.id<render::ViewCameraUpdateOp>(), updates_.size());
                    if (output.size() != updates_.size())
                    {
                        return ERenderSyncPrepareResult::FAILED;
                    }
                    std::ranges::copy(updates_, output.begin());
                }
                if (!builder.valid())
                {
                    return ERenderSyncPrepareResult::FAILED;
                }
                return removals_.empty() && updates_.empty() ? ERenderSyncPrepareResult::PREPARED_NO_COMMANDS
                                                             : ERenderSyncPrepareResult::PREPARED_COMMANDS;
            }

            void commitPrepared() noexcept override
            {
                published_.swap(prepared_);
                dirty_ = revision_ != prepared_revision_;
            }

            void discardPrepared() noexcept override
            {
                prepared_.clear();
            }

          private:
            void cameraChanged(Registry &, Entity) noexcept
            {
                dirty_ = true;
                ++revision_;
            }

            void poseChanged(Registry &registry, Entity entity) noexcept
            {
                if (registry.all_of<Camera>(entity))
                {
                    cameraChanged(registry, entity);
                }
            }

            Registry &registry_;
            render::RenderSceneId scene_;
            double page_size_;
            std::array<std::int64_t, 3> origin_;
            render::ViewCameraOperationIds operations_;
            std::vector<render::ViewHandle> published_, prepared_;
            std::vector<render::ViewCameraUpdatePayload> updates_;
            std::vector<render::ViewCameraRemovePayload> removals_;
            std::uint64_t revision_{}, prepared_revision_{};
            bool dirty_{true};
            entt::scoped_connection camera_added_, camera_changed_, camera_removed_;
            entt::scoped_connection transform_added_, transform_changed_, transform_removed_;
        };

        lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageCreateFailure>
        createCameraExtraction(const RenderSyncStageCreateInfo &info) noexcept
        {
            if (info.scene.isNull() || !info.feature_handle.isValid() || !std::isfinite(info.coordinate_page_size) ||
                info.coordinate_page_size <= 0.0)
            {
                return lux::cxx::unexpected(
                    RenderSyncStageCreateFailure{ERenderSyncStageCreateError::INVALID_CONFIGURATION});
            }
            return std::unique_ptr<RenderSyncStage>{new CameraExtraction{info}};
        }
    } // namespace

    RenderFeatureSceneBinding cameraRenderFeatureBinding() noexcept
    {
        constexpr auto events = static_cast<std::uint8_t>(EComponentObservation::CONSTRUCT) |
                                static_cast<std::uint8_t>(EComponentObservation::UPDATE) |
                                static_cast<std::uint8_t>(EComponentObservation::DESTROY);
        static constexpr std::array observations{
            ComponentObservationSpec{lux::cxx::typeToken<Camera>(), events},
            ComponentObservationSpec{lux::cxx::typeToken<WorldTransform3D>(), events}};
        return {system::systemTypeId(RenderSystem::Description.canonical_name),
                render::featureId("lux.render.view_camera.v1"), observations, &createCameraExtraction};
    }
} // namespace lux::scene
