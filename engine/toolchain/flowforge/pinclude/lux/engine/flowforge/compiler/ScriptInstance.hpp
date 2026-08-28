#pragma once
//
// ScriptInstance.hpp — FlowScriptInstance, the JIT execution host for a
// compiled FlowGraph.
//
// A graph compiles into one module with an entry func.func per OnEvent node
// (symbol `lux_event_<sanitized name>`, emitted with llvm.emit_c_interface
// so it is callable through the packed/ciface convention regardless of its
// signature). The engine drives the script by name:
//
//     auto script = FlowScriptInstance::compile(ctx, graph, host_symbols);
//     void* args[] = { &player_ptr, &dt };
//     script->invoke("Tick", args);
//
// Reflected native calls (NativeFuncCall nodes whose RefInvokable carries a
// type-erased invoker trampoline) are bound automatically: the IR builder
// emits a call to `_lfi_<hash>` with the invoker ABI
// (void(void* obj, void** args, void* ret)) and compile() registers every
// reflected function's trampoline address under that symbol. Hand-written
// C-ABI hosts (the editor's Print / Draw Line) still pass through
// `extra_symbols` exactly like runMainJIT.
//
// Instance state: graph variables live in a block OWNED BY THIS INSTANCE
// (not in the compiled binary — see StateLayout.hpp). Every generated
// function takes the block's base pointer as a hidden leading argument,
// which invoke() supplies automatically. One FlowScriptInstance therefore
// IS one script instance: compile the same graph twice and the two
// instances' variables are fully independent.
//
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Passes.hpp"   // JitNativeSymbol

namespace mlir { class ExecutionEngine; }
namespace lux::meta { struct RefInvokable; }

namespace lux::flowforge
{
    class FlowGraph;
    class IR;
    class IRContext;

    class FlowScriptInstance
    {
    public:
        /// Compile graph -> LLVM -> JIT engine. The instance must not outlive
        /// `context` because the module lives in its MLIRContext.
        [[nodiscard]] static FlowForgeResult<std::unique_ptr<FlowScriptInstance>> compile(
            IRContext& context,
            const FlowGraph& graph,
            std::vector<JitNativeSymbol> extra_symbols = {}
        ) noexcept;

        ~FlowScriptInstance();
        FlowScriptInstance(const FlowScriptInstance&) = delete;
        FlowScriptInstance& operator=(const FlowScriptInstance&) = delete;

        [[nodiscard]] bool        hasEvent(std::string_view event) const;
        [[nodiscard]] std::size_t eventCount() const { return events_.size(); }

        /// Invoke an event entry. args[i] points at the STORAGE of the i-th
        /// payload parameter (packed convention: `&some_ptr` for a pointer
        /// parameter, `&some_float` for a scalar). The argument count must
        /// match the event's signature. The instance-state pointer is
        /// prepended internally — callers never pass it.
        [[nodiscard]] FlowForgeResult<void> invoke(
            std::string_view event,
            std::span<void* const> args
        ) noexcept;

        /// The instance-state block: graph variables at their StateLayout
        /// offsets. Empty for a graph with no variables.
        [[nodiscard]] std::span<std::byte> instanceState() { return state_; }
        [[nodiscard]] std::span<const std::byte> instanceState() const { return state_; }

        /// Re-initialize the instance state to the declared defaults —
        /// same bytes a freshly compiled instance starts with.
        void resetInstanceState();

        /// JIT symbol of a reflected invokable's trampoline — shared naming
        /// SSOT between the IR builder (call-site declaration) and compile()
        /// (symbol binding). Hash-based: stable across builds.
        static std::string invokerSymbol(const lux::meta::RefInvokable& fn);

        /// func.func symbol of an event entry: "lux_event_" + the display
        /// name with every non-[A-Za-z0-9_] character replaced by '_'.
        static std::string eventSymbol(std::string_view event_name);

    private:
        FlowScriptInstance();

        struct EventEntry
        {
            std::string name;      ///< display name (the OnEvent node's name)
            std::string symbol;    ///< func.func symbol (eventSymbol(name))
            std::size_t arg_count; ///< payload parameter count
        };

        std::vector<EventEntry>                events_;
        std::unique_ptr<IR>                    ir_;      // keeps the module alive
        std::unique_ptr<mlir::ExecutionEngine> engine_;
        std::vector<std::byte>                 state_;   // instance-state block
    };
}
