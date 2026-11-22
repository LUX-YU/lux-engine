#pragma once

#include <NRI.h>
#include <exec/task.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::rdesc { struct ShaderProgramArtifact; }

namespace lux::render::detail
{
    class DeviceSession;
    class NativeProgramBindings;
    struct PassBindings;
    struct ProgramCompileInput;
    struct PreparedProgram;
    struct RasterProgramKey;

    // Device-owned native programs. All directory/refcount operations run on RenderThread;
    // workers only borrow an acquired Layout while compiling an unpublished candidate.
    class LUX_RENDER_PUBLIC CompiledPrograms final
    {
    public:
        struct Layout;

        struct ProgramId final
        {
            std::size_t digest = 0;
            std::uint64_t publication = 0;

            [[nodiscard]] explicit operator bool() const noexcept { return publication != 0; }
            [[nodiscard]] bool operator==(const ProgramId&) const noexcept = default;
        };

        enum class LayoutId : std::uint64_t { INVALID };

        CompiledPrograms(DeviceSession&, std::uint32_t unused_limit) noexcept;
        ~CompiledPrograms() noexcept;
        CompiledPrograms(const CompiledPrograms&) = delete;
        CompiledPrograms& operator=(const CompiledPrograms&) = delete;

        [[nodiscard]] exec::task<RenderResult<PreparedProgram>>
        prepareEffectProgram(ProgramCompileInput, execution::CpuScheduler) noexcept;
        [[nodiscard]] exec::task<RenderResult<PreparedProgram>>
        prepareSelectedProgram(ProgramCompileInput, execution::CpuScheduler, stdexec::inplace_stop_token) noexcept;

        [[nodiscard]] RenderResult<Layout*> acquireLayout(PassBindings&, std::uint32_t color_count = 0) noexcept;
        void releaseLayout(Layout*) noexcept;

        [[nodiscard]] static LayoutId identity(const Layout&) noexcept;

        // Caller has stopped recording. The cache retains actual accepted GPU queue frontiers.
        void release(std::size_t count, cxx::function_ref<ProgramId(std::size_t) noexcept> get) noexcept;

        // Before cleanup_scope_ closes; no logical users or CPU compilation borrows may remain.
        void clear() noexcept;

    private:
        template <class PipelineScheduler>
        [[nodiscard]] exec::task<RenderResult<PreparedProgram>>
        compile(ProgramCompileInput, execution::CpuScheduler, PipelineScheduler, stdexec::inplace_stop_token) noexcept;

        struct Program;
        struct Shader;
        struct Lookup final
        {
            std::size_t digest = 0;
            std::uint64_t publication = 0;
            Layout* layout = nullptr;
            Shader* shader = nullptr;
            Program* hit = nullptr;
        };

        [[nodiscard]] Shader& acquireShader(rdesc::ShaderProgramArtifact) noexcept;
        void releaseShader(Shader&) noexcept;
        [[nodiscard]] Lookup lookup(Shader&, Layout&, const RasterProgramKey*) noexcept;

        friend class NativeProgramBindings;
        void releaseBindings(const NativeProgramBindings&) noexcept;

        [[nodiscard]] Program* find(const RasterProgramKey*, const Lookup&, std::uint64_t after) noexcept;
        [[nodiscard]] Program* publish(RasterProgramKey*, const Lookup&, nri::Pipeline*) noexcept;
        [[nodiscard]] Program& resolve(ProgramId) noexcept;
        void dropLayout(Layout&) noexcept;
        void dropLayout(LayoutId) noexcept;
        void trim() noexcept;

        DeviceSession& session_;
        std::unordered_map<std::size_t, std::vector<std::unique_ptr<Program>>> program_index_;
        std::map<std::uint64_t, Program*> unused_;
        std::unordered_map<LayoutId, std::unique_ptr<Layout>> layouts_;
        std::unordered_multimap<std::size_t, Layout*> layout_index_;
        std::unordered_map<std::uint64_t, std::unique_ptr<Shader>> shaders_;
        std::unordered_multimap<std::size_t, Shader*> shader_index_;
        std::uint64_t layout_serial_ = 0, shader_serial_ = 0;

        std::uint32_t unused_limit_;
        std::uint64_t acquisition_serial_ = 0, publication_serial_ = 0;
    };
} // namespace lux::render::detail
