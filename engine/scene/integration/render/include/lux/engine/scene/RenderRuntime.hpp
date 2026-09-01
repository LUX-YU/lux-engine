#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/scene/render/visibility.h>

#include <cstdint>

namespace lux::scene
{
    enum class ERenderRuntimeError : std::uint8_t
    {
        ACTIVATION_FAILURE,
        STOPPING,
        FEATURE_REGISTRATION_FAILURE,
    };

    struct RenderRuntimeFailure final
    {
        ERenderRuntimeError code{ERenderRuntimeError::ACTIVATION_FAILURE};
        render::RenderError render_error{};
    };

    class RenderRuntime;

    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderRuntimeLease final
    {
    public:
        RenderRuntimeLease() noexcept = default;
        ~RenderRuntimeLease() noexcept;

        RenderRuntimeLease(RenderRuntimeLease&& other) noexcept;
        RenderRuntimeLease& operator=(RenderRuntimeLease&& other) noexcept;
        RenderRuntimeLease(const RenderRuntimeLease&) = delete;
        RenderRuntimeLease& operator=(const RenderRuntimeLease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] render::RenderControlSession& control() noexcept;
        [[nodiscard]] render::RenderProgramSession& programs() noexcept;
        [[nodiscard]] render::RenderUploadClient upload() noexcept;
        [[nodiscard]] const render::FeatureCatalog& features() const noexcept;

    private:
        friend class RenderRuntime;
        explicit RenderRuntimeLease(RenderRuntime& owner) noexcept;
        void reset() noexcept;
        RenderRuntime* owner_{};
    };

    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderRuntime
    {
    public:
        virtual ~RenderRuntime() noexcept = default;

        [[nodiscard]] virtual lux::cxx::expected<RenderRuntimeLease, RenderRuntimeFailure>
        acquire() noexcept = 0;

    protected:
        friend class RenderRuntimeLease;

        [[nodiscard]] RenderRuntimeLease makeLease() noexcept;
        virtual void release() noexcept = 0;
        [[nodiscard]] virtual render::RenderControlSession& control() noexcept = 0;
        [[nodiscard]] virtual render::RenderProgramSession& programs() noexcept = 0;
        [[nodiscard]] virtual render::RenderUploadClient upload() noexcept = 0;
        [[nodiscard]] virtual const render::FeatureCatalog& features() const noexcept = 0;
    };
} // namespace lux::scene
