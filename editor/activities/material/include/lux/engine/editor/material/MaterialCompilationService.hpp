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
            std::uint64_t environment = 1
        );
        [[nodiscard]] MaterialCompileResult<std::reference_wrapper<const MaterialCompileOperation>> operation(
            MaterialCompileId
        ) const noexcept;
        [[nodiscard]] MaterialCompileResult<void> acknowledge(MaterialCompileId);
        [[nodiscard]] MaterialCompileResult<void> cancel(MaterialCompileId) noexcept;
        // Release the caller's interest without cancelling accepted work. Idempotent after acknowledgement.
        [[nodiscard]] MaterialCompileResult<void> releaseResult(MaterialCompileId) noexcept;
        // Collects only the released, ready set observed on entry; cleanup-created work waits for another turn.
        [[nodiscard]] MaterialCompileResult<void> collectReleased();
        [[nodiscard]] MaterialCompileResult<std::vector<MaterialCompileId>> snapshotIds() const;
        [[nodiscard]] bool empty() const noexcept;


    private:
        const std::thread::id owner_{std::this_thread::get_id()};
        process::ExecutionRuntime& runtime_;
        std::size_t capacity_;
        struct Record final
        {
            std::unique_ptr<MaterialCompileOperation> operation;
            bool released{};
        };
        std::vector<Record> operations_;
    };
}
