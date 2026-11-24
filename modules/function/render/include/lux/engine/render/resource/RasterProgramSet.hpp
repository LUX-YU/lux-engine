#pragma once

#include <lux/engine/render/graph/Work.hpp>
#include <exec/task.hpp>
#include <memory>

namespace lux::render::detail { class DeviceEntry; }

namespace lux::render
{
    namespace runtime_detail { struct JobExecution; }
    struct ResourceCreateContext;

    // A resource owns a device-specific set of ready raster programs. Its command awaits replace() on the
    // device owner. Frame preparation can select the last successful set while a replacement is compiling.
    class LUX_RENDER_PUBLIC RasterProgramSet final
    {
    public:
        struct Description final
        {
            std::vector<RasterPassTemplate::Input> arguments;
            std::vector<rdesc::ETextureFormat> colors;
            rdesc::ETextureFormat depth = rdesc::ETextureFormat::UNDEFINED;
            std::uint32_t samples = 1;
            bool section_depth_bias = false;
            bool shared_textures = false;
            bool shared_records = false;
            std::vector<rdesc::ShaderProgramArtifact> shaders;
            // Each entry indexes shaders and specifies its graphics state, parameters and geometry locations.
            std::vector<RasterPassTemplate::StreamProgram> programs;
        };

        explicit RasterProgramSet(ResourceCreateContext&) noexcept;
        ~RasterProgramSet() noexcept;
        RasterProgramSet(const RasterProgramSet&) = delete;
        RasterProgramSet& operator=(const RasterProgramSet&) = delete;

        [[nodiscard]] exec::task<RenderResult<void>> replace(Description) noexcept;

    private:
        friend struct runtime_detail::JobExecution;
        struct Table;
        detail::DeviceEntry& device_;
        std::unique_ptr<const Table> current_;
        std::uint64_t revision_ = 0;
        bool preparing_ = false;
    };
} // namespace lux::render
