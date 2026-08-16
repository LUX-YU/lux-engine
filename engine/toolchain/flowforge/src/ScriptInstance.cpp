#include "lux/engine/toolchain/flowforge/mlir/ScriptInstance.hpp"
#include "lux/engine/toolchain/flowforge/mlir/IR.hpp"
#include "lux/engine/toolchain/flowforge/mlir/IRImpl.hpp"
#include "lux/engine/authoring/flowforge/FlowGraph.hpp"
#include "lux/engine/authoring/flowforge/FunctionalNode.hpp"
#include <lux/engine/meta/Meta.hpp>

#include <mlir/ExecutionEngine/ExecutionEngine.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/TargetSelect.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace lux::flowforge
{
    FlowScriptInstance::FlowScriptInstance()  = default;
    FlowScriptInstance::~FlowScriptInstance() = default;

    std::string FlowScriptInstance::invokerSymbol(const lux::meta::RefInvokable& fn)
    {
        // Hash-based and PROCESS-INDEPENDENT: the IR builder bakes this name
        // into the module and the host binds the trampoline address under the
        // same name — full_name is the stable identity. AOT artefacts carry
        // the baked name across processes (cooked on one machine, resolved
        // on another), so the hash must be an algorithm we own: FNV-1a.
        // llvm::hash_value is documented as unstable across executions.
        uint64_t h = 0xcbf29ce484222325ULL;
        for (char c : fn.full_name) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
        std::ostringstream oss;
        oss << "_lfi_" << std::hex << h;
        return oss.str();
    }

    std::string FlowScriptInstance::eventSymbol(std::string_view event_name)
    {
        std::string sym = "lux_event_";
        sym.reserve(sym.size() + event_name.size());
        for (char c : event_name)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9') || c == '_';
            sym.push_back(ok ? c : '_');
        }
        return sym;
    }

    std::unique_ptr<FlowScriptInstance> FlowScriptInstance::compile(
        IRContext&                   ctx,
        const FlowGraph&             graph,
        std::vector<JitNativeSymbol> extra_symbols,
        std::string*                 error_out)
    {
        const auto fail = [&](std::string msg) -> std::unique_ptr<FlowScriptInstance> {
            if (error_out) *error_out = std::move(msg);
            return nullptr;
        };

        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();

        std::unique_ptr<FlowScriptInstance> inst(new FlowScriptInstance());

        // Event table straight from the graph (no IR round trip needed).
        for (const auto& storage : graph.nodes())
        {
            const Node* n = storage.node.get();
            if (!n || n->operation() != ENodeOperation::ON_EVENT) continue;
            const auto& ev = static_cast<const OnEventNode&>(*n);
            inst->events_.push_back(EventEntry{
                ev.name(), eventSymbol(ev.name()), ev.paramInfos().size() });
        }

        MLIRBuilder builder(&ctx);
        auto built = builder.generateIR(graph);
        if (!built)
            return fail("compile failed: " + built.error().message);
        inst->ir_ = std::move(built.value());
        auto lowered = lowerToLLVM(*inst->ir_);
        if (!lowered)
            return fail("compile failed: " + lowered.error().message);

        // Instance-state block: sized by the build-time layout, initialized
        // with the declared variable defaults (see StateLayout.hpp). This
        // instance OWNS its state — invoke() passes the base pointer as the
        // hidden leading argument of every generated function.
        inst->state_.assign(inst->ir_->impl().state_size, std::byte{0});
        inst->resetInstanceState();

        mlir::ModuleOp module = inst->ir_->impl().top_module.get();
        auto* mlir_ctx = module.getContext();
        mlir::registerBuiltinDialectTranslation(*mlir_ctx);
        mlir::registerLLVMDialectTranslation(*mlir_ctx);

        mlir::ExecutionEngineOptions engine_options;
        engine_options.jitCodeGenOptLevel = llvm::CodeGenOptLevel::Default;
        auto maybe_engine = mlir::ExecutionEngine::create(module, engine_options);
        if (!maybe_engine)
            return fail("ExecutionEngine::create failed: "
                        + llvm::toString(maybe_engine.takeError()));
        inst->engine_ = std::move(*maybe_engine);

        // Bind host symbols: every reflected function's invoker trampoline
        // (under its _lfi_ symbol) + the caller's hand-written C-ABI hosts.
        // Name storage only needs to survive the registerSymbols call.
        std::vector<std::pair<std::string, void*>> bound;
        if (lux::meta::ReflectionRegistry::initialized())
        {
            for (const auto& fn_ptr :
                 lux::meta::ReflectionRegistry::instance().functions())
            {
                const lux::meta::RefFunction* fn = fn_ptr.get();
                if (!fn || !fn->invokable.invoker) continue;
                bound.emplace_back(invokerSymbol(fn->invokable),
                                   reinterpret_cast<void*>(fn->invokable.invoker));
            }
        }
        for (const JitNativeSymbol& sym : extra_symbols)
        {
            if (sym.name && sym.address)
                bound.emplace_back(sym.name, sym.address);
        }
        if (!bound.empty())
        {
            inst->engine_->registerSymbols(
                [&](llvm::orc::MangleAndInterner mangle)
                {
                    llvm::orc::SymbolMap map;
                    for (const auto& [name, addr] : bound)
                    {
                        map[mangle(name)] = llvm::orc::ExecutorSymbolDef{
                            llvm::orc::ExecutorAddr::fromPtr(addr),
                            llvm::JITSymbolFlags::Exported};
                    }
                    return map;
                });
        }

        return inst;
    }

    void FlowScriptInstance::resetInstanceState()
    {
        std::fill(state_.begin(), state_.end(), std::byte{0});
        if (!ir_) return;
        const auto& defaults = ir_->impl().state_defaults;
        if (!defaults.empty() && !state_.empty())
            std::memcpy(state_.data(), defaults.data(),
                        std::min(defaults.size(), state_.size()));
    }

    bool FlowScriptInstance::hasEvent(std::string_view event) const
    {
        for (const auto& e : events_)
            if (e.name == event) return true;
        return false;
    }

    bool FlowScriptInstance::invoke(std::string_view event,
                                    std::span<void* const> args,
                                    std::string* error_out)
    {
        const auto fail = [&](std::string msg) {
            if (error_out) *error_out = std::move(msg);
            return false;
        };
        if (!engine_) return fail("script instance has no engine");

        const EventEntry* entry = nullptr;
        for (const auto& e : events_)
            if (e.name == event) { entry = &e; break; }
        if (!entry)
            return fail("unknown event '" + std::string(event) + "'");
        if (args.size() != entry->arg_count)
            return fail("event '" + std::string(event) + "' expects "
                        + std::to_string(entry->arg_count) + " argument(s), got "
                        + std::to_string(args.size()));

        // Packed convention: slot i points at the i-th argument's STORAGE.
        // Slot 0 is the hidden instance-state pointer, so its storage is a
        // local void* holding the block's base address (null when the graph
        // is stateless — generated code then never dereferences it).
        void* state_base = state_.empty() ? nullptr
                                          : static_cast<void*>(state_.data());
        llvm::SmallVector<void*, 8> packed;
        packed.reserve(args.size() + 1);
        packed.push_back(&state_base);
        packed.append(args.begin(), args.end());
        if (llvm::Error err = engine_->invokePacked(entry->symbol, packed))
            return fail("invoke failed: " + llvm::toString(std::move(err)));
        return true;
    }
}
