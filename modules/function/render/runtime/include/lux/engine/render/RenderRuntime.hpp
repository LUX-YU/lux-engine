#pragma once

#include <lux/engine/render/RendererConfig.hpp>

namespace lux::render
{
    enum class ERenderRuntimeState : std::uint8_t
    {
        ACTIVE,
        STOPPING,
        RETIRED
    };

    struct RenderRuntimeStatus final
    {
        ERenderRuntimeState state{ERenderRuntimeState::ACTIVE};
        RenderError error;
    };

    enum class EFeatureRegistrationState : std::uint8_t
    {
        IDLE,
        REGISTERING,
        READY,
        ROLLING_BACK,
        COMMITTED,
        FAILED,
        CANCELLED
    };

    struct FeatureRegistrationStatus final
    {
        EFeatureRegistrationState state{EFeatureRegistrationState::IDLE};
        RenderError error;
    };

    // Main owns admission and results; the dedicated backend owns Vulkan.
    // Feature factories are cold inputs, not a built-in UI/Scene implementation.
    using ValidationMessageSink = std::function<void(std::uint32_t, std::string_view)>;

    class LUX_RENDER_RUNTIME_PUBLIC RenderRuntime final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderRuntime>> create(
            RendererConfig config,
            ValidationMessageSink diagnostics = {}
        );
        ~RenderRuntime();
        RenderRuntime(const RenderRuntime&) = delete;
        RenderRuntime& operator=(const RenderRuntime&) = delete;

        [[nodiscard]] bool controlAvailable(std::size_t packets = 1) const noexcept;
        [[nodiscard]] RenderResult<std::reference_wrapper<RenderControlSession>> control() noexcept;
        [[nodiscard]] RenderResult<RenderUploadClient> upload() noexcept;
        [[nodiscard]] const FeatureCatalog& features() const noexcept;
        [[nodiscard]] RenderResult<void> beginFeatureRegistration(std::vector<RenderFeatureRegistration>);
        [[nodiscard]] FeatureRegistrationStatus featureRegistrationStatus() const noexcept;
        [[nodiscard]] RenderResult<void> commitFeatureRegistration();
        [[nodiscard]] RenderResult<void> cancelFeatureRegistration() noexcept;
        [[nodiscard]] RenderResult<EFrameSubmit> submit(TRenderProgram<>& input) noexcept;
        // One unit is a reply envelope, or one terminal callback after backend exit.
        [[nodiscard]] RenderResult<std::size_t> collectCompletions(std::size_t reply_limit);
        [[nodiscard]] RenderResult<void> submitPending(std::size_t& controls, std::size_t& programs);
        // Register on Main. The caller retains owner; expiry disconnects. Callbacks only request adoption.
        [[nodiscard]] RenderResult<void> bindProgress(std::shared_ptr<void>, void (*)(void*) noexcept) noexcept;
        // Backend fact used to settle retained resources; not a public close protocol.
        [[nodiscard]] RenderRuntimeStatus status() const noexcept;
        [[nodiscard]] RendererStatistics statistics() const noexcept;
        [[nodiscard]] RenderResult<std::optional<RendererDiagnostic>> takeDiagnostic() noexcept;

    private:
        [[nodiscard]] RenderResult<void> beginRetirement() noexcept;
        [[nodiscard]] RenderResult<bool> advanceRetirement(
            std::size_t& replies,
            std::size_t& controls,
            std::size_t& programs
        );
        struct Impl;
        explicit RenderRuntime(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
