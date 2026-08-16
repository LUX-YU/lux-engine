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
#include <string>
#include <memory>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/toolchain/flowforge/visibility.h>

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
    class LUX_TOOLCHAIN_FLOWFORGE_PUBLIC IR
    {
    public:
        IR();
        ~IR();
        std::string toString() const;

        IRImpl& impl();
        const IRImpl& impl() const;

    private:
        std::unique_ptr<IRImpl> impl_;
    };

    enum class EFlowForgeError
    {
        GRAPH_INVALID,
        IR_VERIFICATION_FAILED,
        LOWERING_FAILED,
        JIT_ENGINE_CREATION_FAILED,
        JIT_SYMBOL_LOOKUP_FAILED,
        JIT_INVOCATION_FAILED,
    };

    struct FlowForgeFailure
    {
        EFlowForgeError code = EFlowForgeError::GRAPH_INVALID;
        std::string     message;
        std::uint64_t   node_id = 0;
        std::uint64_t   pin_id = 0;
    };

    template<class T>
    using FlowForgeResult = lux::cxx::expected<T, FlowForgeFailure>;

    // Owns the MLIRContext every IR produced through it lives in. Lifetime
    // contract: any IR built via an MLIRBuilder(this) must be destroyed
    // BEFORE this IRContext — the context's destructor does not know about
    // outstanding modules, so a surviving IR would dangle.
    class LUX_TOOLCHAIN_FLOWFORGE_PUBLIC IRContext
    {
    public:
        IRContext();
        ~IRContext();

        IRContext(const IRContext&) = delete;
        IRContext& operator=(const IRContext&) = delete;
        IRContext(IRContext&& other) noexcept;
        IRContext& operator=(IRContext&& other) noexcept;

        void* context() noexcept { return context_; }
        const void* context() const noexcept { return context_; }

    private:
        void* context_ = nullptr;
    };

    class MLIRBuilderImpl;
    class LUX_TOOLCHAIN_FLOWFORGE_PUBLIC MLIRBuilder
    {
    public:
        MLIRBuilder(IRContext* context);
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
        generateIR(const FlowGraph& graph);

    private:
        std::unique_ptr<MLIRBuilderImpl> impl_;
    };
} // namespace lux::flowforge
