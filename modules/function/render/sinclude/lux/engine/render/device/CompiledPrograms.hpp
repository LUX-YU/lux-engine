#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::rdesc { struct ShaderProgramArtifact; }

namespace lux::render::detail
{
    class DeviceSession;
    struct PassBindings;
    struct RasterProgramKey;

    // Device-owned native programs. All directory/refcount operations run on RenderThread;
    // workers only borrow an acquired Layout while compiling an unpublished candidate.
    class LUX_RENDER_PUBLIC CompiledPrograms final
    {
    public:
        struct Program;
        struct Layout;
        struct Shader;

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
            Shader* shader_ = nullptr;
            Program* hit_ = nullptr;
        };

        CompiledPrograms(DeviceSession&, std::uint32_t unused_limit) noexcept;
        ~CompiledPrograms() noexcept;
        CompiledPrograms(const CompiledPrograms&) = delete;
        CompiledPrograms& operator=(const CompiledPrograms&) = delete;

        [[nodiscard]] RenderResult<Layout*> acquireLayout(PassBindings&, std::uint32_t color_count = 0) noexcept;
        void releaseLayout(Layout*) noexcept;

        [[nodiscard]] Shader& acquireShader(rdesc::ShaderProgramArtifact) noexcept;
        void releaseShader(Shader&) noexcept;
        [[nodiscard]] static const rdesc::ShaderProgramArtifact& artifact(const Shader&) noexcept;

        [[nodiscard]] Lookup lookup(Shader&, Layout&, const RasterProgramKey* raster) noexcept;
        [[nodiscard]] Program* publishRaster(RasterProgramKey, const Lookup&, nri::Pipeline*) noexcept;
        [[nodiscard]] Program* publishCompute(const Lookup&, nri::Pipeline*) noexcept;

        [[nodiscard]] static nri::Pipeline* native(const Program&) noexcept;

        // Caller has stopped recording. The cache retains actual accepted GPU queue frontiers.
        void release(std::span<Program* const>) noexcept;

        // Before cleanup_scope_ closes; no logical users or CPU compilation borrows may remain.
        void clear() noexcept;

    private:
        [[nodiscard]] Program* find(const RasterProgramKey*, const Lookup&, std::uint64_t after) noexcept;
        [[nodiscard]] Program* publish(RasterProgramKey*, const Lookup&, nri::Pipeline*) noexcept;
        void dropLayout(Layout&) noexcept;
        void trim() noexcept;

        DeviceSession& session_;
        std::unordered_map<std::size_t, std::vector<std::unique_ptr<Program>>> program_index_;
        std::map<std::uint64_t, Program*> unused_;
        std::unordered_map<std::uint64_t, std::unique_ptr<Layout>> layouts_;
        std::unordered_multimap<std::size_t, Layout*> layout_index_;
        std::unordered_map<std::uint64_t, std::unique_ptr<Shader>> shaders_;
        std::unordered_multimap<std::size_t, Shader*> shader_index_;
        std::uint64_t layout_serial_ = 0, shader_serial_ = 0;

        std::uint32_t unused_limit_;
        std::uint64_t acquisition_serial_ = 0, publication_serial_ = 0;
    };
} // namespace lux::render::detail
