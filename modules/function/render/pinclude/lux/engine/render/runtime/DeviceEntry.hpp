#pragma once

#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <lux/engine/render/runtime/DeviceRound.hpp>
#include <lux/engine/render/runtime/JobOutput.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <map>

namespace lux::render::detail
{
    class NativeExecution;

    // One actual device. Server settles accepted operations and GPU work before releasing
    // its child domains and this entry. The native worker outlives Session cleanup.
    class DeviceEntry final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceEntry>> create(
            ResourceRegistry&,
            const DeviceOptions&,
            const nri::CallbackInterface&,
            const RenderServerConfig&,
            execution::CpuScheduler
        ) noexcept;
        ~DeviceEntry() noexcept;
        DeviceEntry(const DeviceEntry&) = delete;
        DeviceEntry& operator=(const DeviceEntry&) = delete;

        [[nodiscard]] DeviceSession& session() const noexcept { return *session_; }
        [[nodiscard]] ResourceUploader* uploader() const noexcept { return uploader_.get(); }
        [[nodiscard]] ResourceSet& resources() noexcept { return resources_; }
        [[nodiscard]] FrameProgress& frameProgress() noexcept { return frame_progress_; }
        [[nodiscard]] const FrameProgress& frameProgress() const noexcept { return frame_progress_; }
        [[nodiscard]] FrameOperation::Workspace& frameWorkspace() noexcept { return frame_workspace_; }
        [[nodiscard]] ResourceCreateContext resourceCreateContext() noexcept;
        [[nodiscard]] bool nativeIdle() const noexcept;
        [[nodiscard]] ResourceSet* scopedResources(SceneId, ViewId, bool create) noexcept;
        [[nodiscard]] bool scopedResourceBorrows(SceneId = {}, ViewId = {}) const noexcept;
        void eraseResourceScope(SceneId, ViewId = {}) noexcept;
    private:
        DeviceEntry(
            ResourceRegistry&,
            std::unique_ptr<NativeExecution>,
            std::unique_ptr<DeviceSession>,
            std::uint32_t frame_count,
            std::size_t preparation_capacity
        ) noexcept;

        std::unique_ptr<NativeExecution> native_;
        std::unique_ptr<DeviceSession> session_;
        std::unique_ptr<ResourceUploader> uploader_;
        FrameProgress frame_progress_;
        FrameOperation::Workspace frame_workspace_;
        ResourceSet resources_;
        using ScopeKey = std::pair<std::uint32_t, std::uint32_t>;
        struct SceneResources final
        {
            explicit SceneResources(ResourceSet& parent) noexcept : resources(parent, EResourceDomain::SCENE) {}
            ResourceSet resources;
            std::map<ScopeKey, ResourceSet> views;
        };
        std::map<ScopeKey, SceneResources> scene_resources_;

    public:
        DeviceSessionId id;
        runtime_detail::DeviceRound round;
        cxx::SlotKeySparseSet<TargetId, JobOutput> outputs;
        RenderError failure;

        [[nodiscard]] RenderError admissionError() const noexcept
        {
            if (retiring) return {kDeviceBusy};
            return failure.type ? failure : session().terminalError();
        }

        [[nodiscard]] bool hasBorrows() const noexcept;

        bool retiring = false;
        bool upload_pending = false;
        std::uint32_t pending_starts = 0;
    };
} // namespace lux::render::detail
