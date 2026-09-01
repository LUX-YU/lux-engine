#pragma once

#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>

#include <cstddef>
#include <span>

namespace lux::rendertest
{
    class DeviceRenderFixture;
}

namespace lux::scene::qualification
{
    class QualificationRenderRuntime final : public RenderRuntime
    {
    public:
        struct CreateInfo final
        {
            rendertest::DeviceRenderFixture& fixture;
            const SceneMetaManager& meta;
            std::span<const render::FeatureFactory* const> factories;
        };

        explicit QualificationRenderRuntime(CreateInfo info) noexcept;

        [[nodiscard]] lux::cxx::expected<RenderRuntimeLease, RenderRuntimeFailure>
        acquire() noexcept override;

        [[nodiscard]] std::size_t leaseCount() const noexcept;
        [[nodiscard]] bool catalogReady() const noexcept;

    private:
        void release() noexcept override;
        render::RenderControlSession& control() noexcept override;
        render::RenderProgramSession& programs() noexcept override;
        render::RenderUploadClient upload() noexcept override;
        const render::FeatureCatalog& features() const noexcept override;

        rendertest::DeviceRenderFixture* fixture_{};
        const SceneMetaManager* meta_{};
        std::span<const render::FeatureFactory* const> factories_{};
        render::FeatureCatalog catalog_;
        std::size_t lease_count_{};
        bool catalog_ready_{};
    };
} // namespace lux::scene::qualification
