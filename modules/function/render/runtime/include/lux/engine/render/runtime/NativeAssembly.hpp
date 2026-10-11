#pragma once

#include <lux/engine/render/runtime/Types.hpp>
#include <lux/engine/render/vulkan/graph/Executable.hpp>

namespace lux::render
{
    namespace detail
    {
        struct RuntimeBackendState;
    }
    class RenderRuntime;
    class RenderRuntimeBuilder;

    enum class ERuntimeFrameValue
    {
        CAMERA_FLOAT,
        RENDER_SECONDS,
        HISTORY_EPOCH
    };

    // Cold binding to an existing generated scalar field. No new schema or
    // per-frame name lookup; camera_element applies only to CAMERA_FLOAT.
    struct RuntimeFrameField
    {
        GraphPassId pass;
        std::uint32_t scalar_field;
        ERuntimeFrameValue value;
        std::uint32_t camera_element{};
    };

    // Cold assembly consumes the already validated F2/F3 values. There is no
    // second Shader schema, per-frame compiler, or callback-based Feature API.
    struct RuntimePathSource
    {
        LogicalGraphPlan logical;
        std::vector<vulkan::NativeShaderProgram> programs;
        std::vector<vulkan::NativePassCommand> commands;
        GraphInvocationData initial_values;
        std::vector<vulkan::NativeImportBinding> imports;
        std::vector<RuntimeFrameField> frame_fields;
    };

    struct RuntimeNativeOptions
    {
        vulkan::InstanceOptions instance;
        vulkan::DeviceOptions device;
        RuntimeCapacity capacity;
        bool presentation{false};
    };

    // Native authority and its bounded Scene/View/Target/cache members. Assembly
    // borrows end at Runtime handoff; thereafter only the backend thread calls it.
    // Public assembly is intentionally separate from the neutral Runtime header.
    class VulkanBackend
    {
    public:
        ~VulkanBackend() noexcept;
        VulkanBackend(const VulkanBackend&) = delete;
        VulkanBackend& operator=(const VulkanBackend&) = delete;

        [[nodiscard]] const vulkan::VulkanDevice& device() const noexcept;
        [[nodiscard]] const vulkan::VulkanAllocator& allocator() const noexcept;
        [[nodiscard]] RenderResult<std::reference_wrapper<const vulkan::Buffer>> retain(vulkan::Buffer buffer) noexcept;
        [[nodiscard]] RenderResult<std::reference_wrapper<const vulkan::Image>> retain(vulkan::Image image) noexcept;
        [[nodiscard]] RenderResult<GraphSampler> retain(vulkan::Sampler sampler) noexcept;
        [[nodiscard]] RenderResult<RuntimePath> addPath(RuntimeTarget target, RuntimePathSource source) noexcept;
        [[nodiscard]] RenderResult<RuntimeScene> addScene() noexcept;
        [[nodiscard]] RenderResult<RuntimeTarget> addOffscreenTarget(
            std::uint32_t width,
            std::uint32_t height
        ) noexcept;
        // Win32 window/instance are host borrows that outlive the Runtime.
        [[nodiscard]] RenderResult<RuntimeTarget> addWindowTarget(
            void* instance,
            void* window,
            std::uint32_t width,
            std::uint32_t height
        ) noexcept;
        [[nodiscard]] RenderResult<RuntimeView> addView(
            RuntimeScene scene,
            RuntimeTarget target,
            RuntimePath path,
            GraphInvocationData values,
            std::span<const vulkan::NativeImportBinding> history = {},
            RuntimeOutputPlacement placement = {}
        ) noexcept;
        // Explicit reuse of a completed View result on another output Target;
        // this does not re-execute target-dependent Graph work for that Target.
        [[nodiscard]] RenderResult<void> reuseOutput(
            RuntimeView view,
            RuntimeTarget target,
            RuntimeOutputPlacement placement = {}
        ) noexcept;

    private:
        friend class RenderRuntime;
        friend class RenderRuntimeBuilder;
        explicit VulkanBackend(std::unique_ptr<detail::RuntimeBackendState> backing) noexcept;
        [[nodiscard]] static RenderResult<std::unique_ptr<VulkanBackend>> create(
            const RuntimeNativeOptions& options,
            std::uint64_t owner
        ) noexcept;
        std::unique_ptr<detail::RuntimeBackendState> backing_;
    };
} // namespace lux::render

#include <lux/engine/render/runtime/Runtime.hpp>

namespace lux::render
{
    // Short-lived cold assembly and unique handoff into a started Runtime.
    // No public Runtime exists before its transport/native/thread factory succeeds.
    class RenderRuntimeBuilder
    {
    public:
        [[nodiscard]] static RenderResult<RenderRuntimeBuilder> create(
            const RuntimeNativeOptions& native,
            RuntimeOptions options = {}
        ) noexcept;
        ~RenderRuntimeBuilder() noexcept;
        RenderRuntimeBuilder(RenderRuntimeBuilder&&) noexcept;
        RenderRuntimeBuilder& operator=(RenderRuntimeBuilder&&) noexcept;
        RenderRuntimeBuilder(const RenderRuntimeBuilder&) = delete;
        RenderRuntimeBuilder& operator=(const RenderRuntimeBuilder&) = delete;
        [[nodiscard]] VulkanBackend& backend() noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<RenderRuntime>> finish() && noexcept;

    private:
        struct Assembly;
        explicit RenderRuntimeBuilder(std::unique_ptr<Assembly> assembly) noexcept;
        std::unique_ptr<Assembly> assembly_;
    };
} // namespace lux::render
