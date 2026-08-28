#include "lux/engine/flowforge/compiler/ScriptInstance.hpp"
#include "lux/engine/flowforge/compiler/IR.hpp"
#include "lux/engine/flowforge/compiler/IRImpl.hpp"
#include "lux/engine/flowforge/graph/FlowGraph.hpp"
#include "lux/engine/flowforge/graph/FunctionalNode.hpp"
#include <lux/engine/meta/Meta.hpp>

#include <mlir/ExecutionEngine/ExecutionEngine.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/TargetSelect.h>

#include <algorithm>
#include <cstring>
#include <new>
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

    FlowForgeResult<std::unique_ptr<FlowScriptInstance>> FlowScriptInstance::compile(
        IRContext& context,
        const FlowGraph& graph,
        std::vector<JitNativeSymbol> extra_symbols
    ) noexcept
    {
        try
        {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();

            std::unique_ptr<FlowScriptInstance> inst(new FlowScriptInstance());

            // Event table straight from the graph (no IR round trip needed).
            for (const auto& storage : graph.nodes())
            {
                const Node* node = storage.node.get();
                if (node == nullptr || node->operation() != ENodeOperation::ON_EVENT)
                    continue;
                const auto& event = static_cast<const OnEventNode&>(*node);
                inst->events_.push_back(EventEntry{
                    event.name(),
                    eventSymbol(event.name()),
                    event.paramInfos().size()
                });
            }

            auto builder = MLIRBuilder::create(context);
            if (!builder)
                return lux::cxx::unexpected(std::move(builder.error()));
            auto built = builder->generateIR(graph);
            if (!built)
                return lux::cxx::unexpected(std::move(built.error()));
            inst->ir_ = std::move(*built);
            auto lowered = lowerToLLVM(*inst->ir_);
            if (!lowered)
                return lux::cxx::unexpected(std::move(lowered.error()));

            // Instance-state block: sized by the build-time layout, initialized
            // with the declared variable defaults (see StateLayout.hpp). This
            // instance OWNS its state — invoke() passes the base pointer as the
            // hidden leading argument of every generated function.
            inst->state_.assign(inst->ir_->impl().state_size, std::byte{0});
            inst->resetInstanceState();

            mlir::ModuleOp module = inst->ir_->impl().top_module.get();
            auto* mlir_context = module.getContext();
            mlir::registerBuiltinDialectTranslation(*mlir_context);
            mlir::registerLLVMDialectTranslation(*mlir_context);

            mlir::ExecutionEngineOptions engine_options;
            engine_options.jitCodeGenOptLevel = llvm::CodeGenOptLevel::Default;
            auto maybe_engine = mlir::ExecutionEngine::create(module, engine_options);
            if (!maybe_engine)
            {
                return lux::cxx::unexpected(FlowForgeFailure{
                    .code = EFlowForgeError::JIT_ENGINE_CREATION_FAILED,
                    .message = llvm::toString(maybe_engine.takeError())
                });
            }
            inst->engine_ = std::move(*maybe_engine);

        // Bind host symbols: every reflected function's invoker trampoline
        // (under its _lfi_ symbol) + the caller's hand-written C-ABI hosts.
        // Name storage only needs to survive the registerSymbols call.
            std::vector<std::pair<std::string, void*>> bound;
            if (lux::meta::ReflectionRegistry::initialized())
            {
                for (const auto& function : lux::meta::ReflectionRegistry::instance().functions())
                {
                    const lux::meta::RefFunction* reflected = function.get();
                    if (reflected == nullptr || reflected->invokable.invoker == nullptr)
                        continue;
                    bound.emplace_back(
                        invokerSymbol(reflected->invokable),
                        reinterpret_cast<void*>(reflected->invokable.invoker)
                    );
                }
            }
            for (const JitNativeSymbol& symbol : extra_symbols)
            {
                if (symbol.name != nullptr && symbol.address != nullptr)
                    bound.emplace_back(symbol.name, symbol.address);
            }
            if (!bound.empty())
            {
                inst->engine_->registerSymbols(
                    [&](llvm::orc::MangleAndInterner mangle)
                    {
                        llvm::orc::SymbolMap map;
                        for (const auto& [name, address] : bound)
                        {
                            map[mangle(name)] = llvm::orc::ExecutorSymbolDef{
                                llvm::orc::ExecutorAddr::fromPtr(address),
                                llvm::JITSymbolFlags::Exported
                            };
                        }
                        return map;
                    }
                );
            }

            return inst;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
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

    FlowForgeResult<void> FlowScriptInstance::invoke(
        std::string_view event,
        std::span<void* const> args
    ) noexcept
    {
        try
        {
            const auto fail = [](EFlowForgeError code, std::string message) -> FlowForgeResult<void>
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = code, .message = std::move(message)});
            };
            if (!engine_)
                return fail(EFlowForgeError::JIT_INVOCATION_FAILED, "script instance has no engine");

            const EventEntry* entry = nullptr;
            for (const auto& candidate : events_)
            {
                if (candidate.name == event)
                {
                    entry = &candidate;
                    break;
                }
            }
            if (entry == nullptr)
                return fail(EFlowForgeError::JIT_SYMBOL_LOOKUP_FAILED, "unknown event '" + std::string(event) + "'");
            if (args.size() != entry->arg_count)
            {
                return fail(
                    EFlowForgeError::JIT_INVOCATION_FAILED,
                    "event '" + std::string(event) + "' expects " + std::to_string(entry->arg_count) +
                        " argument(s), got " + std::to_string(args.size())
                );
            }

        // Packed convention: slot i points at the i-th argument's STORAGE.
        // Slot 0 is the hidden instance-state pointer, so its storage is a
        // local void* holding the block's base address (null when the graph
        // is stateless — generated code then never dereferences it).
            void* state_base = state_.empty() ? nullptr : static_cast<void*>(state_.data());
            llvm::SmallVector<void*, 8> packed;
            packed.reserve(args.size() + 1);
            packed.push_back(&state_base);
            packed.append(args.begin(), args.end());
            if (llvm::Error error = engine_->invokePacked(entry->symbol, packed))
            {
                return fail(
                    EFlowForgeError::JIT_INVOCATION_FAILED,
                    "invoke failed: " + llvm::toString(std::move(error))
                );
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }
}
