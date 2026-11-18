#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;
    struct PassBindings;
    struct RasterProgramKey;
    struct PreparedShader;

    // Device-owned native programs. All directory/refcount operations run on RenderThread;
    // workers only borrow an acquired Layout while compiling an unpublished candidate.
    class LUX_RENDER_PUBLIC CompiledPrograms final
    {
    public:
        struct Program;
        struct Layout;

        // One immutable key/qualified Layout pairing during lookup and native compilation.
        // The caller keeps the Layout borrowed through publication.
        class Lookup final
        {
        public:
            [[nodiscard]] Program* hit() const noexcept { return hit_; }

        private:
            friend class CompiledPrograms;
            std::size_t digest_ = 0;
            std::uint64_t publication_ = 0;
            Layout* layout_ = nullptr;
            Program* hit_ = nullptr;
        };

        CompiledPrograms(DeviceSession&, std::uint32_t unused_limit) noexcept;
        ~CompiledPrograms() noexcept;
        CompiledPrograms(const CompiledPrograms&) = delete;
        CompiledPrograms& operator=(const CompiledPrograms&) = delete;

        [[nodiscard]] RenderResult<Layout*> acquireLayout(PassBindings&, std::uint32_t color_count = 0) noexcept;
        void releaseLayout(Layout*) noexcept;

        [[nodiscard]] Lookup lookupRaster(const RasterProgramKey&, Layout&) noexcept;
        [[nodiscard]] Lookup lookupCompute(const std::shared_ptr<const PreparedShader>&, Layout&) noexcept;
        [[nodiscard]] Program* publishRaster(RasterProgramKey, const Lookup&, nri::Pipeline*) noexcept;
        [[nodiscard]] Program*
        publishCompute(std::shared_ptr<const PreparedShader>, const Lookup&, nri::Pipeline*) noexcept;
        [[nodiscard]] static nri::Pipeline* native(const Program&) noexcept;

        // Caller has stopped recording. The cache retains actual accepted GPU queue frontiers.
        void release(std::span<Program* const>) noexcept;

        // Before cleanup_scope_ closes; no logical users or CPU compilation borrows may remain.
        void clear() noexcept;

    private:
        template <class Key> Program* find(const Key&, const Lookup&, std::uint64_t after) noexcept;
        template <class Key> Lookup lookup(const Key&, Layout&) noexcept;
        template <class Key> Program* publish(Key, const Lookup&, nri::Pipeline*) noexcept;
        void dropLayout(Layout&) noexcept;
        void trim() noexcept;

        using ProgramIndex = std::unordered_map<std::size_t, std::vector<Program*>>;
        using LayoutIndex = std::unordered_multimap<std::size_t, std::unique_ptr<Layout>>;

        DeviceSession& session_;
        std::vector<std::unique_ptr<Program>> programs_;
        ProgramIndex program_index_;
        LayoutIndex layout_index_;
        std::uint32_t unused_limit_;
        std::uint64_t acquisition_serial_ = 0, publication_serial_ = 0;
    };
} // namespace lux::render::detail
