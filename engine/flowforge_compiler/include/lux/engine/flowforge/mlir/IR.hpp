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
//   * BuildTimeError — thrown by MLIRBuilder::generateIR for graph-level
//                      problems (missing entry node, dangling links, etc.).
//
// The IRImpl class declared below is forward-only; consumers that need to
// reach into the wrapped mlir::ModuleOp should include the matching
// IRImpl.hpp from pinclude/.
//
#include <string>
#include <memory>
#include <stdexcept>
#include <lux/engine/editor/visibility.h>

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
    class LUX_EDITOR_PUBLIC IR
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

    // Thrown by MLIRBuilder::generateIR when a graph-level constraint fails
    // (no entry node, multi-entry, unlinked required pin, unsupported op
    // kind, ...). `nodePtr()` / `pinPtr()` carry the offending Node / Pin
    // address as opaque uintptrs so the editor UI can highlight them; a
    // future stable-ID scheme will replace these.
    //
    // LUX_EDITOR_PUBLIC matters: without it the type's RTTI is per-DLL on
    // MSVC and `catch (const BuildTimeError&)` from outside the compiler
    // DLL only triggers via `catch (...)`.
    class LUX_EDITOR_PUBLIC BuildTimeError : public std::runtime_error
    {
    public:
        BuildTimeError(const std::string& msg, uintptr_t node_ptr, uintptr_t pin_ptr)
            : std::runtime_error(msg), node_ptr_(node_ptr), pin_ptr_(pin_ptr){}

        uintptr_t nodePtr() const { return node_ptr_; }
        uintptr_t pinPtr()  const { return pin_ptr_; }

    private:
        uintptr_t node_ptr_;
        uintptr_t pin_ptr_;
    };

    class LUX_EDITOR_PUBLIC IRContext
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
    class LUX_EDITOR_PUBLIC MLIRBuilder
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

        // throws BuildTimeError on graph-level errors.
        std::unique_ptr<IR> generateIR(const FlowGraph& graph) noexcept(false);

    private:
        std::unique_ptr<MLIRBuilderImpl> impl_;
    };
} // namespace lux::flowforge
