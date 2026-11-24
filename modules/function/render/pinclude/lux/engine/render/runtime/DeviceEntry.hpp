#pragma once

#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <lux/engine/render/runtime/DeviceRound.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <map>
#include <lux/engine/render/resource/view/SceneViewStorage.hpp>

namespace lux::render::runtime_detail { struct RenderJob; }

namespace lux::render::detail
{
    class NativeExecution;
    class JobGpuResources;


    // The session and its workers outlive all native uses on this device.
    class DeviceEntry final
    {
    public:
        using JobTable = cxx::StableSlotMap<runtime_detail::RenderJob, RenderJobTag>;

        struct Host final
        {
            const RenderServerConfig& config;
            const nri::CallbackInterface& callbacks;
            RecordingWorkers& workers;
            const JobTable& jobs;
        };

        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceEntry>>
        create(ResourceRegistry&, const DeviceOptions&, Host) noexcept;
        ~DeviceEntry() noexcept;
        DeviceEntry(const DeviceEntry&) = delete;
        DeviceEntry& operator=(const DeviceEntry&) = delete;

        [[nodiscard]] RecordingWorkers& workers() const noexcept { return workers_; }
        [[nodiscard]] DeviceSession& session() const noexcept { return *session_; }
        [[nodiscard]] ResourceUploader* uploader() const noexcept { return uploader_.get(); }
        [[nodiscard]] ResourceSet& resources() noexcept { return resources_; }
        [[nodiscard]] FrameProgress& frameProgress() noexcept { return frame_progress_; }
        [[nodiscard]] const FrameProgress& frameProgress() const noexcept { return frame_progress_; }
        [[nodiscard]] ResourceCreateContext resourceCreateContext() noexcept;
        [[nodiscard]] std::optional<exec::task<RenderResult<void>>> startReadyUploads() noexcept;
        [[nodiscard]] bool nativeIdle() const noexcept;
        [[nodiscard]] std::unique_ptr<JobGpuResources> prepareJobResources(ExecutionBindingId) noexcept;
        [[nodiscard]] std::unique_ptr<JobGpuResources>
        installJobResources(RenderJobId, std::unique_ptr<JobGpuResources>) noexcept;
        [[nodiscard]] JobGpuResources& jobResources(ExecutionBindingId) & noexcept;
        [[nodiscard]] std::unique_ptr<JobGpuResources> takeJobResources(ExecutionBindingId) noexcept;
        void installEffectResources(EffectId, std::span<BoundConstantBuffer>) noexcept;
        void removeEffectResources(EffectId) noexcept;
        [[nodiscard]] ResourceSet* scopedResources(SceneId, ViewId, ResourceCreateContext* creation) noexcept;
        [[nodiscard]] bool scopedResourceBorrows(SceneId = {}, ViewId = {}) const noexcept;
        void eraseSceneResources(SceneId) noexcept;
        [[nodiscard]] SubmissionFrontiers retireViewResources(SceneId, ViewId) noexcept;
    private:
        DeviceEntry(
            ResourceRegistry&, std::unique_ptr<NativeExecution>, std::unique_ptr<DeviceSession>, Host
        ) noexcept;

        const JobTable& jobs_;
        RecordingWorkers& workers_;
        std::unique_ptr<NativeExecution> native_;
        std::unique_ptr<DeviceSession> session_;
        std::unique_ptr<ResourceUploader> uploader_;
        FrameProgress frame_progress_;
        ResourceSet resources_;
        cxx::SlotKeySparseSet<RenderJobId, std::unique_ptr<JobGpuResources>> job_resources_;
        using EffectBuffers = std::vector<std::unique_ptr<ConstantBufferStorage>>;
        cxx::SlotKeySparseSet<EffectId, EffectBuffers> effect_buffers_;
        using ScopeKey = std::pair<std::uint32_t, std::uint32_t>;
        struct SceneResources final
        {
            SceneResources(ResourceSet& parent, DeviceSession& device, std::uint32_t frames) noexcept
                : view_table(device, frames), resources(parent, EResourceDomain::SCENE) {}
            SceneViewStorage view_table;
            ResourceSet resources;
            std::map<ScopeKey, ResourceSet> views;
        };
        std::map<ScopeKey, SceneResources> scene_resources_;

    public:

        DeviceSessionId id;
        runtime_detail::DeviceRound round;
        RenderError failure;

        [[nodiscard]] RenderError admissionError() const noexcept
        {
            if (retiring) return {kDeviceBusy};
            return failure.type ? failure : session().terminalError();
        }

        [[nodiscard]] bool jobUsesResource(ResourceInstanceId) const noexcept;
        [[nodiscard]] bool hasBorrows() const noexcept;

        bool retiring = false;
        bool upload_pending = false;
    };
} // namespace lux::render::detail
