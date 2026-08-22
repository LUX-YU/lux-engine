#pragma once
/**
 * @file FlowGraphCompile.hpp
 * @brief Typed editor execution protocol for compiling a FlowGraph snapshot.
 *
 * The protocol is deliberately separate from FlowGraphCompiler. Async
 * assembly depends only on this operation vocabulary; the compiler object
 * owns main-thread cache coordination and depends on the client value.
 */

#include "app/EditorAsyncTypes.hpp"

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/execution/AsyncOperation.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <memory>
#include <string>
#include <utility>

namespace lux::editor
{
    struct FlowGraphCompileJob;

    struct FlowGraphCompileResult final
    {
        lux::asset::asset_id_t asset{};
        std::string stem;
        std::string error;
        bool ok{false};
    };

    struct CompileFlowGraph final
    {
        using Value = FlowGraphCompileResult;
        using Error = EEditorAsyncError;

        std::shared_ptr<const FlowGraphCompileJob> job;
    };

    class FlowGraphCompileClient final
    {
    public:
        using Completion = lux::cxx::move_only_function<void(
            lux::async::OperationOutcome<CompileFlowGraph>)>;

        FlowGraphCompileClient() noexcept = default;

        [[nodiscard]] bool submit(
            CompileFlowGraph operation,
            Completion completion) const;

        [[nodiscard]] bool valid() const noexcept
        {
            return submit_ != nullptr;
        }

    private:
        using SubmitFn = bool(*)(void*, CompileFlowGraph, Completion);

        FlowGraphCompileClient(
            std::shared_ptr<void> owner,
            void* context,
            SubmitFn submit) noexcept
            : owner_(std::move(owner)),
              context_(context),
              submit_(submit)
        {
        }

        friend class EditorAsyncService;

        // Pins EditorAsyncService::State while a compiler still owns the
        // client. Admission itself is rejected once that state begins close.
        std::shared_ptr<void> owner_;
        void* context_{nullptr};
        SubmitFn submit_{nullptr};
    };

    /// Pure worker entry point. The job owns its graph and cache-directory
    /// snapshot; this function never touches AssetManager, ECS, UI or mutable
    /// main-thread state.
    [[nodiscard]] FlowGraphCompileResult compileFlowGraphJob(
        const FlowGraphCompileJob& job) noexcept;
}
