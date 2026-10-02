#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>

namespace lux::editor::material
{
    // Finite operation ownership. Task execution/completion and preview adoption retain their original owners.
    class MaterialCompilationService final
    {
    public:
        explicit MaterialCompilationService(process::ExecutionRuntime&, std::size_t capacity = 16);
        ~MaterialCompilationService();
        MaterialCompilationService(const MaterialCompilationService&) = delete;
        MaterialCompilationService& operator=(const MaterialCompilationService&) = delete;
        MaterialCompilationService(MaterialCompilationService&&) = delete;
        MaterialCompilationService& operator=(MaterialCompilationService&&) = delete;
        [[nodiscard]] MaterialCompileResult<MaterialCompileId> start(
            MaterialSnapshot,
            MaterialCompileSettings = {},
            std::uint64_t environment = 1,
            std::uint64_t target = 1
        );
        [[nodiscard]] MaterialCompileResult<std::reference_wrapper<const MaterialCompileOperation>> operation(
            MaterialCompileId
        ) const noexcept;
        [[nodiscard]] MaterialCompileResult<void> acknowledge(MaterialCompileId);
        [[nodiscard]] MaterialCompileResult<void> cancel(MaterialCompileId) noexcept;
        // Fixed owner-thread identities, including operations whose initiating view has disappeared.
        // The application may acknowledge ready unreferenced results; enumeration never cancels work.
        [[nodiscard]] MaterialCompileResult<std::vector<MaterialCompileId>> snapshotIds() const;

    private:
        const std::thread::id owner_{std::this_thread::get_id()};
        process::ExecutionRuntime& runtime_;
        std::size_t capacity_;
        std::vector<std::unique_ptr<MaterialCompileOperation>> operations_;
    };
}
