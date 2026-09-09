#include <lux/engine/editor/sessions/scene/SceneView.hpp>
#include <lux/engine/editor/rendering/RenderView.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
namespace lux::editor::sessions
{
    namespace
    {
        auto fail(ESceneError code, SessionId id = {}) noexcept
        {
            return lux::cxx::unexpected(SceneFailure{code, id});
        }
        auto renderFailure(const rendering::RendererFailure &source, SessionId id) noexcept
        {
            SceneFailure error{ESceneError::RESOURCE_FAILURE, id};
            error.renderer = source;
            if (source.code == rendering::ERendererError::NOT_READY)
                error.code = ESceneError::NOT_READY;
            if (source.code == rendering::ERendererError::WRONG_THREAD)
                error.code = ESceneError::WRONG_THREAD;
            return lux::cxx::unexpected(error);
        }
    } // namespace
    struct SceneView::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        SceneSession *session{};
        SessionId session_id;
        std::unique_ptr<rendering::RenderView> view;
        SceneCamera camera;
        rendering::ViewStatus observed;
        std::uint64_t revision{1};
        bool closing{}, closed{true}, busy{};
        SceneResult<void> check() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return fail(ESceneError::WRONG_THREAD);
            if (busy)
                return fail(ESceneError::BUSY, session_id);
            if (closed)
                return fail(ESceneError::CLOSED, session_id);
            return {};
        }
    };
    SceneView::SceneView(lux::object::ObjectDispatcherRef dispatcher, std::unique_ptr<Impl> impl)
        : Object(std::move(dispatcher)), impl_(std::move(impl))
    {
    }
    SceneView::~SceneView() noexcept
    {
        if (!impl_->closed)
            std::terminate();
    }
    SceneResult<std::unique_ptr<SceneView>> SceneView::create(lux::object::ObjectDispatcherRef dispatcher,
                                                              SceneSession &session,
                                                              rendering::EditorRenderer &renderer) noexcept
    {
        if (!dispatcher || !dispatcher.isCurrent())
            return fail(ESceneError::WRONG_THREAD);
        try
        {
            auto impl = std::make_unique<Impl>();
            impl->session = &session;
            impl->session_id = session.id();
            auto owner = std::unique_ptr<SceneView>(new SceneView(std::move(dispatcher), std::move(impl)));
            auto scene = session.attachView(renderer);
            if (!scene)
                return lux::cxx::unexpected(scene.error());
            auto view = renderer.openView(*scene, {{0, 0}, true});
            if (!view)
            {
                session.detachView();
                return renderFailure(view.error(), session.id());
            }
            owner->impl_->view = std::move(*view);
            owner->impl_->closed = false;
            return owner;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session.id());
        }
    }
    SceneResult<void> SceneView::synchronize() noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (impl_->closing)
            return fail(ESceneError::CLOSED, impl_->session_id);
        const auto status = impl_->view->status();
        if (status.state == rendering::EViewState::FAILED && status.failure)
            return renderFailure(*status.failure, impl_->session_id);
        const auto extent = status.ready_extent;
        if (extent.width && extent.height)
        {
            auto projection = impl_->camera.projection(static_cast<double>(extent.width) / extent.height);
            if (!projection)
                return lux::cxx::unexpected(projection.error());
            const Eigen::Vector3d origin = (impl_->camera.position().array() / 1024).floor().matrix() * 1024;
            const auto view = impl_->camera.view(origin);
            rendering::CameraFrame camera;
            std::copy_n(view.data(), 16, camera.view.data());
            std::copy_n(projection->data(), 16, camera.projection.data());
            std::copy_n(origin.data(), 3, camera.origin.data());
            camera.desired = {impl_->session_id.value, impl_->session->stamp().content_revision, impl_->revision,
                              status.acknowledged_sequence};
            auto result = impl_->view->setCamera(camera);
            if (!result)
                return renderFailure(result.error(), impl_->session_id);
        }
        const bool changed = status.state != impl_->observed.state ||
                             status.acknowledged_sequence != impl_->observed.acknowledged_sequence;
        impl_->observed = status;
        if (changed)
        {
            impl_->busy = true;
            notify<imageChanged>(ViewImageNotice{impl_->session_id, status});
            impl_->busy = false;
        }
        return {};
    }
    SceneResult<void> SceneView::moveCamera(const CameraMotion &motion) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (impl_->closing)
            return fail(ESceneError::CLOSED, impl_->session_id);
        if (impl_->revision == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ESceneError::CONTRACT_FAILURE, impl_->session_id);
        auto result = impl_->camera.move(motion);
        if (result)
            ++impl_->revision;
        return result;
    }
    SessionId SceneView::sessionId() const noexcept
    {
        return impl_->session_id;
    }
    SceneResult<void> SceneView::resetCamera() noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (impl_->closing)
            return fail(ESceneError::CLOSED, impl_->session_id);
        if (impl_->revision == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ESceneError::CONTRACT_FAILURE, impl_->session_id);
        impl_->camera.reset();
        ++impl_->revision;
        return {};
    }
    SceneResult<void> SceneView::frameSelection() noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (impl_->closing)
            return fail(ESceneError::CLOSED, impl_->session_id);
        const auto selection = impl_->session->selection();
        if (!selection.current)
            return fail(ESceneError::NOT_READY, impl_->session_id);
        auto data = impl_->session->readEntity(*selection.current);
        if (!data)
            return lux::cxx::unexpected(data.error());
        Eigen::Vector3d center = Eigen::Vector3d::Zero();
        if (data->world_transform)
            center = data->world_transform->value.translation();
        else if (data->transform)
            center = data->transform->translation;
        if (impl_->revision == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ESceneError::CONTRACT_FAILURE, impl_->session_id);
        auto result = impl_->camera.focus(center, 1.5);
        if (result)
            ++impl_->revision;
        return result;
    }
    SceneResult<void> SceneView::requestExtent(rendering::PixelExtent extent) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        auto result = impl_->view->requestExtent(extent);
        if (!result)
            return renderFailure(result.error(), impl_->session_id);
        return {};
    }
    SceneResult<rendering::ViewImage> SceneView::image() const noexcept
    {
        if (auto result = impl_->check(); !result)
            return lux::cxx::unexpected(result.error());
        auto result = impl_->view->acquireImage();
        if (!result)
            return renderFailure(result.error(), impl_->session_id);
        return std::move(*result);
    }
    SceneResult<void> SceneView::beginClose() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return fail(ESceneError::WRONG_THREAD);
        if (impl_->busy)
            return fail(ESceneError::BUSY, impl_->session_id);
        if (impl_->closed)
            return {};
        auto result = impl_->view->beginClose();
        if (!result)
            return renderFailure(result.error(), impl_->session_id);
        impl_->closing = true;
        return {};
    }
    SceneResult<ECloseProgress> SceneView::advanceClose() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return fail(ESceneError::WRONG_THREAD);
        if (impl_->busy)
            return fail(ESceneError::BUSY, impl_->session_id);
        if (impl_->closed)
            return ECloseProgress::COMPLETE;
        if (!impl_->closing)
            return fail(ESceneError::BUSY, impl_->session_id);
        auto result = impl_->view->advanceClose();
        if (!result)
            return renderFailure(result.error(), impl_->session_id);
        if (*result == rendering::ERenderClose::PENDING)
            return ECloseProgress::PENDING;
        impl_->view.reset();
        impl_->session->detachView();
        impl_->closed = true;
        return ECloseProgress::COMPLETE;
    }
} // namespace lux::editor::sessions
