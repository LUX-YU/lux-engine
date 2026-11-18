#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/device/NativeExecution.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>

namespace lux::render::detail
{
    DeviceEntry::DeviceEntry(
        ResourceRegistry& registry,
        std::unique_ptr<NativeExecution> native,
        std::unique_ptr<DeviceSession> session,
        std::uint32_t frame_count,
        std::size_t preparation_capacity
    ) noexcept
        : native_(std::move(native)), session_(std::move(session)),
          frame_progress_(frame_count, session_.get(), session_->queueCount()),
          frame_workspace_(frame_count, session_->queueCount(), preparation_capacity),
          resources_(registry, *session_)
    {
    }

    RenderResult<std::unique_ptr<DeviceEntry>> DeviceEntry::create(
        ResourceRegistry& registry,
        const DeviceOptions& options,
        const nri::CallbackInterface& callbacks,
        const RenderServerConfig& config,
        execution::CpuScheduler cpu
    ) noexcept
    {
        auto native = NativeExecution::create();
        if (!native)
        {
            return cxx::unexpected(native.error());
        }
        auto session = DeviceSession::create(options, callbacks, config.validation, (*native)->scheduler());
        if (!session)
        {
            return cxx::unexpected(session.error());
        }
        const auto preparation_capacity = config.uploads ? config.uploads->window_regions : 0;
        auto entry = std::unique_ptr<DeviceEntry>(
            new DeviceEntry(registry, std::move(*native), std::move(*session), config.frames_in_flight,
                preparation_capacity)
        );
        if (config.uploads)
        {
            auto uploader = ResourceUploader::create(entry->session(), *config.uploads, cpu);
            if (!uploader)
            {
                return cxx::unexpected(uploader.error());
            }
            entry->uploader_ = std::move(*uploader);
        }
        return entry;
    }

    DeviceEntry::~DeviceEntry() noexcept = default;

    ResourceCreateContext DeviceEntry::resourceCreateContext() noexcept
    {
        return {session(), uploader(), frame_progress_.frameCount(), resources_.registry(), &frame_progress_};
    }

    bool DeviceEntry::nativeIdle() const noexcept
    {
        return native_->idle();
    }
} // namespace lux::render::detail
