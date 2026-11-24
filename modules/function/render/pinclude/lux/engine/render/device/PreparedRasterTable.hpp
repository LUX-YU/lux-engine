#pragma once

#include <lux/engine/render/device/ProgramCompileInput.hpp>

namespace lux::render::detail
{
    class DeviceEntry;

    // The asset candidate owns each native use as soon as its pipeline returns, including a partial set on failure.
    class PreparedRasterTable final
    {
    public:
        explicit PreparedRasterTable(DeviceEntry& device) noexcept : device_(device) {}
        ~PreparedRasterTable() noexcept;
        PreparedRasterTable(const PreparedRasterTable&) = delete;
        PreparedRasterTable& operator=(const PreparedRasterTable&) = delete;

        [[nodiscard]] exec::task<RenderResult<void>> compile(std::vector<RasterProgramLink>) noexcept;
        [[nodiscard]] std::span<const PublishedRasterProgram> programs() const & noexcept { return programs_; }
        std::span<const PublishedRasterProgram> programs() const && = delete;

    private:
        DeviceEntry& device_;
        std::vector<PublishedRasterProgram> programs_;
    };
} // namespace lux::render::detail
