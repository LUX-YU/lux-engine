#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/scene/RenderResources.hpp>

namespace lux::services
{
    struct ServiceDescriptor;
}

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
            lux::scene::RenderAssetInput assets = {}
        );
        [[nodiscard]] MaterialCompileResult<std::reference_wrapper<const MaterialCompileOperation>> operation(
            MaterialCompileId
        ) const noexcept;
        [[nodiscard]] MaterialCompileResult<void> acknowledge(MaterialCompileId);
        [[nodiscard]] MaterialCompileResult<void> cancel(MaterialCompileId) noexcept;
        // Windows observe results; only explicit acknowledgement discards an operation. Reopening uses
        // the full SessionId and the original asset read view, never today's mutable project version.
        [[nodiscard]] MaterialCompileResult<std::optional<MaterialCompileId>>
        latest(sessions::SessionId) const noexcept;
        [[nodiscard]] MaterialCompileResult<lux::scene::RenderAssetInput> assets(MaterialCompileId) const noexcept;
        [[nodiscard]] MaterialCompileResult<std::vector<MaterialCompileId>> snapshotIds() const;
        // A terminal task does not settle the service until its business completion is received.
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

    private:
        const std::thread::id owner_{std::this_thread::get_id()};
        process::ExecutionRuntime& runtime_;
        std::size_t capacity_;
        struct Record final
        {
            std::unique_ptr<MaterialCompileOperation> operation;
            lux::scene::RenderAssetInput assets;
        };
        std::vector<Record> operations_;
    };
    extern const services::ServiceDescriptor kMaterialCompilationService;
} // namespace lux::editor::material
