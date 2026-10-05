#include <array>
#include <imgui.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/detail/ViewportStateCodec.hpp>
#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/editor/scene/SceneCreationPoint.hpp>
#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/views/ViewportElement.hpp>
#include <lux/engine/editor/workbench/UiFailure.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/WorldResidency.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::scene
{
    namespace
    {
        constexpr sessions::SessionKindIdView kContentKinds[]{sessions::SessionKindIdView{"lux.editor.scene"}};
        template <class T> auto rejected(T error)
        {
            return cxx::unexpected(SceneViewFailure{std::move(error)});
        }
        template <class T> SceneViewResult<void> adopted(T result)
        {
            if (!result)
            {
                return rejected(result.error());
            }
            return {};
        }
        desktop::UiResult<std::optional<SceneViewState>> decodeState(
            std::uint32_t schema,
            std::span<const std::byte> bytes
        )
        {
            if (schema != 1)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::INVALID_CONFIGURATION,
                    "scene.view.state",
                    schema,
                    "Unknown schema"
                });
            }
            if (bytes.empty())
            {
                return std::optional<SceneViewState>{};
            }
            serialization::BinaryReader reader(bytes);
            SceneViewState candidate;
            const bool is_valid_viewport = views::detail::readViewportState(reader, candidate.camera, candidate.extent);
            const auto plane = reader.readFloat<float>();
            const bool is_invalid_plane = !plane || !std::isfinite(*plane);
            const bool is_invalid_state = !is_valid_viewport || is_invalid_plane || reader.remaining() != 0;
            if (is_invalid_state)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::INVALID_CONFIGURATION,
                    "scene.view.state",
                    schema,
                    "Invalid camera state"
                });
            }
            candidate.work_plane_height = *plane;
            return std::optional{candidate};
        }
        desktop::UiFailure uiFailure(const SceneViewFailure& error)
        {
            return workbench::detail::uiFailure(error, workbench::detail::isRetryableUiFailure(error));
        }
        constexpr services::ServiceDependency ui_dependencies[]{
            {services::ServiceNameView{"lux.editor.sessions"},
             1,
             cxx::typeToken<sessions::SessionStore>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.scene.runtime"},
             1,
             cxx::typeToken<lux::scene::SceneRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.scene.projections"}, 1, cxx::typeToken<ScenePresentationHub>()},
            {services::ServiceNameView{"lux.editor.scene.projection.environment"},
             1,
             cxx::typeToken<ProjectionEnvironment>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.scene.runs"},
             1,
             cxx::typeToken<RunStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true},
            {services::ServiceNameView{"lux.editor.scene.model-drop"},
             1,
             cxx::typeToken<SceneView::ModelDrop>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true}
        };
        SceneInteractionGroup* interaction(const VSceneViewBinding& binding) noexcept
        {
            return std::visit(
                [](const auto& value) -> SceneInteractionGroup*
                {
                    if constexpr (requires { value.interaction; })
                    {
                        return value.interaction;
                    }
                    else
                    {
                        return nullptr;
                    }
                },
                binding
            );
        }
    } // namespace
    desktop::UiResult<std::unique_ptr<lux::ui::Pane>> SceneView::createConfigured(
        services::ServiceResolver& resolver,
        const desktop::UiCreateInfo& input
    )
    {
        auto receiver = resolver.require<ModelDrop>(5);
        const bool has_receiver_failure = !receiver && receiver.error().code != services::EServiceError::NOT_FOUND;
        if (has_receiver_failure)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "scene.model-drop",
                static_cast<std::uint64_t>(receiver.error().code),
                receiver.error().detail
            });
        }
        const bool is_empty_receiver = receiver && !receiver->get();
        if (is_empty_receiver)
        {
            return cxx::unexpected(desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.model-drop"});
        }
        auto state = decodeState(input.configuration.schema, input.configuration.bytes);
        if (!state)
        {
            return cxx::unexpected(std::move(state.error()));
        }
        const auto rejectedDependency = [](const services::ServiceFailure& error)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "services",
                static_cast<std::uint64_t>(error.code),
                error.detail
            });
        };
        auto store = resolver.require<sessions::SessionStore>(0);
        if (!store)
        {
            return rejectedDependency(store.error());
        }
        auto runtime = resolver.require<lux::scene::SceneRuntime>(1);
        if (!runtime)
        {
            return rejectedDependency(runtime.error());
        }
        auto projections = resolver.get<ScenePresentationHub>(2);
        if (!projections)
        {
            return rejectedDependency(projections.error());
        }
        auto environment = resolver.get<ProjectionEnvironment>(3);
        if (!environment)
        {
            return rejectedDependency(environment.error());
        }
        const bool is_invalid_environment = !(*environment)->resources || !(*environment)->renderer;
        if (is_invalid_environment)
        {
            return cxx::unexpected(uiFailure(SceneViewFailure{desktop::EUiError::ATTACHMENT}));
        }
        auto runs = resolver.get<RunStore>(4);
        if (!runs && runs.error().code != services::EServiceError::NOT_FOUND)
        {
            return rejectedDependency(runs.error());
        }
        std::optional<RunInspectAccess> inspection;
        if (runs)
        {
            auto retained = RunInspectAccess::create(*runs);
            if (!retained)
            {
                return cxx::unexpected(uiFailure(SceneViewFailure{retained.error()}));
            }
            inspection.emplace(std::move(*retained));
        }
        SceneViewCreateInfo info;
        info.id = input.instance;
        info.state.camera.transform.translation = {0, 3, 8};
        if (*state)
        {
            info.state = **state;
        }
        auto view = SceneView::create(
            input.dispatcher,
            {store->get().access<SceneSession>(),
             std::move(*projections),
             runtime->get(),
             *(*environment)->resources,
             *(*environment)->renderer,
             std::move(*environment),
             std::move(inspection)},
            std::move(info)
        );
        if (!view)
        {
            return cxx::unexpected(uiFailure(view.error()));
        }
        auto bound = (*view)->rebindContent(input.content);
        if (!bound)
        {
            return cxx::unexpected(uiFailure(bound.error()));
        }
        if (receiver)
        {
            auto connection = object::LuxObject::connect(
                view->get(),
                &SceneView::modelDropped,
                [receiver = *receiver](const ModelPlacement& value) noexcept { receiver.get()(value); }
            );
            if (!connection)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::FACTORY_FAILURE,
                    "scene.model-drop",
                    static_cast<std::uint64_t>(connection.error())
                });
            }
            (*view)->model_connection_ = std::move(*connection);
        }
        return std::unique_ptr<lux::ui::Pane>(std::move(*view));
    }
    constinit const desktop::UiDescriptor kSceneView = []
    {
        desktop::UiDescriptor descriptor{
            views::ViewTypeIdView{"lux.editor.scene.view"},
            "Scene",
            ui_dependencies,
            1,
            nullptr,
            SceneView::createConfigured,
            kContentKinds,
            true,
            +[](const lux::ui::Pane& pane) noexcept -> views::ViewContent
            {
                const auto* author = std::get_if<EditedSceneBinding>(&static_cast<const SceneView&>(pane).binding());
                return author ? views::ViewContent{{author->session.id()}, author->session.id()} : views::ViewContent{};
            },
            +[](lux::ui::Pane& pane, const views::ViewContent& content) -> desktop::UiResult<void>
            {
                auto result = static_cast<SceneView&>(pane).rebindContent(content);
                if (!result)
                {
                    return cxx::unexpected(uiFailure(result.error()));
                }
                return {};
            },
            +[](lux::ui::Pane& pane) -> desktop::UiResult<void>
            {
                auto result = static_cast<SceneView&>(pane).cancelEdit();
                if (!result)
                {
                    return cxx::unexpected(uiFailure(result.error()));
                }
                return {};
            },
            +[](const lux::ui::Pane& pane) -> desktop::UiResult<workspace::VersionedViewState>
            { return static_cast<const SceneView&>(pane).captureState(); }
        };
        descriptor.prepare_state =
            +[](lux::ui::Pane& pane, const workspace::VersionedViewState& state) -> desktop::UiStateResult
        { return static_cast<SceneView&>(pane).prepareState(state.schema, state.bytes); };
        descriptor.cancel_preview = descriptor.prepare_close;
        descriptor.restore_content = true;
        return descriptor;
    }();
    struct SceneView::Impl final
    {
        // Asset drop belongs to the Scene tool. The shared viewport remains asset/domain independent.
        struct Canvas final : lux::ui::Element
        {
            Impl& owner_;
            lux::editor::views::ViewportElement viewport_;
            Canvas(object::ObjectDispatcherRef dispatcher, Impl& owner)
                : Element(dispatcher, lux::ui::ElementId{"canvas"}), owner_(owner),
                  viewport_(dispatcher, lux::ui::ElementId{"viewport"})
            {
                if (!addSubElement(viewport_))
                {
                    std::terminate(); // Complete fixed-member composition before attachment to any Root.
                }
            }
            lux::ui::SizeHint sizeHintContent() noexcept override
            {
                return viewport_.sizeHint();
            }
            lux::ui::SizeHint measureContent(float width) noexcept override
            {
                return viewport_.measure(width);
            }
            void arrangeContent() noexcept override
            {
                viewport_.arrange({{}, rect().size});
            }
            void draw() noexcept override
            {
                drawChild(viewport_);
                if (!viewport_.bound() || !ImGui::BeginDragDropTarget())
                {
                    return;
                }
                if (const auto* payload = ImGui::AcceptDragDropPayload(project::kAssetReferencePayload))
                {
                    auto reference = project::decodeAssetReference(
                        {static_cast<const std::byte*>(payload->Data), static_cast<std::size_t>(payload->DataSize)}
                    );
                    if (reference)
                    {
                        const auto& image = viewport_.image().interaction();
                        owner_.status_ = owner_.view_.dropModel(
                            *reference,
                            {image.local_pointer.x, image.local_pointer.y},
                            {image.size.width, image.size.height}
                        );
                    }
                    else
                    {
                        owner_.status_ = rejected(std::string_view{"Invalid model drag payload"});
                    }
                }
                ImGui::EndDragDropTarget();
            }
        };
        enum class EControl : std::uint8_t
        {
            NONE,
            UNDO,
            REDO
        };
        SceneView& view_;
        SceneViewServices services_;
        std::shared_ptr<SceneSession> model_;
        std::shared_ptr<SceneInteractionGroup> interaction_;
        VSceneViewBinding binding_{UnboundSceneBinding{}};
        SceneViewState state_;
        system::SystemInstanceId system_;
        // Shared derived projection only, never a writable author source or an instance lease.
        std::shared_ptr<SceneProjection> projection_;
        lux::scene::SceneInstanceId presented_;
        lux::ui::Layout layout_, toolbar_;
        lux::ui::Button undo_, redo_;
        lux::ui::Label message_;
        Canvas canvas_;
        lux::editor::views::ViewportElement& viewport_;
        std::array<object::Connection, 4> controls_;
        std::optional<lux::editor::views::ViewportPoint> pick_;
        lux::scene::SceneInstanceId pick_instance_;
        SceneViewResult<void> status_;
        EControl control_{};
        lux::editor::views::CameraMotion motion_;
        bool motion_pending_{}, camera_pending_{};

        Impl(SceneView& view, SceneViewServices services, SceneViewState state, system::SystemInstanceId system)
            : view_(view), services_(services), state_(std::move(state)), system_(system),
              layout_(view.dispatcherRef(), lux::ui::ElementId{"content"}),
              toolbar_(view.dispatcherRef(), lux::ui::ElementId{"toolbar"}, lux::ui::ELayoutType::HORIZONTAL),
              undo_(view.dispatcherRef(), lux::ui::ElementId{"undo"}, "Undo"),
              redo_(view.dispatcherRef(), lux::ui::ElementId{"redo"}, "Redo"),
              message_(view.dispatcherRef(), lux::ui::ElementId{"status"}, "No scene bound"),
              canvas_(view.dispatcherRef(), *this), viewport_(canvas_.viewport_)
        {
            toolbar_.setStretch({1, 0});
            message_.setStretch({1, 0});
            for (auto* child : std::array<lux::ui::Element*, 2>{&undo_, &redo_})
            {
                if (auto result = toolbar_.addSubElement(*child); !result)
                {
                    status_ = rejected(result.error());
                    return;
                }
            }
            for (auto* child : std::array<lux::ui::Element*, 3>{&toolbar_, &message_, &canvas_})
            {
                if (auto result = layout_.addSubElement(*child); !result)
                {
                    status_ = rejected(result.error());
                    return;
                }
            }
            if (auto result = view.setContent(layout_); !result)
            {
                status_ = rejected(result.error());
                return;
            }
            auto connect = [&](lux::ui::Button& button, EControl value, std::size_t index)
            {
                auto result = object::LuxObject::connect(
                    &button,
                    &lux::ui::Button::activated,
                    [this, value]() noexcept { control_ = value; }
                );
                if (result)
                {
                    controls_[index] = std::move(*result);
                }
                else
                {
                    status_ = rejected(desktop::EUiError::CAPACITY);
                }
            };
            connect(undo_, EControl::UNDO, 0);
            connect(redo_, EControl::REDO, 1);
            viewport_.enableNavigation(true);
            auto navigation = object::LuxObject::connect(
                &viewport_,
                &lux::editor::views::ViewportElement::cameraMoved,
                [this](const lux::editor::views::CameraMotion& motion) noexcept
                {
                    motion_.angular_delta += motion.angular_delta;
                    motion_.pan_delta += motion.pan_delta;
                    motion_.dolly += motion.dolly;
                    motion_pending_ = true;
                }
            );
            if (!navigation)
            {
                status_ = rejected(desktop::EUiError::CAPACITY);
            }
            else
            {
                controls_[2] = std::move(*navigation);
            }
            auto picked = object::LuxObject::connect(
                &viewport_,
                &lux::editor::views::ViewportElement::clicked,
                [this](const lux::editor::views::ViewportPoint& point) noexcept
                {
                    pick_ = point;
                    pick_instance_ = presented_;
                }
            );
            if (!picked)
            {
                status_ = rejected(desktop::EUiError::CAPACITY);
            }
            else
            {
                controls_[3] = std::move(*picked);
            }
        }
        SceneViewResult<std::unique_ptr<lux::editor::views::ViewportPresentation>> preparePresentation(
            lux::scene::SceneInstanceId instance,
            std::optional<system::SystemInstanceId> requested_system = {}
        )
        {
            auto prepared = lux::editor::views::ViewportPresentation::create(
                services_.runtime,
                instance,
                services_.resources,
                requested_system.value_or(system_),
                state_.camera.transform,
                state_.camera.camera,
                lux::scene::ViewConfig{.extent = state_.extent}
            );
            if (!prepared)
            {
                return rejected(prepared.error());
            }
            return std::move(*prepared);
        }
        SceneViewResult<void> rebind(
            VSceneViewBinding binding,
            std::optional<system::SystemInstanceId> requested_system = {}
        )
        {
            if (!view_.isOnAffinityThread() || object::LuxObject::isDispatching())
            {
                return rejected(desktop::EUiError::BUSY);
            }
            if (binding == binding_)
            {
                return {};
            }
            std::shared_ptr<SceneProjection> projection;
            std::shared_ptr<SceneSession> model;
            lux::scene::SceneInstanceId instance;
            if (const auto* author = std::get_if<EditedSceneBinding>(&binding))
            {
                const bool is_wrong_group = !author->interaction || author->interaction->session() != author->session;
                if (is_wrong_group)
                {
                    return rejected(desktop::EUiError::NOT_FOUND);
                }
                auto session = services_.sessions.read(author->session);
                if (!session)
                {
                    return rejected(SceneEditError{session.error()});
                }
                auto shared = services_.sessions.share(author->session);
                if (!shared)
                {
                    return rejected(SceneEditError{shared.error()});
                }
                model = std::move(*shared);
                auto acquired = services_.projections->acquire(session->get(), *services_.environment);
                if (!acquired)
                {
                    return rejected(acquired.error());
                }
                projection = std::move(*acquired);
                instance = projection->instance();
            }
            else if (const auto* running = std::get_if<RunningSceneBinding>(&binding))
            {
                if (!services_.runs || !running->interaction)
                {
                    return rejected(desktop::EUiError::NOT_FOUND);
                }
                auto run = services_.runs->describe(running->run);
                if (!run)
                {
                    return rejected(run.error());
                }
                const bool wrong_group = running->interaction->run() ? *running->interaction->run() != running->run
                                                                     : !running->interaction->session() ||
                                                                           running->interaction->session()->id() !=
                                                                               run->provenance.content.session;
                if (wrong_group)
                {
                    return rejected(desktop::EUiError::NOT_FOUND);
                }
                if (run->state == ERunState::STOPPED || run->state == ERunState::STOPPING)
                {
                    return rejected(RunFailure{ERunError::STOPPED});
                }
                instance = run->instance;
            }
            std::unique_ptr<lux::editor::views::ViewportPresentation> presentation;
            if (instance.valid())
            {
                auto prepared = preparePresentation(instance, requested_system);
                if (!prepared)
                {
                    return rejected(prepared.error());
                }
                presentation = std::move(*prepared);
            }
            // Only after the entire candidate has been prepared may the old interaction be ended.
            // BUSY preserves both original binding and overlay; candidate resource cancellation is owned.
            if (auto* previous = interaction(binding_))
            {
                auto cancelled = previous->cancel();
                if (!cancelled)
                {
                    return rejected(cancelled.error());
                }
            }
            motion_pending_ = false;
            motion_ = {};
            pick_.reset();
            projection_ = std::move(projection);
            viewport_.setPresentation(std::move(presentation), state_.extent);
            presented_ = instance;
            binding_ = std::move(binding);
            model_ = std::move(model);
            if (requested_system)
            {
                system_ = *requested_system;
            }
            status_ = {};
            return {};
        }
        SceneViewResult<void> history(bool redo)
        {
            auto* author = std::get_if<EditedSceneBinding>(&binding_);
            if (!author)
            {
                return rejected(desktop::EUiError::ATTACHMENT);
            }
            if (author->interaction->overlay())
            {
                return rejected(desktop::EUiError::BUSY);
            }
            auto session = services_.sessions.edit(author->session);
            if (!session)
            {
                return rejected(SceneEditError{session.error()});
            }
            return redo ? adopted(session->get().redo()) : adopted(session->get().undo());
        }
        SceneViewResult<void> navigate(const lux::editor::views::CameraMotion& motion)
        {
            auto candidate = lux::editor::views::navigateCamera(state_.camera.transform, state_.camera.camera, motion);
            if (!candidate)
            {
                return rejected(candidate.error());
            }
            if (viewport_.bound())
            {
                auto changed = viewport_.presentation().setCameraPose(candidate->transform, candidate->camera);
                if (!changed)
                {
                    return rejected(changed.error());
                }
            }
            state_.camera = std::move(*candidate);
            return {};
        }
        SceneViewResult<lux::math::Ray3d> imageRay(Eigen::Vector2d position, Eigen::Vector2d extent)
        {
            const bool is_valid_input = position.allFinite() && extent.allFinite() && (extent.array() > 0).all() &&
                                        (position.array() >= 0).all() && (position.array() <= extent.array()).all();
            if (!is_valid_input || !viewport_.bound())
            {
                return rejected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
            }
            if (camera_pending_ || motion_pending_)
            {
                return rejected(render::RendererFailure{render::ERendererError::NOT_READY});
            }
            auto output = viewport_.presentation().currentImageExtent();
            if (!output)
            {
                return rejected(output.error());
            }
            // The displayed image may be stretched while resize is pending. Normalize once, then
            // use the sampled output's aspect; framebuffer scale is not applied again here.
            const Eigen::Vector2d pixels{double(output->width), double(output->height)};
            const Eigen::Vector2d point = position.cwiseQuotient(extent).cwiseProduct(pixels);
            simulation::ecs::WorldTransform3D camera;
            camera.value = Eigen::Translation3d(state_.camera.transform.translation) *
                           state_.camera.transform.rotation * Eigen::Scaling(state_.camera.transform.scale);
            auto ray = views::cameraRay(camera, state_.camera.camera, point, pixels);
            if (!ray)
            {
                return rejected(ray.error());
            }
            return *ray;
        }
        SceneViewResult<void> pick(Eigen::Vector2d position, Eigen::Vector2d extent)
        {
            auto* selected = interaction(binding_);
            if (!selected || !presented_.valid())
            {
                return rejected(desktop::EUiError::ATTACHMENT);
            }
            const auto* author = std::get_if<EditedSceneBinding>(&binding_);
            if (author)
            {
                auto info = services_.sessions.describe(author->session);
                if (!info)
                {
                    return rejected(SceneEditError{info.error()});
                }
                if (!projection_ || projection_->version().content != info->current)
                {
                    return rejected(SceneEditError{ESceneEditError::STALE_CONTENT});
                }
            }
            auto registry = std::as_const(services_.runtime).borrowInstance(presented_);
            if (!registry)
            {
                return rejected(ProjectionFailure{registry.error()});
            }
            const auto* query = registry->get().ctx().find<lux::scene::MeshQuery>();
            if (!query)
            {
                return rejected(lux::scene::MeshQueryFailure{lux::scene::EMeshQueryError::NOT_READY});
            }
            auto ray = imageRay(position, extent);
            if (!ray)
            {
                return cxx::unexpected(ray.error());
            }
            lux::scene::RayHit3D hit;
            auto found = query->raycastNearest(*ray, 1.0e12, hit);
            if (!found)
            {
                return rejected(found.error());
            }
            SceneSelection selection;
            if (*found)
            {
                if (author)
                {
                    const auto* identities = registry->get().ctx().find<lux::scene::WorldResidency>();
                    if (!identities)
                    {
                        return rejected(SceneEditError{ESceneEditError::INVALID_OBJECT});
                    }
                    const auto object = identities->identities().object(hit.entity);
                    selection.objects.emplace_back(
                        SceneObjectRef{author->session.id(), projection_->version().content.state.history, object}
                    );
                }
                else if (auto* running = std::get_if<RunningSceneBinding>(&binding_); running && services_.runs)
                {
                    auto ref = services_.runs->reference(running->run, hit.entity);
                    if (!ref)
                    {
                        return rejected(ref.error());
                    }
                    selection.objects.emplace_back(*ref);
                }
            }
            auto ended = selected->cancel();
            if (!ended)
            {
                return rejected(ended.error());
            }
            return adopted(selected->select(std::move(selection)));
        }
        SceneViewResult<ModelPlacement> placement(
            AssetReference asset,
            Eigen::Vector2d position,
            Eigen::Vector2d extent
        )
        {
            const auto* author = std::get_if<EditedSceneBinding>(&binding_);
            if (!author || !projection_ || !presented_.valid())
            {
                return rejected(desktop::EUiError::ATTACHMENT);
            }
            const auto based_on = projection_->version().content;
            auto session = services_.sessions.read(author->session);
            if (!session)
            {
                return rejected(SceneEditError{session.error()});
            }
            if (session->get().describe().current != based_on)
            {
                return rejected(SceneEditError{ESceneEditError::STALE_CONTENT});
            }
            auto read = session->get().read();
            if (!read)
            {
                return rejected(read.error());
            }
            auto single = read->withRead(
                [](const SceneReadView& source) -> SceneEditResult<void>
                {
                    // Multi-partition placement needs an explicit destination; never guess ordinal zero.
                    if (source.configuration().world->data().partitionCount() != 1)
                    {
                        return cxx::unexpected(SceneEditError{ESceneEditError::INVALID_PARTITION});
                    }
                    return {};
                }
            );
            if (!single)
            {
                return rejected(single.error());
            }
            auto ray = imageRay(position, extent);
            if (!ray)
            {
                return cxx::unexpected(ray.error());
            }
            auto registry = std::as_const(services_.runtime).borrowInstance(presented_);
            if (!registry)
            {
                return rejected(ProjectionFailure{registry.error()});
            }
            lux::scene::RayHit3D hit;
            bool found{};
            if (const auto* query = registry->get().ctx().find<lux::scene::MeshQuery>())
            {
                auto result = query->raycastNearest(*ray, 1.0e12, hit);
                if (!result)
                {
                    return rejected(result.error());
                }
                found = *result;
            }
            auto point = sceneCreationPoint(found ? &hit : nullptr, *ray, state_.work_plane_height);
            if (!point)
            {
                return rejected(point.error());
            }
            return ModelPlacement{author->session, based_on, asset, *point, {0}};
        }
        SceneViewResult<void> refresh()
        {
            if (const auto* author = std::get_if<EditedSceneBinding>(&binding_))
            {
                auto session = services_.sessions.read(author->session);
                if (!session)
                {
                    return rejected(SceneEditError{session.error()});
                }
                if (projection_->version().environment != services_.environment->version)
                {
                    auto next = services_.projections->acquire(session->get(), *services_.environment);
                    if (!next)
                    {
                        return rejected(next.error());
                    }
                    auto presentation = preparePresentation((*next)->instance());
                    if (!presentation)
                    {
                        return rejected(presentation.error());
                    }
                    // The complete candidate is ready; old output and camera survived every failed attempt.
                    viewport_.setPresentation(std::move(*presentation), state_.extent);
                    projection_ = std::move(*next);
                    presented_ = projection_->instance();
                }
                auto refreshed = projection_->update(session->get());
                if (!refreshed)
                {
                    return rejected(refreshed.error());
                }
                if (projection_->instance() != presented_)
                {
                    auto prepared = preparePresentation(projection_->instance());
                    if (!prepared)
                    {
                        return rejected(prepared.error());
                    }
                    viewport_.setPresentation(std::move(*prepared), state_.extent);
                    presented_ = projection_->instance();
                }
            }
            else if (const auto* running = std::get_if<RunningSceneBinding>(&binding_))
            {
                auto info = services_.runs->describe(running->run);
                if (!info)
                {
                    return rejected(info.error());
                }
                if (info->instance != presented_ || info->state == ERunState::STOPPED ||
                    info->state == ERunState::STOPPING)
                {
                    return rejected(RunFailure{ERunError::STOPPED});
                }
            }
            if (!viewport_.bound())
            {
                return {};
            }
            if (camera_pending_)
            {
                auto changed = viewport_.presentation().setCameraPose(state_.camera.transform, state_.camera.camera);
                if (!changed)
                {
                    return rejected(changed.error());
                }
                camera_pending_ = false;
            }
            if (motion_pending_)
            {
                auto moved = navigate(motion_);
                if (!moved)
                {
                    return moved;
                }
                motion_ = {};
                motion_pending_ = false;
            }
            auto* group = interaction(binding_);
            if (group)
            {
                auto synchronized = group->synchronize();
                if (!synchronized)
                {
                    return rejected(synchronized.error());
                }
            }
            auto registry = std::as_const(services_.runtime).borrowInstance(presented_);
            if (!registry)
            {
                return rejected(ProjectionFailure{registry.error()});
            }
            lux::editor::views::OverlayConfiguration overlay;
            overlay.plane_height = state_.work_plane_height;
            if (group && !group->selection().objects.empty())
            {
                std::visit(
                    [&](const auto& selected)
                    {
                        using T = std::decay_t<decltype(selected)>;
                        if constexpr (std::same_as<T, SceneObjectRef>)
                        {
                            const auto* author = std::get_if<EditedSceneBinding>(&binding_);
                            const auto* identities = registry->get().ctx().find<lux::scene::WorldResidency>();
                            const bool is_current = author && selected.session == author->session.id() &&
                                                    selected.history == projection_->version().content.state.history;
                            if (is_current && identities)
                            {
                                overlay.selection = identities->identities().entity(selected.object);
                            }
                        }
                        else
                        {
                            const auto* run = std::get_if<RunningSceneBinding>(&binding_);
                            if (run && selected.run == run->run && selected.instance == presented_)
                            {
                                overlay.selection = selected.entity;
                            }
                        }
                    },
                    group->selection().objects.front()
                );
            }
            // Entity generation and projection serial are part of the backend overlay key.
            overlay.selection_version = static_cast<std::uint32_t>(overlay.selection);
            overlay.structure_version = projection_ ? projection_->version().serial : presented_.generation;
            auto submitted = viewport_.presentation().updateOverlay(services_.renderer, overlay);
            if (!submitted)
            {
                return rejected(submitted.error());
            }
            return {};
        }
        void update() noexcept
        {
            const bool author = std::holds_alternative<EditedSceneBinding>(binding_);
            undo_.setEnabled(author);
            redo_.setEnabled(author);
            const auto control = std::exchange(control_, EControl::NONE);
            if (control != EControl::NONE)
            {
                status_ = history(control == EControl::REDO);
            }
            auto refreshed = refresh();
            if (!refreshed)
            {
                status_ = std::move(refreshed);
            }
            else if (pick_)
            {
                const auto point = *std::exchange(pick_, {});
                status_ = pick_instance_ == presented_
                              ? pick({point.position.x, point.position.y}, {point.extent.width, point.extent.height})
                              : SceneViewResult<void>{rejected(desktop::EUiError::NOT_FOUND)};
            }
            if (std::holds_alternative<UnboundSceneBinding>(binding_))
            {
                message_.setText("No scene bound");
            }
            else if (!status_)
            {
                message_.setText("Scene operation unavailable; previous binding retained");
            }
            else
            {
                message_.setText(author ? "Author scene" : "Run (independent source)");
            }
        }
    };
    SceneView::SceneView(object::ObjectDispatcherRef dispatcher, SceneViewServices services, SceneViewCreateInfo info)
        : Pane(dispatcher, std::move(info.id), lux::ui::PaneTypeId{kSceneView.type.name()}, std::move(info.title)),
          impl_(std::make_unique<Impl>(*this, services, std::move(info.state), info.render_system))
    {
    }
    SceneView::~SceneView() noexcept = default;
    SceneViewResult<std::unique_ptr<SceneView>> SceneView::create(
        object::ObjectDispatcherRef dispatcher,
        SceneViewServices services,
        SceneViewCreateInfo info
    )
    {
        const bool is_invalid_services = !services.projections || !services.environment;
        if (is_invalid_services)
        {
            return rejected(desktop::EUiError::ATTACHMENT);
        }
        auto binding = info.binding;
        auto view = std::unique_ptr<SceneView>(new SceneView(dispatcher, services, std::move(info)));
        if (!view->status())
        {
            return rejected(view->status().error());
        }
        auto bound = view->rebind(std::move(binding));
        if (!bound)
        {
            return rejected(bound.error());
        }
        return view;
    }
    const VSceneViewBinding& SceneView::binding() const noexcept
    {
        return impl_->binding_;
    }
    const SceneViewState& SceneView::state() const noexcept
    {
        return impl_->state_;
    }
    desktop::UiResult<workspace::VersionedViewState> SceneView::captureState() const
    {
        workspace::VersionedViewState result;
        serialization::BinaryWriter writer(result.bytes);
        views::detail::writeViewportState(writer, impl_->state_.camera, impl_->state_.extent);
        (void)writer.writeFloat(impl_->state_.work_plane_height);
        return result;
    }
    desktop::UiResult<cxx::move_only_function<void()>> SceneView::prepareState(
        std::uint32_t schema,
        std::span<const std::byte> bytes
    )
    {
        auto candidate = decodeState(schema, bytes);
        if (!candidate)
        {
            return cxx::unexpected(candidate.error());
        }
        if (!*candidate)
        {
            return cxx::move_only_function<void()>{};
        }
        return cxx::move_only_function<void()>{[this, value = **candidate]() noexcept
                                               {
                                                   impl_->state_ = value;
                                                   impl_->camera_pending_ = true;
                                               }};
    }
    SceneViewResult<void> SceneView::rebind(VSceneViewBinding binding)
    {
        return impl_->rebind(std::move(binding));
    }
    SceneViewResult<void> SceneView::pick(Eigen::Vector2d position, Eigen::Vector2d extent)
    {
        return impl_->pick(position, extent);
    }
    SceneViewResult<void> SceneView::dropModel(
        AssetReference reference,
        Eigen::Vector2d position,
        Eigen::Vector2d extent
    )
    {
        auto request = impl_->placement(reference, position, extent);
        if (!request)
        {
            return cxx::unexpected(request.error());
        }
        const auto delivery = emit(modelDropped, *request);
        if (!delivery.complete())
        {
            return rejected(desktop::EUiError::CAPACITY);
        }
        return {};
    }
    SceneViewResult<void> SceneView::navigate(const lux::editor::views::CameraMotion& motion)
    {
        return impl_->navigate(motion);
    }
    SceneViewResult<void> SceneView::undo()
    {
        return impl_->history(false);
    }
    SceneViewResult<void> SceneView::redo()
    {
        return impl_->history(true);
    }
    const std::shared_ptr<SceneInteractionGroup>& SceneView::interactionOwner() const noexcept
    {
        return impl_->interaction_;
    }
    SceneViewResult<void> SceneView::rebindRun(RunId run, std::optional<system::SystemInstanceId> render_system)
    {
        if (!impl_->services_.runs)
        {
            return rejected(desktop::EUiError::ATTACHMENT);
        }
        auto group =
            std::make_shared<SceneInteractionGroup>(*impl_->services_.runs, run, InteractionGroupId{id().hash()});
        auto adopted = impl_->rebind(RunningSceneBinding{run, group.get()}, render_system);
        if (!adopted)
        {
            return adopted;
        }
        impl_->interaction_ = std::move(group);
        return {};
    }
    SceneViewResult<void> SceneView::rebindContent(const views::ViewContent& content)
    {
        const bool is_single = content.sessions.size() == 1 && content.primary == content.sessions.front();
        const bool is_invalid = !content.valid() || (!content.sessions.empty() && !is_single);
        if (is_invalid)
        {
            return rejected(desktop::EUiError::NOT_FOUND);
        }
        if (const auto* current = std::get_if<EditedSceneBinding>(&impl_->binding_);
            current && is_single && current->session.id() == *content.primary)
        {
            return {};
        }
        std::shared_ptr<SceneInteractionGroup> group;
        VSceneViewBinding binding{UnboundSceneBinding{}};
        std::optional<system::SystemInstanceId> render_system;
        if (is_single)
        {
            auto key = impl_->services_.sessions.key(*content.primary);
            if (!key)
            {
                return rejected(SceneEditError{key.error()});
            }
            auto session = impl_->services_.sessions.read(*key);
            if (!session)
            {
                return rejected(SceneEditError{session.error()});
            }
            auto view = session->get().read();
            if (!view)
            {
                return rejected(view.error());
            }
            auto selected = view->withRead(
                [&](const auto& read) -> SceneEditResult<void>
                {
                    const auto& description = read.configuration().scene->data();
                    for (std::size_t i{}; i < description.systemCount(); ++i)
                    {
                        if (const auto system = description.systemAt(i);
                            system.type() == lux::scene::builtinRenderSystemRegistration().type)
                        {
                            if (render_system)
                            {
                                return cxx::unexpected(SceneEditError{sessions::ESessionError::INVALID_ARGUMENT});
                            }
                            render_system = system.instanceId();
                        }
                    }
                    return {};
                }
            );
            if (!selected)
            {
                return rejected(selected.error());
            }
            group = std::make_shared<SceneInteractionGroup>(
                impl_->services_.sessions,
                *key,
                InteractionGroupId{id().hash()},
                impl_->services_.runs
            );
            binding = EditedSceneBinding{*key, group.get()};
        }
        auto adopted = impl_->rebind(std::move(binding), render_system);
        if (!adopted)
        {
            return adopted;
        }
        impl_->interaction_ = std::move(group);
        return {};
    }
    SceneViewResult<void> SceneView::beginEdit(std::string label)
    {
        auto* author = std::get_if<EditedSceneBinding>(&impl_->binding_);
        return author ? adopted(author->interaction->begin(std::move(label)))
                      : SceneViewResult<void>{rejected(desktop::EUiError::ATTACHMENT)};
    }
    SceneViewResult<void> SceneView::previewEdit(std::vector<VSceneEdit>& edits)
    {
        auto* author = std::get_if<EditedSceneBinding>(&impl_->binding_);
        return author ? adopted(author->interaction->preview(edits))
                      : SceneViewResult<void>{rejected(desktop::EUiError::ATTACHMENT)};
    }
    SceneViewResult<void> SceneView::commitEdit()
    {
        auto* author = std::get_if<EditedSceneBinding>(&impl_->binding_);
        return author ? adopted(author->interaction->commit())
                      : SceneViewResult<void>{rejected(desktop::EUiError::ATTACHMENT)};
    }
    SceneViewResult<void> SceneView::cancelEdit()
    {
        auto* group = interaction(impl_->binding_);
        return group ? adopted(group->cancel()) : SceneViewResult<void>{};
    }
    std::optional<sessions::ContentStamp> SceneView::projectedContent() const noexcept
    {
        return impl_->projection_ ? std::optional{impl_->projection_->version().content} : std::nullopt;
    }
    lux::scene::SceneInstanceId SceneView::presentedInstance() const noexcept
    {
        return impl_->presented_;
    }
    lux::scene::RenderResourceId SceneView::viewport() const noexcept
    {
        return impl_->viewport_.view();
    }
    system::SystemInstanceId SceneView::renderSystem() const noexcept
    {
        return impl_->system_;
    }
    render::RTextureHandle SceneView::image() const noexcept
    {
        return impl_->viewport_.image().image();
    }
    const SceneViewResult<void>& SceneView::status() const noexcept
    {
        return impl_->status_;
    }
    void SceneView::update() noexcept
    {
        impl_->update();
    }

} // namespace lux::editor::scene
