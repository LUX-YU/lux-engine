#include <lux/engine/editor/application/EditorApplication.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <exception>
#include <new>
#include <thread>
#include <utility>

namespace lux::editor
{
    namespace
    {
        struct CloseResult final
        {
            std::atomic_bool complete{};
        };

        struct CloseReceiver final
        {
            using receiver_concept = stdexec::receiver_t;

            void set_value() && noexcept
            {
                result->complete.store(true, std::memory_order_release);
                result->complete.notify_all();
            }

            [[nodiscard]] stdexec::empty_env get_env() const noexcept { return {}; }

            CloseResult* result{};
        };
    } // namespace

    EditorApplication::EditorApplication(
        process::ExecutionRuntime runtime,
        scene::SceneMetaManager scene_meta
    )
        : execution_(std::move(runtime)),
          scene_meta_(std::move(scene_meta))
    {
        ui_.emplace();
        selection_.emplace(ui_->dispatcherRef());
        toolset_.emplace();
        tasks_.emplace();
    }

    EditorApplication::CreateResult EditorApplication::create(EditorApplicationCreateInfo info) noexcept
    {
        auto runtime = process::ExecutionRuntime::create(info.execution);
        if (!runtime)
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_CREATE_FAILURE);

        std::unique_ptr<EditorApplication> application;
        try
        {
            application.reset(new EditorApplication(std::move(*runtime), std::move(info.scene_meta)));
            for (auto& mount : info.mounts)
            {
                if (application->vfs_.mount(std::move(mount)) == asset::kInvalidMountId)
                {
                    static_cast<void>(application->shutdown());
                    return lux::cxx::unexpected(EEditorApplicationError::VFS_MOUNT_FAILURE);
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            if (application)
                static_cast<void>(application->shutdown());
            return lux::cxx::unexpected(EEditorApplicationError::ALLOCATION_FAILURE);
        }

        auto blocking = application->execution_.blocking();
        if (!blocking)
        {
            static_cast<void>(application->shutdown());
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_CREATE_FAILURE);
        }
        auto endpoint = process::asset_loading::VfsAssetReadEndpoint::create(
            application->vfs_.view(),
            *blocking,
            info.asset_read
        );
        if (!endpoint)
        {
            static_cast<void>(application->shutdown());
            return lux::cxx::unexpected(EEditorApplicationError::ASSET_READ_CREATE_FAILURE);
        }
        application->asset_read_endpoint_ = std::move(*endpoint);
        return application;
    }

    EditorApplication::~EditorApplication() noexcept
    {
        if (state_ != EState::JOINED)
            std::terminate();
    }

    lux::cxx::expected<void, EEditorApplicationError> EditorApplication::start() noexcept
    {
        if (state_ != EState::COMPOSING || !asset_read_endpoint_)
            return lux::cxx::unexpected(EEditorApplicationError::INVALID_STATE);
        toolset_->freeze();
        context_.emplace(EditorContextCreateInfo{
            *toolset_,
            vfs_.view(),
            asset_read_endpoint_->port(),
            execution_,
            *tasks_,
            *selection_,
            *ui_,
            scene_meta_
        });
        state_ = EState::RUNNING;
        return {};
    }

    EditorApplication::ContextResult EditorApplication::context() noexcept
    {
        if (state_ != EState::RUNNING || !context_)
            return lux::cxx::unexpected(EEditorApplicationError::INVALID_STATE);
        return std::ref(*context_);
    }

    lux::cxx::expected<std::size_t, EEditorApplicationError>
    EditorApplication::drainMain(std::size_t budget) noexcept
    {
        auto drained = execution_.drainMain(budget);
        if (!drained)
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_JOIN_FAILURE);
        return *drained;
    }

    bool EditorApplication::closeRootTasks() noexcept
    {
        CloseResult result;
        try
        {
            auto operation = stdexec::connect(tasks_->close(), CloseReceiver{&result});
            stdexec::start(operation);
            while (!result.complete.load(std::memory_order_acquire))
            {
                if (!execution_.drainMain(64U))
                    return false;
                std::this_thread::yield();
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    lux::cxx::expected<void, EEditorApplicationError> EditorApplication::shutdown() noexcept
    {
        if (state_ == EState::JOINED)
            return lux::cxx::unexpected(EEditorApplicationError::INVALID_STATE);
        state_ = EState::STOPPING;
        if (!closeRootTasks())
            return lux::cxx::unexpected(EEditorApplicationError::TASK_CLOSE_FAILURE);

        context_.reset();
        tasks_.reset();
        toolset_->requestStop();
        toolset_.reset();
        selection_.reset();
        ui_.reset();
        if (asset_read_endpoint_)
        {
            asset_read_endpoint_->requestStop();
            if (!asset_read_endpoint_->join())
                return lux::cxx::unexpected(EEditorApplicationError::ASSET_READ_JOIN_FAILURE);
            asset_read_endpoint_.reset();
        }

        execution_.requestStop();
        while (true)
        {
            auto drained = execution_.drainMain(64U);
            if (!drained)
                return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_JOIN_FAILURE);
            if (*drained == 0U)
                break;
        }
        if (!execution_.join())
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_JOIN_FAILURE);
        state_ = EState::JOINED;
        return {};
    }
} // namespace lux::editor
