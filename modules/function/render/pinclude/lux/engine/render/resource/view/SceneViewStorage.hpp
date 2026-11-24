#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/device/SubmissionFrontiers.hpp>
#include <NRI.h>
#include <map>
#include <lux/engine/render/resource/view/Parameters.hpp>

namespace lux::render::detail
{
    class DeviceSession;

    // One logical row belongs to the Scene for the lifetime of its ViewId, across all devices.
    class SceneViewRow final
    {
    public:
        [[nodiscard]] RenderResult<void> update(std::span<const ViewGpuParameters>) noexcept;
        [[nodiscard]] const std::uint64_t& revision() const & noexcept { return revision_; }
        [[nodiscard]] ResourceViewValue<ViewGpuParameters> parameters() const & noexcept { return {value_, revision_}; }

    private:
        ViewGpuParameters value_;
        std::uint64_t revision_ = 1;
    };

    // Each device's Scene scope owns its native table. Pages have stable addresses as other Views are created.
    class SceneViewStorage final
    {
    public:
        static constexpr std::uint32_t rows_per_page = 64;
        using Buffers = std::array<nri::Buffer*, 3>;
        struct Page final
        {
            Buffers buffers{};
            std::array<SubmissionFrontiers, rows_per_page> uses;
        };

        SceneViewStorage(DeviceSession& device, std::uint32_t frames) noexcept : device_(device), frames_(frames) {}
        ~SceneViewStorage() noexcept;
        SceneViewStorage(const SceneViewStorage&) = delete;
        SceneViewStorage& operator=(const SceneViewStorage&) = delete;

        [[nodiscard]] RenderResult<Page*> page(std::uint32_t view_index) noexcept;
        [[nodiscard]] SubmissionFrontiers takePendingUses(std::uint32_t view_index) noexcept;
        void releasePage(std::uint32_t page_index) noexcept;

    private:
        void retire(const Buffers&) noexcept;
        DeviceSession& device_;
        std::uint32_t frames_;
        std::map<std::uint32_t, Page> pages_;
    };

} // namespace lux::render::detail
