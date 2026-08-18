#pragma once
// ============================================================================
//  FlowForgeCompilerService — the editor's flow-graph cook/precompile service.
//
//  A FlowGraph is Authoring source data. This service compiles it into the
//  same native-module artifact a shipping cook emits; it is deliberately not
//  an IScriptBackend and cannot make an Authoring document executable inside
//  the Runtime Script variant.
//
//    graph asset ──content hash──▶ .lux/cache/flowforge/<name>_<hash>.dll
//                                  (+ .manifest sidecar: events + state recipe)
//        cache miss → submit a typed async compile
//        cache hit  → zero compile
//    dll + manifest ──▶ cooked NativeModuleScript artifact
//
//  lld-link missing / compile failure = LOUD failure, no JIT fallback
//  (per the user's ruling: no fallback for a half-finished mechanism). Cache
//  dlls are versioned by content hash, so stale generations can be retired.
//
//  Compiled to a loud no-op when the MLIR compiler is off
//  (LUX_FLOWFORGE_HAS_MLIR). Private editor header (src/script — not
//  installed).
// ============================================================================

#include <lux/engine/resource/asset/Asset.hpp>                 // asset_id_t
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/execution/AsyncOperation.hpp>
#include "app/EditorAsyncTypes.hpp"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    struct FlowForgeCompileJob;

    struct FlowForgeCompileResult final
    {
        std::string stem;
        std::string graph_id;
        std::string error;
        bool ok{false};
    };

    struct CompileFlowForgeOperation final
    {
        using Value = FlowForgeCompileResult;
        using Error = EEditorAsyncError;

        std::shared_ptr<const FlowForgeCompileJob> job;
    };

    class FlowForgeCompileClient final
    {
    public:
        using Completion = lux::cxx::move_only_function<void(
            lux::exec::AsyncOutcome<CompileFlowForgeOperation>)>;

        FlowForgeCompileClient() noexcept = default;

        [[nodiscard]] bool submit(
            CompileFlowForgeOperation operation,
            Completion completion) const;

        [[nodiscard]] bool valid() const noexcept
        {
            return submit_ != nullptr;
        }

    private:
        using SubmitFn = bool(*)(
            void*,
            CompileFlowForgeOperation,
            Completion);

        FlowForgeCompileClient(
            std::shared_ptr<void> owner,
            void* context,
            SubmitFn submit) noexcept
            : owner_(std::move(owner)), context_(context), submit_(submit)
        {
        }

        friend class EditorAsyncService;

        std::shared_ptr<void> owner_;
        void*                context_{nullptr};
        SubmitFn             submit_{nullptr};
    };

    /// Pure worker entry point. The job owns its graph and cache-directory
    /// snapshot; this function never touches AssetManager, ECS, UI or the
    /// backend's main-thread state.
    [[nodiscard]] FlowForgeCompileResult compileFlowForgeJob(
        const FlowForgeCompileJob& job) noexcept;

    class FlowForgeCompilerService final
    {
    public:
        /// @param cache_dir The AOT cache directory (…/.lux/cache/flowforge).
        ///                  Created on first use; empty = cache disabled and
        ///                  every graph-script Play fails loudly.
        /// @param compile_client The typed editor operation client. Its
        ///                  admission closes with EditorAsyncService.
        explicit FlowForgeCompilerService(
            std::filesystem::path cache_dir,
            FlowForgeCompileClient compile_client
        );
        
        ~FlowForgeCompilerService();

        /// 重指缓存目录(openProject → <工程根>/.lux/cache/flowforge)。主线程调;
        /// 在途的后台编译按取任务时的快照落旧目录(无害,冷缓存)。
        void setCacheDir(std::filesystem::path dir);

        /// MainThreadScheduler completion target. Updates only coordinator-derived
        /// main-thread bookkeeping; failed stems are remembered until content
        /// changes and therefore do not retry every frame.
        void adoptPrecompileResult(FlowForgeCompileResult result);

        /// Background precompile on save (main thread): hash the FLOW_GRAPH
        /// asset and queue a background compile+link on a cache miss. No-op on
        /// hit / unloadable asset / MLIR off.
        /// @param assets The manager to resolve @p id through (the panel's).
        void precompile(lux::asset::AssetManager& assets, const lux::asset::asset_id_t& id);

    private:
        friend FlowForgeCompileResult compileFlowForgeJob(const FlowForgeCompileJob& job) noexcept;

        struct Impl;                    // MLIR + cache guts — pimpl keeps the
        std::unique_ptr<Impl> impl_;    // compiler out of every includer
    };

} // namespace lux::editor
