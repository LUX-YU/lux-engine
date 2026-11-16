#pragma once

#include <NRI.h>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;
    struct PassBindings;
    struct RasterProgramKey;

    // Device-owned native programs. All directory/refcount operations run on RenderThread;
    // workers only borrow an acquired Layout while compiling an unpublished candidate.
    class LUX_RENDER_PUBLIC CompiledPrograms final
    {
        struct Program;
        struct Layout;

    public:
        CompiledPrograms(DeviceSession&, std::uint32_t unused_limit) noexcept;
        ~CompiledPrograms() noexcept;
        CompiledPrograms(const CompiledPrograms&) = delete;
        CompiledPrograms& operator=(const CompiledPrograms&) = delete;

        [[nodiscard]] RenderResult<nri::PipelineLayout*>
        acquireLayout(PassBindings&, std::uint32_t color_count = 0) noexcept;
        // Only releases an unsubmitted CPU compilation borrow. GPU uses go through release().
        void releaseLayout(nri::PipelineLayout*) noexcept;
        [[nodiscard]] nri::Pipeline* acquireRaster(const RasterProgramKey&, PassBindings&) noexcept;
        [[nodiscard]] nri::Pipeline* acquireCompute(const rdesc::ShaderProgramArtifact&, PassBindings&) noexcept;
        // Takes the candidate Pipeline and one Layout borrow, including duplicate publication.
        [[nodiscard]] nri::Pipeline*
        publishRaster(RasterProgramKey, nri::PipelineLayout*, nri::Pipeline*) noexcept;
        [[nodiscard]] nri::Pipeline*
        publishCompute(rdesc::ShaderProgramArtifact, nri::PipelineLayout*, nri::Pipeline*) noexcept;
        // Caller has stopped recording. The cache retains actual accepted GPU queue frontiers.
        void
        release(std::span<nri::Pipeline* const>, std::span<nri::PipelineLayout* const> direct_layouts = {}) noexcept;
        // Before cleanup_scope_ closes; no logical users or CPU compilation borrows may remain.
        void clear() noexcept;

    private:
        [[nodiscard]] Layout layoutKey(const PassBindings&) const noexcept;
        template <class Key> nri::Pipeline* acquire(const Key&, PassBindings&) noexcept;
        template <class Key> nri::Pipeline* publish(Key, nri::PipelineLayout*, nri::Pipeline*) noexcept;
        void dropLayout(Layout&) noexcept;
        void trim() noexcept;

        DeviceSession& session_;
        std::vector<std::unique_ptr<Program>> programs_;
        std::vector<std::unique_ptr<Layout>> layouts_;
        std::unordered_map<std::size_t, std::vector<Program*>> program_index_;
        std::unordered_map<std::size_t, std::vector<Layout*>> layout_index_;
        std::uint32_t unused_limit_;
        std::uint64_t acquisition_serial_ = 0;
    };
} // namespace lux::render::detail
