#pragma once
//
// IR.hpp — Public API surface of the FlowForge MLIR compiler.
//
// This header is the only one consumers (editor, build tool, tests) need to
// include. It exposes:
//   * IRContext      — owns an MLIRContext + loaded dialects.
//   * MLIRBuilder    — turns a FlowGraph into an IR.
//   * IR             — opaque IR handle; pass to Passes.hpp helpers to lower
//                      to SCF / LLVM dialect or JIT-execute.
//   * FlowForgeFailure — structured graph/compiler failure.
//
// The IRImpl class declared below is forward-only; consumers that need to
// reach into the wrapped mlir::ModuleOp should include the matching
// IRImpl.hpp from pinclude/.
//
#include <memory>
#include <string>

#include <lux/engine/flowforge/Compiler.hpp>

namespace lux::flowforge
{
    class FlowGraph;
    class Node;
    class ExecSourceNode;
    class ExecIntermediateNode;
    class DataInPin;
    class DataOutPin;
    class ExecOutPin;

    class IRImpl;
    class IR
    {
    public:
        IR();
        ~IR();
        [[nodiscard]] FlowForgeResult<std::string> toString() const noexcept;

        IRImpl& impl();
        const IRImpl& impl() const;

    private:
        std::unique_ptr<IRImpl> impl_;
    };

    // Owns the MLIRContext every IR produced through it lives in. Lifetime
    // contract: any IR built via an MLIRBuilder(this) must be destroyed
    // BEFORE this IRContext — the context's destructor does not know about
    // outstanding modules, so a surviving IR would dangle.
    class IRContext
    {
    public:
        [[nodiscard]] static FlowForgeResult<IRContext> create() noexcept;

        ~IRContext();

        IRContext(const IRContext&) = delete;
        IRContext& operator=(const IRContext&) = delete;
        IRContext(IRContext&& other) noexcept;
        IRContext& operator=(IRContext&& other) noexcept;

        void* context() noexcept { return context_; }
        const void* context() const noexcept { return context_; }

    private:
        explicit IRContext(void* context) noexcept : context_(context)
        {
        }

        void* context_ = nullptr;
    };

    class MLIRBuilderImpl;
    class MLIRBuilder
    {
    public:
        [[nodiscard]] static FlowForgeResult<MLIRBuilder> create(IRContext& context) noexcept;

        ~MLIRBuilder();

        MLIRBuilder(const MLIRBuilder&) = delete;
        MLIRBuilder& operator=(const MLIRBuilder&) = delete;
        // Move members are out-of-line (defined in IR.cpp where
        // MLIRBuilderImpl is complete). Inline `= default` would force the
        // compiler to instantiate unique_ptr<MLIRBuilderImpl>'s destructor
        // at the point of the class definition where MLIRBuilderImpl is
        // only a forward declaration -> "can't delete an incomplete type".
        MLIRBuilder(MLIRBuilder&&) noexcept;
        MLIRBuilder& operator=(MLIRBuilder&&) noexcept;

        [[nodiscard]] FlowForgeResult<std::unique_ptr<IR>>
        generateIR(const FlowGraph& graph) noexcept;

    private:
        explicit MLIRBuilder(std::unique_ptr<MLIRBuilderImpl> impl) noexcept;

        std::unique_ptr<MLIRBuilderImpl> impl_;
    };
} // namespace lux::flowforge
