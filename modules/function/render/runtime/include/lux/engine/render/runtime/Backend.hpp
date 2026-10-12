#pragma once

#include <lux/engine/render/runtime/Runtime.hpp>

namespace lux::render
{
    namespace vulkan
    {
        struct BackendOptions;
        struct EffectSource;
        class VulkanDevice;
    } // namespace vulkan

    // Server is separately owned by the host. Client can safely outlive this object.
    class RenderBackend
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderBackend>> create(
            const vulkan::BackendOptions& native,
            RuntimeOptions options = {}
        ) noexcept;
        ~RenderBackend() noexcept;
        RenderBackend(const RenderBackend&) = delete;
        RenderBackend& operator=(const RenderBackend&) = delete;
        [[nodiscard]] RenderResult<std::unique_ptr<RenderRuntime>> start() noexcept;
        void requestStop() noexcept;
        [[nodiscard]] RenderResult<bool> waitStopped(std::uint32_t timeout_ms) noexcept;
        // Cold assembly only; every access is rejected after start.
        [[nodiscard]] RenderResult<RuntimeEffect> addEffect(vulkan::EffectSource source) noexcept;
        [[nodiscard]] RenderResult<void> removeEffect(RuntimeEffect effect) noexcept;
        [[nodiscard]] RenderResult<std::reference_wrapper<const vulkan::VulkanDevice>> assemblyDevice() const noexcept;

    private:
        struct Impl;
        explicit RenderBackend(std::unique_ptr<Impl> implementation) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
