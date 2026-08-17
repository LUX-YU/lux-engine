#include <atomic>
#include <queue>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringMap.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <llvm/Support/raw_ostream.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/Diagnostics.h>
#include <mlir/IR/Verifier.h>
#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE
#include "lux/engine/toolchain/flowforge/mlir/IRImpl.hpp"
#include "lux/engine/toolchain/flowforge/mlir/TypeSizeMap.hpp"
#include "lux/engine/toolchain/flowforge/mlir/ScriptInstance.hpp"   // invokerSymbol / eventSymbol
#include "lux/engine/authoring/flowforge/FlowGraph.hpp"
#include "lux/engine/authoring/flowforge/StateLayout.hpp"
#include "lux/engine/authoring/flowforge/NodeBase.hpp"
#include "lux/engine/authoring/flowforge/ControlNode.hpp"
#include "lux/engine/authoring/flowforge/FunctionalNode.hpp"
#include "lux/engine/authoring/flowforge/ObjectNode.hpp"
#include "lux/engine/authoring/flowforge/ArithmeticNode.hpp"

namespace lux::flowforge {
    // ============================================================================
    // Per-build context — owns the module, the builder, and ALL per-build
    // state (constant pools, symbol-uniquification id). Lives here rather
    // than on MLIRBuilderImpl so each generateIR call gets fresh state and
    // pools aren't reused across builds (which would leak stale Op handles
    // from a previous module).
    // ============================================================================
    class BuilderContext {
    public:
        BuilderContext(mlir::MLIRContext* context)
            : ctx(context),
              builder(context),
              loc(builder.getUnknownLoc()),
              module_id(next_module_id.fetch_add(1, std::memory_order_relaxed))
        {
            module       = builder.create<mlir::ModuleOp>(loc);
            module_owner = mlir::OwningOpRef<mlir::ModuleOp>(module);
            token        = mlir::flowforge::FLOWFORGE_TOKEN_TYPE::get(ctx);
        }

        mlir::MLIRContext* ctx;
        mlir::OpBuilder    builder;
        mlir::Location     loc;
        mlir::Type         token;             // flowforge::FLOWFORGE_TOKEN_TYPE

        // The module is created detached, so somebody must erase it — the
        // OwningOpRef reclaims a half-built module on every failure path.
        mlir::OwningOpRef<mlir::ModuleOp> module_owner;
        mlir::ModuleOp     module;            // plain view of *module_owner
        mlir::func::FuncOp main_func;         // the @main wrapper

        // Symbol-uniquification id, fresh per BuilderContext (per generateIR).
        // Used to scope LLVM global symbols emitted by this build so multiple
        // FlowForge-compiled modules can coexist in one binary without clash.
        const uint32_t module_id;

        // Per-build constant pools. Class storage is keyed by (class_hash,
        // value_hash) so different VALUES of the same class produce
        // independent storage. String pool dedupes by bytes; emitted symbol
        // name is hash-based, not the string content (avoids leaking long /
        // non-ASCII string literals into the binary's symbol table).
        llvm::DenseMap<std::pair<uint64_t, uint64_t>, mlir::LLVM::GlobalOp> class_globals;
        llvm::StringMap<mlir::LLVM::GlobalOp>                                string_globals;

        // Extern function declarations cache. Dedupes by symbol name so
        // multiple NativeFuncCall nodes targeting the same function share
        // one func.func declaration in the module.
        llvm::StringMap<mlir::func::FuncOp>                                  extern_funcs;

        // The graph being compiled (variable table lookups) and its
        // instance-state layout: variable accesses lower to
        // `state_ptr + offset` into a HOST-owned block (StateLayout.hpp),
        // so the compiled binary carries no variable storage of its own.
        const FlowGraph*   graph = nullptr;
        StateLayout        state_layout;

        // Leading block argument of the CURRENT function being lowered
        // (set by lowerFunction): the instance-state base pointer. Valid
        // inside nested regions too — they are not isolated from above.
        mlir::Value        state_ptr;

        const Node* current_node = nullptr;
        const Pin*  current_pin  = nullptr;

    private:
        static std::atomic<uint32_t> next_module_id;
    };

    std::atomic<uint32_t> BuilderContext::next_module_id{1};

    static FlowForgeFailure buildFailure(
        const BuilderContext& bc,
        std::string           message,
        bool                  include_pin = false
    )
    {
        return FlowForgeFailure{
            .code = EFlowForgeError::GRAPH_INVALID,
            .message = std::move(message),
            .node_id = bc.current_node ? bc.current_node->id() : 0,
            .pin_id = include_pin && bc.current_pin ? bc.current_pin->id() : 0,
        };
    }

#define LUX_FF_FAIL(build_context, message) \
    return lux::cxx::unexpected(buildFailure((build_context), (message)))
#define LUX_FF_FAIL_AT_PIN(build_context, message) \
    return lux::cxx::unexpected(buildFailure((build_context), (message), true))
#define LUX_FF_TRY_VALUE(name, expression) \
    auto name##_result = (expression); \
    if (!name##_result) \
        return lux::cxx::unexpected(std::move(name##_result.error())); \
    auto name = std::move(name##_result.value())
#define LUX_FF_TRY(expression) \
    do \
    { \
        auto lux_ff_result = (expression); \
        if (!lux_ff_result) \
            return lux::cxx::unexpected(std::move(lux_ff_result.error())); \
    } while (false)

    // Format a unique LLVM global symbol name for this BuilderContext.
    //   _lf_<module_id>_<kind>_<a>[_<b>]   (all hex)
    //
    // kind: short tag ("g" = class storage, "s" = string).
    // a, b: opaque hashes (e.g. class_hash, value_hash). b=0 omits the suffix.
    static std::string makeGlobalSymbol(BuilderContext& bc, const char* kind,
                                        uint64_t a, uint64_t b = 0) {
        std::ostringstream oss;
        oss << "_lf_" << std::hex << bc.module_id << '_' << kind << '_' << a;
        if (b)
        {
            oss << '_' << b;
        }
        return oss.str();
    }

    static bool isPointerQual(const lux::meta::RefType& rt) {
        using lux::meta::ETypeQual;
        switch (static_cast<ETypeQual>(rt.qtype.qual)) {
            case ETypeQual::Ptr:
            case ETypeQual::PtrToConst:
            case ETypeQual::ConstPtr:
            case ETypeQual::ConstPtrToConst:
                return true;
            default:
                return false;
        }
    }

    // Convert a reflected RefType to an MLIR type. Pointer-qualified types
    // are llvm.ptr REGARDLESS of their base (an `int32_t*` is a pointer, not
    // an i32); of the value types only the primitive bases get specific MLIR
    // types, and records/unknowns fall back to llvm.ptr. Signedness (int vs
    // uint) is not encoded in the MLIR integer type itself (MLIR convention:
    // signless integers) — op SELECTION carries the signedness instead, see
    // isUnsignedInt / lowerBinaryOp.
    static mlir::Type refTypeToMLIR(BuilderContext& bc, const lux::meta::RefType& rt) {
        using lux::meta::EBaseType;
        auto& b = bc.builder;
        if (isPointerQual(rt))
        {
            return mlir::LLVM::LLVMPointerType::get(bc.ctx);
        }
        switch (static_cast<EBaseType>(rt.qtype.base)) {
            case EBaseType::Bool:   return b.getI1Type();
            case EBaseType::Int8:
            case EBaseType::Uint8:  return b.getI8Type();
            case EBaseType::Int16:
            case EBaseType::Uint16: return b.getIntegerType(16);
            case EBaseType::Int32:
            case EBaseType::Uint32: return b.getI32Type();
            case EBaseType::Int64:
            case EBaseType::Uint64: return b.getI64Type();
            case EBaseType::Float:  return b.getF32Type();
            case EBaseType::Double: return b.getF64Type();
            case EBaseType::Void:
            case EBaseType::Record:
            case EBaseType::Unknown:
            default:                return mlir::LLVM::LLVMPointerType::get(bc.ctx);
        }
    }

    static bool isUnsignedInt(const lux::meta::RefType& rt) {
        using lux::meta::EBaseType;
        switch (static_cast<EBaseType>(rt.qtype.base)) {
            case EBaseType::Uint8:
            case EBaseType::Uint16:
            case EBaseType::Uint32:
            case EBaseType::Uint64: return true;
            default:                return false;
        }
    }

    static bool isFloatType(const lux::meta::RefType& rt) {
        using lux::meta::EBaseType;
        auto base = static_cast<EBaseType>(rt.qtype.base);
        return base == EBaseType::Float || base == EBaseType::Double;
    }

    //==============================================================================
    // MLIRBuilderImpl — drives FlowGraph -> FlowForge dialect IR generation.
    //==============================================================================
    class MLIRBuilderImpl {
        // Pin id -> SSA value mappings (per-build).
        struct ValueMaps {
            llvm::DenseMap<uint64_t, mlir::Value> exec_tok;

            // Values produced at a definite point ON the exec chain
            // (native-call results, loop induction variables, set-variable
            // passthroughs). Computed exactly once; safe to reference from
            // any point their definition dominates, so a single flat map.
            llvm::DenseMap<uint64_t, mlir::Value> exec_data;

            // Per-region caches of PURE recomputable values (arithmetic /
            // comparison nodes, pin constants). Lookup consults only the
            // TOP scope and re-materializes on miss — that implements the
            // UE-style "re-evaluated per use site" semantics: an expression
            // used inside a loop body or branch leg is re-emitted in that
            // region rather than reusing an outer region's value, so it
            // observes per-iteration state. Cross-region duplicates are
            // trivially cleaned up by LLVM CSE.
            llvm::SmallVector<llvm::DenseMap<uint64_t, mlir::Value>, 4> pure_scopes;

            ValueMaps() { pure_scopes.emplace_back(); }

            // RAII: one pure-value scope per lowered region.
            struct PureScope {
                ValueMaps& vm;
                explicit PureScope(ValueMaps& v) : vm(v) { vm.pure_scopes.emplace_back(); }
                ~PureScope() { vm.pure_scopes.pop_back(); }
            };

            // Explicit lookup helper. Use this instead of `vm.exec_tok[id]`:
            // operator[] silently default-constructs a null mlir::Value on
            // miss, which downstream code happily uses as an operand and
            // crashes during op verification with a confusing "operand 0
            // was null" message. requireExecTok reports the missing-link
            // site together with the offending node.
            FlowForgeResult<mlir::Value>
                requireExecTok(uint64_t pin_id, BuilderContext& bc) const {
                auto it = exec_tok.find(pin_id);
                if (it == exec_tok.end() || !it->second)
                {
                    LUX_FF_FAIL(bc, "exec token not materialised");
                }
                return it->second;
            }

            FlowForgeResult<llvm::SmallVector<mlir::Value>>
                gatherPredTokens(const ExecInPin& in, BuilderContext& bc) const {
                llvm::SmallVector<mlir::Value> preds;
                for (auto* ex : in.linkedPins()) {
                    auto it = exec_tok.find(ex->id());
                    if (it == exec_tok.end() || !it->second)
                    {
                        LUX_FF_FAIL_AT_PIN(bc, "exec token not materialised");
                    }
                    preds.push_back(it->second);
                }
                return preds;
            }
        };

        // OpBuilder::createBlock both creates the block with args and SETS the
        // insertion point into it. InsertionGuard restores the caller's
        // insertion point on return — callers can therefore use the caller's
        // bc.builder without worrying about it leaking into the new block.
        static mlir::Block* addSingleBlockWithArgs(mlir::OpBuilder& b,
            mlir::Region& region,
            mlir::TypeRange argTys,
            mlir::Location loc)
        {
            mlir::OpBuilder::InsertionGuard guard(b);
            llvm::SmallVector<mlir::Location, 4> locs(argTys.size(), loc);
            return b.createBlock(&region, region.end(), argTys, locs);
        }

    public:
        explicit MLIRBuilderImpl(mlir::MLIRContext* ctx);
        FlowForgeResult<std::unique_ptr<IR>> generateMLIR(const FlowGraph&);

    private:
        FlowForgeResult<mlir::Value> getOperand(
            const DataInPin&, ValueMaps&, BuilderContext&, bool asIndex = false
        );
        FlowForgeResult<mlir::Value> buildConstant(
            const DataInPin&, BuilderContext&, const lux::meta::RuntimeObject&,
            bool asIndex = false
        );

        // Pin-independent scalar-constant emission (bool / ints / floats).
        // Shared by buildConstant and variable default-value initialization.
        // Throws for non-scalar base types.
        FlowForgeResult<mlir::Value> buildScalarConstantValue(
            BuilderContext&, const lux::meta::RefType&,
            const lux::meta::RuntimeObject&, bool asIndex = false
        );

        // Materialize `bytes` as internal constant module storage and return
        // a pointer to it. Backs both aggregate (struct) constants and
        // strings — matching refTypeToMLIR, which types every non-scalar
        // native-call parameter as !llvm.ptr.
        mlir::Value materializeBytesConstant(BuilderContext&, uint64_t type_hash, llvm::StringRef bytes);

        // On-demand expansion of the PURE data subgraph. Called by
        // getOperand when a linked source has no materialized value yet:
        // emits the pure node (recursively evaluating ITS inputs first) at
        // the current insertion point and caches the result in the current
        // pure scope. Non-pure sources (a native call that hasn't run yet)
        // and data cycles return a structured graph error.
        FlowForgeResult<mlir::Value> materializePureValue(
            const DataOutPin& src, ValueMaps&, BuilderContext&
        );
        FlowForgeResult<mlir::Value> lowerBinaryOp(
            const BinaryOpNode&, ValueMaps&, BuilderContext&
        );
        FlowForgeResult<mlir::Value> lowerUnaryOp(
            const UnaryOpNode&, ValueMaps&, BuilderContext&
        );

        // Implicit scalar conversion of `v` to the declared type `dst_rt`
        // (int widening/narrowing, int<->float, float widening). Extension
        // signedness follows the DESTINATION type. Anything non-scalar or
        // float->int throws.
        FlowForgeResult<mlir::Value> coerceScalar(
            BuilderContext&, mlir::Value v, const lux::meta::RefType& dst_rt
        );

        // Address of a graph variable inside the instance-state block:
        // `state_ptr + layout offset` (byte GEP at the current insertion
        // point). Defaults are NOT materialized here — the host initializes
        // the block from the layout's defaults blob before invoking.
        FlowForgeResult<mlir::Value> varSlotAddress(uint64_t var_id, BuilderContext&);

        // Merge convergent control tokens into a single SSA value.
        // - 0 inputs: build error (the caller's exec-in pin has no link).
        // - 1 input: return it directly.
        // - >1 inputs all identical: return the unique value.
        // - >1 inputs with distinct SSA: emit `flowforge.token_merge`. The
        //   FlowForge -> LLVM lowering pass realises this as a basic-block
        //   PHI in the join block.
        FlowForgeResult<mlir::Value> mergeExecTokens(
            BuilderContext&,
            const llvm::SmallVector<mlir::Value>&
        );

        template<size_t Bits>
        mlir::Value global_constant_assign(BuilderContext&, const lux::meta::RuntimeObject&, bool asIndex);
        mlir::Value global_string_constant_assign(BuilderContext&, const char*, size_t);

        // Sequential per-node lowering helpers — no region recursion. Control
        // ops (Branch / ForLoop / WhileLoop / Sequence) are inlined in
        // lowerChain's switch since each needs to set up its own region(s)
        // and recurse.
        FlowForgeResult<void> lowerReturnImpl(
            const Node&, mlir::Value in_tok, ValueMaps&, BuilderContext&
        );
        FlowForgeResult<void> lowerNativeCallImpl(
            const Node&, mlir::Value in_tok, ValueMaps&, BuilderContext&
        );

        // Per-region recursive lowering driver.
        //
        // reachableFromPin: forward reachability through exec edges. Used
        //   by findPostDom to determine which graph nodes lie downstream of
        //   a given control-op leg.
        // findPostDom:      for a Branch with two legs, the closest node
        //   reachable from BOTH legs — i.e., where the legs reconverge.
        //   That node belongs to the OUTER scope; the legs nest only the
        //   nodes strictly before it.
        // lowerChain:       walks the exec chain forward from a given start
        //   pin into the current insertion point. For control ops it
        //   creates the op and recurses INTO each sub-region's block, then
        //   continues the outer chain via the post-dom (Branch) or
        //   .completed pin (loops). Returns the SSA value of the "current
        //   exec token" at the end of the chain — what the enclosing
        //   region's yield should use — or a NULL value when the chain
        //   terminated (Return/Break emitted a terminator; nothing may
        //   follow it, so the caller must NOT emit a yield).
        //   `loop_depth` counts enclosing loop regions — Break outside any
        //   loop is rejected at build time.
        std::unordered_set<const Node*>
            reachableFromPin(const ExecOutPin* pin) const;
        const Node* findPostDom(const Node* branch) const;
        FlowForgeResult<mlir::Value> lowerChain(
            BuilderContext& bc, ValueMaps& vm,
            const ExecOutPin& start_pin,
            std::unordered_set<const Node*>& lowered,
            const std::unordered_set<const Node*>& external,
            int loop_depth
        );

        // Lower one graph function: a START entry becomes @main, a
        // FUNC_DEF_START entry becomes a func.func named after the
        // FuncDefNode (signature from its arg/ret declarations). All
        // functions share one module, so graph calls are plain func.calls
        // and recursion is legal.
        FlowForgeResult<void> lowerFunction(BuilderContext& bc, const Node& entry);

        mlir::MLIRContext* context_;

        // Pure nodes currently on the materialization recursion stack —
        // re-entering one means the data graph has a cycle.
        llvm::SmallPtrSet<const Node*, 8> materializing_;
    };

    // ----------------------------------------------------------------------------
    // ctor — nothing to wire up; control-op dispatch happens inside lowerChain.
    // ----------------------------------------------------------------------------
    MLIRBuilderImpl::MLIRBuilderImpl(mlir::MLIRContext* context)
        : context_(context)
    { }

    // =============================================================================
    // Public API
    //
    // Generation walks the exec graph recursively. Outer chain lives inside
    // a `func.func @main` wrapper at module body; control ops (Branch /
    // ForLoop / WhileLoop / Sequence) lower their sub-regions by re-entering
    // lowerChain with insertion point inside the region's block.
    // =============================================================================
    FlowForgeResult<std::unique_ptr<IR>>
    MLIRBuilderImpl::generateMLIR(const FlowGraph& g)
    {
        BuilderContext bc(context_);
        bc.graph = &g;

        // Instance-state layout up front: validates every variable (type /
        // scalar-ness / default value) once, and gives varSlotAddress its
        // offsets. The recipe is published on the produced IR for hosts.
        {
            std::string layout_error;
            bc.state_layout = computeStateLayout(g, &layout_error);
            if (!layout_error.empty())
            {
                LUX_FF_FAIL(bc, std::move(layout_error));
            }
        }

        // Collect entries: at most one START (-> @main) plus any number of
        // FUNC_DEF_STARTs (-> named graph functions). All functions land in
        // ONE module, so graph calls are plain func.calls (whole-program).
        llvm::SmallVector<const Node*, 4> entries;
        const Node* start = nullptr;
        std::unordered_set<std::string> func_names;
        for (auto& storage : g.nodes()) {
            const Node* n = storage.node.get();
            switch (n->operation()) {
                case ENodeOperation::START:
                    if (start)
                    {
                        LUX_FF_FAIL(bc, "graph has more than one Start node");
                    }
                    start = n;
                    entries.push_back(n);
                    break;
                case ENodeOperation::FUNC_DEF_START: {
                    bc.current_node = n;
                    if (n->name().empty())
                    {
                        LUX_FF_FAIL(bc, "graph function has no name");
                    }
                    if (n->name() == "main")
                    {
                        LUX_FF_FAIL(bc, "'main' is reserved for the Start entry");
                    }
                    if (!func_names.insert(n->name()).second)
                    {
                        LUX_FF_FAIL(bc, "duplicate graph function name");
                    }
                    entries.push_back(n);
                    break;
                }
                case ENodeOperation::ON_EVENT: {
                    bc.current_node = n;
                    if (n->name().empty())
                    {
                        LUX_FF_FAIL(bc, "event entry has no name");
                    }
                    // Uniqueness on the SANITIZED symbol — two display names
                    // that collapse to the same symbol would collide.
                    if (!func_names.insert(
                            FlowScriptInstance::eventSymbol(n->name())).second)
                        LUX_FF_FAIL(bc, "duplicate event name");
                    entries.push_back(n);
                    break;
                }
                default: break;
            }
        }
        if (entries.empty())
        {
            LUX_FF_FAIL(bc, "no entry node found");
        }

        for (const Node* entry : entries)
        {
            LUX_FF_TRY(lowerFunction(bc, *entry));
        }

        // Verify before handing the module out and retain MLIR diagnostics.
        {
            std::string diag_text;
            mlir::ScopedDiagnosticHandler handler(context_,
                [&](mlir::Diagnostic& d) {
                    llvm::raw_string_ostream os(diag_text);
                    os << d.str() << '\n';
                    return mlir::success();
                });
            if (mlir::failed(mlir::verify(bc.module)))
                return lux::cxx::unexpected(FlowForgeFailure{
                    .code = EFlowForgeError::IR_VERIFICATION_FAILED,
                    .message = "generated MLIR module failed verification:\n"
                        + diag_text,
                });
        }

        auto ir = std::make_unique<IR>();
        ir->impl().top_module     = std::move(bc.module_owner);
        ir->impl().state_size     = bc.state_layout.size;
        ir->impl().state_hash     = bc.state_layout.hash;
        ir->impl().state_defaults = bc.state_layout.defaults;
        return ir;
    }

    // =============================================================================
    // lowerFunction — one func.func per entry node.
    //
    // Signature derivation: START -> void @main(); FUNC_DEF_START -> name +
    // argument types from the FuncDefNode's declarations, result types from
    // its declared return values. Argument block-args are published as
    // exec_data for the FuncDef's argument out-pins.
    // =============================================================================
    FlowForgeResult<void>
    MLIRBuilderImpl::lowerFunction(BuilderContext& bc, const Node& entry)
    {
        bc.current_node = &entry;
        bc.builder.setInsertionPointToEnd(bc.module.getBody());

        std::string fn_name = "main";
        // EVERY generated function takes the instance-state base pointer as
        // its LEADING argument — graph variables live in a host-owned block
        // (StateLayout.hpp), and graph-internal calls forward the pointer so
        // all of an instance's functions share its variables. Declared /
        // payload arguments follow it.
        llvm::SmallVector<mlir::Type, 4> arg_tys;
        arg_tys.push_back(mlir::LLVM::LLVMPointerType::get(bc.ctx));
        llvm::SmallVector<mlir::Type, 2> ret_tys;
        const FuncDefNode* def = nullptr;
        const OnEventNode* event = nullptr;
        const ExecOutPin* entry_pin = nullptr;

        if (entry.operation() == ENodeOperation::START) {
            entry_pin = &static_cast<const StartNode&>(entry).execOutPin();
        } else if (entry.operation() == ENodeOperation::FUNC_DEF_START) {
            def = &static_cast<const FuncDefNode&>(entry);
            fn_name = def->name();
            for (const auto& a : def->argInfos()) {
                if (!a.type)
                {
                    LUX_FF_FAIL(bc, "function argument has no type");
                }
                arg_tys.push_back(refTypeToMLIR(bc, *a.type));
            }
            for (const auto& r : def->retInfos()) {
                if (!r.type)
                {
                    LUX_FF_FAIL(bc, "function return value has no type");
                }
                ret_tys.push_back(refTypeToMLIR(bc, *r.type));
            }
            entry_pin = &def->execOutPin();
        } else if (entry.operation() == ENodeOperation::ON_EVENT) {
            event = &static_cast<const OnEventNode&>(entry);
            fn_name = FlowScriptInstance::eventSymbol(event->name());
            for (const auto& p : event->paramInfos()) {
                if (!p.type)
                {
                    LUX_FF_FAIL(bc, "event parameter has no type");
                }
                arg_tys.push_back(refTypeToMLIR(bc, *p.type));
            }
            entry_pin = &event->execOutPin();
        } else {
            LUX_FF_FAIL(bc, "unsupported entry node type");
        }

        auto fn = bc.builder.create<mlir::func::FuncOp>(
            bc.loc, fn_name, bc.builder.getFunctionType(arg_tys, ret_tys));
        // Event entries are invoked by the HOST through the packed/ciface
        // convention (FlowScriptInstance::invoke -> invokePacked), which
        // needs the _mlir_ciface wrapper.
        if (event)
            fn->setAttr("llvm.emit_c_interface",
                        mlir::UnitAttr::get(bc.ctx));
        auto* entry_block = fn.addEntryBlock();
        bc.main_func = fn;   // current function: alloca/return-type context
        bc.builder.setInsertionPointToStart(entry_block);

        ValueMaps vm;
        std::unordered_set<const Node*> lowered{&entry};

        // Entry token + argument surfacing.
        auto entry_tok = bc.builder.create<mlir::flowforge::StartOp>(
            bc.loc, bc.token).getResult();
        vm.exec_tok[entry_pin->id()] = entry_tok;
        bc.state_ptr = entry_block->getArgument(0);
        if (def) {
            const auto& arg_pins = def->argPins();
            for (size_t i = 0; i < arg_pins.size(); ++i)
            {
                vm.exec_data[arg_pins[i]->id()] = entry_block->getArgument(i + 1);
            }
        }
        if (event) {
            const auto& param_pins = event->paramPins();
            for (size_t i = 0; i < param_pins.size(); ++i)
            {
                vm.exec_data[param_pins[i]->id()] = entry_block->getArgument(i + 1);
            }
        }

        LUX_FF_TRY_VALUE(
            tail,
            lowerChain(
                bc, vm, *entry_pin, lowered, /*external=*/{}, /*loop_depth=*/0
            )
        );

        // A null tail means the chain ended in an explicit Return/Break
        // terminator. Otherwise the chain just stopped — synthesize the
        // implicit "fall off the end" return, which is only legal for a
        // void function.
        if (tail) {
            if (!ret_tys.empty())
                LUX_FF_FAIL(bc,
                    "graph function with return values must end in a "
                    "Function Return node on every path");
            bc.builder.create<mlir::flowforge::ReturnOp>(
                bc.loc, mlir::TypeRange{}, mlir::ValueRange{tail});
        }
        return {};
    }

    // ============================================================================
    // Forward reachability through exec edges. Starts at `pin->nextPin()->node()`
    // (skipping the source) and walks every exec_out it encounters. Used by the
    // post-dominator finder to determine which graph nodes lie downstream of a
    // given control-op leg.
    // ============================================================================
    std::unordered_set<const Node*>
    MLIRBuilderImpl::reachableFromPin(const ExecOutPin* pin) const
    {
        std::unordered_set<const Node*> reach;
        if (!pin)
        {
            return reach;
        }
        const ExecInPin* in = pin->nextPin();
        if (!in)
        {
            return reach;
        }

        // Explicit worklist — editor graphs can be deep enough that a
        // recursive DFS risks blowing the stack.
        llvm::SmallVector<const Node*, 16> worklist{in->node()};
        while (!worklist.empty()) {
            const Node* n = worklist.pop_back_val();
            if (!reach.insert(n).second)
            {
                continue;
            }
            for (const Pin* p : n->outPins()) {
                if (p->kind() != EPinKind::EXEC_OUT)
                {
                    continue;
                }
                auto* ex = static_cast<const ExecOutPin*>(p);
                if (auto* dst = ex->nextPin())
                {
                    worklist.push_back(dst->node());
                }
            }
        }
        return reach;
    }

    // ============================================================================
    // Find the closest node reachable from BOTH legs of a Branch — that node
    // is the post-dominator and belongs to the OUTER scope (the leg regions
    // nest only the strictly-before-PD nodes; the PD is lowered once after the
    // BranchOp with its incoming tokens merged via flowforge.token_merge).
    //
    // BFS from the Branch through both legs in lockstep so the first node we
    // hit that's in the intersection is the topologically-closest one. Returns
    // nullptr when the legs never reconverge (each leg is independently
    // self-terminating — typically each ends in its own Return).
    // ============================================================================
    const Node* MLIRBuilderImpl::findPostDom(const Node* control) const
    {
        if (!control || control->operation() != ENodeOperation::BRANCH)
            return nullptr;
        const auto& br = static_cast<const BranchNode&>(*control);

        auto up_reach   = reachableFromPin(&br.execOutPinUp());
        auto down_reach = reachableFromPin(&br.execOutPinDown());

        std::unordered_set<const Node*> common;
        for (auto* n : up_reach)
            if (down_reach.count(n)) common.insert(n);
        if (common.empty()) return nullptr;

        std::queue<const Node*> q;
        std::unordered_set<const Node*> visited{control};
        if (auto* in = br.execOutPinUp().nextPin())   q.push(in->node());
        if (auto* in = br.execOutPinDown().nextPin()) q.push(in->node());
        while (!q.empty()) {
            auto* n = q.front(); q.pop();
            if (!visited.insert(n).second) continue;
            if (common.count(n)) return n;
            for (const Pin* p : n->outPins()) {
                if (p->kind() != EPinKind::EXEC_OUT) continue;
                auto* ex = static_cast<const ExecOutPin*>(p);
                if (auto* dst = ex->nextPin())
                    if (!visited.count(dst->node())) q.push(dst->node());
            }
        }
        return nullptr;
    }

    // ============================================================================
    // The recursive chain driver.
    //
    // Preconditions on entry:
    //   - bc.builder's insertion point is at the END of the target block.
    //   - vm.exec_tok[start_pin.id()] holds the incoming token for this chain
    //     (the predecessor's out-token, or the enclosing region's block-arg).
    //
    // The loop walks `start_pin -> nextPin().node()` and dispatches by op kind.
    // For each control op with sub-regions it sets the insertion point into
    // the region's block, recurses, then restores the insertion point on
    // return (via InsertionGuard) and continues the outer chain.
    //
    // Returns the SSA value of the "current exec token" at the end of the
    // chain, suitable for use as the operand of an enclosing YieldOp. If the
    // chain terminates in a Return op, returns the input token (the chain
    // produced no out-token — the Return is a terminator).
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::lowerChain(
        BuilderContext& bc, ValueMaps& vm,
        const ExecOutPin& start_pin,
        std::unordered_set<const Node*>& lowered,
        const std::unordered_set<const Node*>& external,
        int loop_depth)
    {
        const ExecOutPin* cur_pin = &start_pin;
        LUX_FF_TRY_VALUE(cur_tok, vm.requireExecTok(cur_pin->id(), bc));

        while (true) {
            const ExecInPin* next_in = cur_pin->nextPin();
            if (!next_in) return cur_tok;                  // chain ends
            const Node* node = next_in->node();
            if (external.count(node)) return cur_tok;      // outer scope handles
            if (lowered.count(node))  return cur_tok;      // already lowered
            lowered.insert(node);
            bc.current_node = node;

            // Merge multi-link in-tokens via flowforge.token_merge (single-link
            // and all-same fast-paths inside mergeExecTokens).
            mlir::Value in_tok = cur_tok;
            if (next_in->linkedPins().size() > 1) {
                LUX_FF_TRY_VALUE(preds, vm.gatherPredTokens(*next_in, bc));
                LUX_FF_TRY_VALUE(merged, mergeExecTokens(bc, preds));
                in_tok = merged;
            }

            switch (node->operation()) {
                // --------------------- RETURN / FUNC_RETURN -------------------
                // Legal ANYWHERE (also nested in Branch/Loop regions): the
                // FlowForge -> CF lowering turns every return into a jump to
                // the function's exit block.
                case ENodeOperation::RETURN:
                case ENodeOperation::FUNC_RETURN: {
                    LUX_FF_TRY(lowerReturnImpl(*node, in_tok, vm, bc));
                    return mlir::Value{};
                }

                // -------------------------- BREAK -----------------------------
                case ENodeOperation::BREAK: {
                    if (loop_depth == 0)
                        LUX_FF_FAIL(bc,
                            "Break is only valid inside a loop body");
                    bc.builder.create<mlir::flowforge::BreakOp>(bc.loc, in_tok);
                    return mlir::Value{};
                }

                // ----------------------- NATIVE_CALL -------------------------
                case ENodeOperation::NATIVE_FUNC_CALL: {
                    LUX_FF_TRY(lowerNativeCallImpl(*node, in_tok, vm, bc));
                    const auto& call = static_cast<const NativeFuncCall&>(*node);
                    LUX_FF_TRY_VALUE(
                        next_token,
                        vm.requireExecTok(call.execOutPin().id(), bc)
                    );
                    cur_tok = next_token;
                    cur_pin = &call.execOutPin();
                    break;
                }

                // -------------------- GRAPH_FUNC_CALL -------------------------
                // A call to a FuncDef in the same graph: plain func.call to
                // the callee's symbol in the same module. The callee's
                // func.func may be generated before or after this one —
                // symbol references are order-independent.
                case ENodeOperation::GRAPH_FUNC_CALL: {
                    const auto& call = static_cast<const GraphFuncCallNode&>(*node);
                    const FuncDefNode* callee = call.callee();
                    if (!callee)
                        LUX_FF_FAIL(bc, "graph call has no callee");

                    llvm::SmallVector<mlir::Value, 4> operands;
                    // Callee shares THIS instance's variables: forward the
                    // state pointer as the hidden leading argument.
                    operands.push_back(bc.state_ptr);
                    for (const auto& pin : call.argPins()) {
                        LUX_FF_TRY_VALUE(v, getOperand(*pin, vm, bc));
                        if (pin->info().type
                            && !mlir::isa<mlir::LLVM::LLVMPointerType>(
                                   refTypeToMLIR(bc, *pin->info().type))
                            && !mlir::isa<mlir::LLVM::LLVMPointerType>(v.getType()))
                        {
                            LUX_FF_TRY_VALUE(
                                coerced,
                                coerceScalar(bc, v, *pin->info().type)
                            );
                            v = coerced;
                        }
                        operands.push_back(v);
                    }
                    llvm::SmallVector<mlir::Type, 2> ret_tys;
                    for (const auto& pin : call.resultPins())
                        ret_tys.push_back(refTypeToMLIR(bc, *pin->info().type));

                    auto callOp = bc.builder.create<mlir::func::CallOp>(
                        bc.loc, callee->name(), ret_tys, operands);
                    for (size_t i = 0; i < call.resultPins().size(); ++i)
                        vm.exec_data[call.resultPins()[i]->id()] = callOp.getResult(i);

                    vm.exec_tok[call.execOutPin().id()] = in_tok;
                    cur_tok = in_tok;
                    cur_pin = &call.execOutPin();
                    break;
                }

                // ----------------------- SET_FIELD ----------------------------
                case ENodeOperation::SET_FIELD: {
                    const auto& sf = static_cast<const SetFieldNode&>(*node);
                    if (!sf.field())
                        LUX_FF_FAIL(bc, "field access has no reflected field");
                    LUX_FF_TRY_VALUE(obj, getOperand(sf.objectPin(), vm, bc));
                    if (!mlir::isa<mlir::LLVM::LLVMPointerType>(obj.getType()))
                        LUX_FF_FAIL(bc, "field access needs an object pointer");
                    mlir::Type fty = refTypeToMLIR(bc, sf.field()->type);
                    if (mlir::isa<mlir::LLVM::LLVMPointerType>(fty)
                        && !isPointerQual(sf.field()->type))
                        LUX_FF_FAIL(bc,
                            "record-typed fields are not supported yet");
                    LUX_FF_TRY_VALUE(val, getOperand(sf.valueIn(), vm, bc));
                    if (!mlir::isa<mlir::LLVM::LLVMPointerType>(fty))
                    {
                        LUX_FF_TRY_VALUE(
                            coerced,
                            coerceScalar(bc, val, sf.field()->type)
                        );
                        val = coerced;
                    }
                    auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc.ctx);
                    mlir::Value gep = bc.builder.create<mlir::LLVM::GEPOp>(
                        bc.loc, ptr_ty, bc.builder.getI8Type(), obj,
                        llvm::ArrayRef<mlir::LLVM::GEPArg>{
                            static_cast<int32_t>(sf.field()->offset) });
                    bc.builder.create<mlir::LLVM::StoreOp>(bc.loc, val, gep);

                    // Object passthrough + threaded exec token.
                    vm.exec_data[sf.objectOut().id()] = obj;
                    vm.exec_tok[sf.execOutPin().id()] = in_tok;
                    cur_tok = in_tok;
                    cur_pin = &sf.execOutPin();
                    break;
                }

                // ---------------------- SET_VARIABLE -------------------------
                case ENodeOperation::SET_VARIABLE: {
                    const auto& sv = static_cast<const SetVariableNode&>(*node);
                    const auto* var =
                        bc.graph ? bc.graph->findVariable(sv.variableId()) : nullptr;
                    if (!var || !var->type)
                        LUX_FF_FAIL(bc, "graph variable not found");

                    LUX_FF_TRY_VALUE(
                        operand,
                        getOperand(sv.valueIn(), vm, bc)
                    );
                    LUX_FF_TRY_VALUE(
                        val,
                        coerceScalar(bc, operand, *var->type)
                    );
                    LUX_FF_TRY_VALUE(
                        slot,
                        varSlotAddress(sv.variableId(), bc)
                    );
                    bc.builder.create<mlir::LLVM::StoreOp>(bc.loc, val, slot);

                    // Passthrough value + threaded exec token.
                    vm.exec_data[sv.valueOut().id()] = val;
                    vm.exec_tok[sv.execOutPin().id()] = in_tok;
                    cur_tok = in_tok;
                    cur_pin = &sv.execOutPin();
                    break;
                }

                // ------------------------- BRANCH ----------------------------
                case ENodeOperation::BRANCH: {
                    const auto& br = static_cast<const BranchNode&>(*node);
                    LUX_FF_TRY_VALUE(cond, getOperand(br.dataInPin(), vm, bc));

                    const Node* pd = findPostDom(node);
                    auto inner_ext = external;
                    if (pd) inner_ext.insert(pd);

                    auto op = bc.builder.create<mlir::flowforge::BranchOp>(
                        bc.loc, mlir::TypeRange{bc.token, bc.token},
                        mlir::ValueRange{in_tok, cond});

                    // then-region — block-arg(0) = the input token for this
                    // leg. A null chain result means the leg ended in a
                    // Return/Break terminator — no yield may follow it.
                    bool then_terminated = false;
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, FLOWFORGE_GET_THEN_REGION(op),
                            mlir::TypeRange{bc.token}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        ValueMaps::PureScope pure_scope(vm);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto blk_arg = blk->getArgument(0);
                        vm.exec_tok[br.execOutPinUp().id()] = blk_arg;
                        LUX_FF_TRY_VALUE(
                            end,
                            lowerChain(
                                bc, vm, br.execOutPinUp(), lowered, inner_ext,
                                loop_depth
                            )
                        );
                        if (end)
                            bc.builder.create<mlir::flowforge::YieldOp>(bc.loc, end);
                        else
                            then_terminated = true;
                    }
                    // else-region — same shape
                    bool else_terminated = false;
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, FLOWFORGE_GET_ELSE_REGION(op),
                            mlir::TypeRange{bc.token}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        ValueMaps::PureScope pure_scope(vm);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto blk_arg = blk->getArgument(0);
                        vm.exec_tok[br.execOutPinDown().id()] = blk_arg;
                        LUX_FF_TRY_VALUE(
                            end,
                            lowerChain(
                                bc, vm, br.execOutPinDown(), lowered, inner_ext,
                                loop_depth
                            )
                        );
                        if (end)
                            bc.builder.create<mlir::flowforge::YieldOp>(bc.loc, end);
                        else
                            else_terminated = true;
                    }

                    // Post-lowering: the BranchOp's results are the per-leg
                    // out-tokens visible to OUTER scope. Re-publish:
                    //  - the Branch's own out-pins (so any direct consumer
                    //    sees the result, not the inner block-arg);
                    //  - every exec_out pin of nodes reachable inside each leg
                    //    (so PD's gatherPredTokens, which still reads the
                    //    inside-leg exec_out_pin ids, sees Branch.result(i)
                    //    rather than the now-out-of-scope inside-block SSA).
                    vm.exec_tok[br.execOutPinUp().id()]   = op.getResult(0);
                    vm.exec_tok[br.execOutPinDown().id()] = op.getResult(1);
                    auto up_reach   = reachableFromPin(&br.execOutPinUp());
                    auto down_reach = reachableFromPin(&br.execOutPinDown());
                    for (auto* n : up_reach) {
                        if (n == pd) continue;
                        for (const Pin* p : n->outPins())
                            if (p->kind() == EPinKind::EXEC_OUT
                                && vm.exec_tok.count(p->id()))
                                vm.exec_tok[p->id()] = op.getResult(0);
                    }
                    for (auto* n : down_reach) {
                        if (n == pd) continue;
                        for (const Pin* p : n->outPins())
                            if (p->kind() == EPinKind::EXEC_OUT
                                && vm.exec_tok.count(p->id()))
                                vm.exec_tok[p->id()] = op.getResult(1);
                    }

                    if (!pd) {
                        // Legs never reconverge. If BOTH legs terminated
                        // (Return/Break), control cannot flow past the
                        // branch — but the containing block still needs a
                        // terminator (BranchOp is not one). Emit the
                        // unreachable marker; the CF lowering erases it
                        // together with its provably-unreachable block.
                        if (then_terminated && else_terminated) {
                            bc.builder.create<mlir::flowforge::UnreachableOp>(
                                bc.loc, op.getResult(0));
                            return mlir::Value{};
                        }
                        // Otherwise at least one leg falls through and the
                        // outer chain simply has nothing more to lower.
                        return cur_tok;
                    }
                    // Pivot to the post-dominator. Any of its linked
                    // predecessors works (they all map to the right Branch
                    // result thanks to the remap above); the next iteration
                    // will see PD as `node`, gather the (already-remapped)
                    // pred tokens, and emit token_merge.
                    const ExecInPin* pd_exec_in = nullptr;
                    for (const Pin* p : pd->inPins())
                        if (p->kind() == EPinKind::EXEC_IN) {
                            pd_exec_in = static_cast<const ExecInPin*>(p);
                            break;
                        }
                    if (!pd_exec_in || pd_exec_in->linkedPins().empty())
                        LUX_FF_FAIL(bc, "post-dominator has no exec_in link");
                    cur_pin = pd_exec_in->linkedPins().front();
                    LUX_FF_TRY_VALUE(
                        post_dom_token,
                        vm.requireExecTok(cur_pin->id(), bc)
                    );
                    cur_tok = post_dom_token;
                    break;
                }

                // ----------------------- FOR_LOOP ----------------------------
                case ENodeOperation::FOR_LOOP: {
                    const auto& loop = static_cast<const ForLoopNode&>(*node);
                    auto idxTy = bc.builder.getIndexType();
                    // Constants come back as index directly (asIdx); linked
                    // integer values need an explicit index_cast.
                    auto toIndex = [&](mlir::Value v)
                        -> FlowForgeResult<mlir::Value> {
                        if (v.getType() == idxTy) return v;
                        if (mlir::isa<mlir::IntegerType>(v.getType()))
                            return bc.builder.create<mlir::arith::IndexCastOp>(
                                bc.loc, idxTy, v).getResult();
                        LUX_FF_FAIL(bc, "for-loop bound is not an integer");
                    };
                    LUX_FF_TRY_VALUE(
                        first_operand,
                        getOperand(loop.first_index(), vm, bc, /*asIdx=*/true)
                    );
                    LUX_FF_TRY_VALUE(
                        last_operand,
                        getOperand(loop.lastIndex(), vm, bc, /*asIdx=*/true)
                    );
                    LUX_FF_TRY_VALUE(first, toIndex(first_operand));
                    LUX_FF_TRY_VALUE(last, toIndex(last_operand));

                    auto op = bc.builder.create<mlir::flowforge::ForLoopOp>(
                        bc.loc,
                        mlir::TypeRange{bc.token, bc.token, idxTy},
                        mlir::ValueRange{in_tok, first, last});

                    // body region — args = (per-iter token, iv). The yield
                    // carries only the continuation token: the IV is a
                    // loop-defined block-arg (scf.for model), not a value
                    // that flows along region control-flow edges. A null
                    // chain result means the body ended in Return/Break —
                    // that terminator stands, no yield.
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, op.getBodyRegion(),
                            mlir::TypeRange{bc.token, idxTy}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        ValueMaps::PureScope pure_scope(vm);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto body_arg = blk->getArgument(0);
                        auto iv_arg   = blk->getArgument(1);
                        vm.exec_tok[loop.loopBody().id()]  = body_arg;
                        vm.exec_data[loop.indexPin().id()] = iv_arg;
                        LUX_FF_TRY_VALUE(
                            body_end,
                            lowerChain(
                                bc, vm, loop.loopBody(), lowered, external,
                                loop_depth + 1
                            )
                        );
                        if (body_end)
                            bc.builder.create<mlir::flowforge::YieldOp>(
                                bc.loc, body_end);
                    }

                    cur_tok = op.getResult(1);
                    vm.exec_tok[loop.completed().id()] = cur_tok;
                    cur_pin = &loop.completed();
                    break;
                }

                // ----------------------- WHILE_LOOP --------------------------
                // The condition's pure subgraph is expanded INSIDE the cond
                // region, so it is re-evaluated every iteration (a condition
                // reading a graph variable observes the body's writes).
                case ENodeOperation::WHILE_LOOP: {
                    const auto& loop = static_cast<const WhileLoopNode&>(*node);

                    auto op = bc.builder.create<mlir::flowforge::WhileLoopOp>(
                        bc.loc,
                        mlir::TypeRange{bc.token, bc.token},
                        mlir::ValueRange{in_tok});

                    // cond region — per-iteration condition evaluation.
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, op.getCondRegion(),
                            mlir::TypeRange{bc.token}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        ValueMaps::PureScope pure_scope(vm);
                        bc.builder.setInsertionPointToEnd(blk);
                        LUX_FF_TRY_VALUE(
                            cond_val,
                            getOperand(loop.dataInPin(), vm, bc)
                        );
                        bc.builder.create<mlir::flowforge::CondYieldOp>(
                            bc.loc, blk->getArgument(0), cond_val);
                    }
                    // body region — recursive
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, op.getBodyRegion(),
                            mlir::TypeRange{bc.token}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        ValueMaps::PureScope pure_scope(vm);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto body_arg = blk->getArgument(0);
                        vm.exec_tok[loop.loopBody().id()] = body_arg;
                        LUX_FF_TRY_VALUE(
                            body_end,
                            lowerChain(
                                bc, vm, loop.loopBody(), lowered, external,
                                loop_depth + 1
                            )
                        );
                        if (body_end)
                            bc.builder.create<mlir::flowforge::YieldOp>(bc.loc, body_end);
                    }

                    cur_tok = op.getResult(1);
                    vm.exec_tok[loop.completed().id()] = cur_tok;
                    cur_pin = &loop.completed();
                    break;
                }

                // ------------------------ SEQUENCE ---------------------------
                case ENodeOperation::SEQUENCE: {
                    // Sequence is pure ordering: the runtime fires each leg
                    // in order. The lowering therefore inlines the legs
                    // sequentially into the CURRENT block, threading the
                    // token from one leg's end to the next leg's start — no
                    // dedicated op or region is needed.
                    const auto& seq = static_cast<const SequenceNode&>(*node);
                    llvm::SmallVector<const ExecOutPin*, 4> legs;
                    legs.push_back(&seq.execOutPin());
                    for (auto& extra : seq.execOutPins())
                        legs.push_back(extra.get());

                    mlir::Value tok = in_tok;
                    for (const ExecOutPin* leg : legs) {
                        vm.exec_tok[leg->id()] = tok;
                        LUX_FF_TRY_VALUE(
                            end,
                            lowerChain(
                                bc, vm, *leg, lowered, external, loop_depth
                            )
                        );
                        // A leg that ended in Return/Break terminates the
                        // chain — remaining legs are unreachable (the
                        // runtime aborts the sequence there too).
                        if (!end) return mlir::Value{};
                        tok = end;
                    }

                    // Sequence has no continuation pin of its own — each leg
                    // already carried its chain to its end. Chain ends here;
                    // `tok` is the token after the last leg completed.
                    return tok;
                }

                // -------------------- START / FUNC_DEF -----------------------
                // Both are entry-only. Encountering them mid-chain means the
                // graph contains a back-edge from a later node to the entry,
                // which the runtime should already have rejected. Reject
                // loudly here too.
                case ENodeOperation::START:
                case ENodeOperation::FUNC_DEF_START:
                    LUX_FF_FAIL(bc, "entry node reached mid-chain");

                default:
                    LUX_FF_FAIL(bc, "no lowering registered");
            }
        }
    }

    // =============================================================================
    // getOperand — resolve a DataInPin to an MLIR Value.
    //
    // Resolution order for a linked source:
    //   1. exec_data — values a non-pure node already produced on the exec
    //      chain (call results, loop IVs, set-variable passthroughs).
    //   2. the CURRENT pure scope's cache.
    //   3. materializePureValue — expand the pure subgraph on demand at the
    //      current insertion point (re-evaluation semantics).
    // Unlinked pins fall back to their editor-provided constant.
    // =============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::getOperand(
        const DataInPin& in, ValueMaps& vm, BuilderContext& bc, bool asIdx)
    {
        bc.current_pin = &in;

        if (auto* src = in.linkedPin()) {
            if (auto it = vm.exec_data.find(src->id()); it != vm.exec_data.end())
                return it->second;
            auto& scope = vm.pure_scopes.back();
            if (auto it = scope.find(src->id()); it != scope.end())
                return it->second;
            return materializePureValue(*src, vm, bc);
        }

        // Constant / default value path.
        if (in.allowDefault()) {
            if (!in.validConstant())
                LUX_FF_FAIL_AT_PIN(bc,
                    "pin has no link and no valid default constant");
            LUX_FF_TRY_VALUE(
                cst,
                buildConstant(in, bc, in.constantData(), asIdx)
            );
            vm.pure_scopes.back()[in.id()] = cst;
            return cst;
        }

        LUX_FF_FAIL_AT_PIN(bc, "DataInPin has no source and no default value");
    }

    template<size_t Bits>
    mlir::Value MLIRBuilderImpl::global_constant_assign(BuilderContext& bc, const lux::meta::RuntimeObject& obj, bool is_seq)
    {
        using buffer_type = typename TypeSizeMap<Bits>::type;
        auto& builder = bc.builder;

        if (is_seq)
            return TypeSizeMap<Bits>::getIndex(builder, obj);

        auto llvm_type = TypeSizeMap<Bits>::getLLVMType(builder);
        auto attr      = TypeSizeMap<Bits>::getAttr(builder, obj);
        return builder.create<mlir::LLVM::ConstantOp>(bc.loc, llvm_type, attr);
    }

    mlir::Value MLIRBuilderImpl::global_string_constant_assign(BuilderContext& bc, const char* str_data, size_t str_size)
    {
        llvm::StringRef bytes(str_data, str_size);
        auto it = bc.string_globals.find(bytes);
        if (it == bc.string_globals.end())
        {
            // Storage carries an explicit trailing '\0' so the pointer is a
            // valid C string for native callees. The symbol name is
            // hash-based (NOT the string content) — short, safe for
            // non-ASCII / long values, avoids leaking the literal into the
            // binary's symbol table — with a per-module ordinal appended so
            // two different strings whose hashes collide still get distinct
            // symbols.
            std::string terminated(str_data, str_size);
            terminated.push_back('\0');
            uint64_t h = static_cast<uint64_t>(llvm::hash_value(bytes));
            std::string sym_name = makeGlobalSymbol(
                bc, "s", h, bc.string_globals.size() + 1);

            auto arr_ty = mlir::LLVM::LLVMArrayType::get(
                bc.builder.getI8Type(), terminated.size());
            mlir::OpBuilder::InsertionGuard guard(bc.builder);
            bc.builder.setInsertionPointToStart(bc.module.getBody());
            auto g_obj = bc.builder.create<mlir::LLVM::GlobalOp>(
                bc.loc, arr_ty, /*isConstant=*/true,
                mlir::LLVM::Linkage::Internal, sym_name,
                mlir::StringAttr::get(bc.ctx, terminated));
            it = bc.string_globals.try_emplace(bytes, g_obj).first;
        }

        auto ptr_type = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        auto sym = mlir::FlatSymbolRefAttr::get(bc.ctx, it->second.getSymName());
        return bc.builder.create<mlir::LLVM::AddressOfOp>(bc.loc, ptr_type, sym);
    }

    // ============================================================================
    // buildScalarConstantValue — pin-independent scalar constants.
    // `as_index` (index type requested by for-loop bounds) is only
    // meaningful for integers and is honored inside global_constant_assign<>.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::buildScalarConstantValue(
        BuilderContext& bc, const lux::meta::RefType& rt,
        const lux::meta::RuntimeObject& obj, bool as_index)
    {
        using lux::meta::EBaseType;
        auto& b   = bc.builder;
        auto& loc = bc.loc;

        switch (static_cast<EBaseType>(rt.qtype.base)) {
            case EBaseType::Bool: {
                const bool* p = static_cast<const bool*>(obj.data());
                return b.create<mlir::arith::ConstantOp>(
                    loc, b.getI1Type(), b.getBoolAttr(*p));
            }
            case EBaseType::Float: {
                const float* p = static_cast<const float*>(obj.data());
                return b.create<mlir::arith::ConstantOp>(
                    loc, b.getF32Type(), b.getF32FloatAttr(*p));
            }
            case EBaseType::Double: {
                const double* p = static_cast<const double*>(obj.data());
                return b.create<mlir::arith::ConstantOp>(
                    loc, b.getF64Type(), b.getF64FloatAttr(*p));
            }
            case EBaseType::Int8:
            case EBaseType::Uint8:  return global_constant_assign<8>(bc, obj, as_index);
            case EBaseType::Int16:
            case EBaseType::Uint16: return global_constant_assign<16>(bc, obj, as_index);
            case EBaseType::Int32:
            case EBaseType::Uint32: return global_constant_assign<32>(bc, obj, as_index);
            case EBaseType::Int64:
            case EBaseType::Uint64: return global_constant_assign<64>(bc, obj, as_index);
            default:
                LUX_FF_FAIL(bc, "not a scalar base type");
        }
    }

    // ============================================================================
    // buildConstant — scalars, strings, and trivially-copyable aggregates.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::buildConstant(
        const DataInPin& in,
        BuilderContext& bc,
        const lux::meta::RuntimeObject& obj,
        bool is_seq
    ) {
        using namespace lux::meta;
        auto& b   = bc.builder;
        auto& loc = bc.loc;
        auto& rt  = *in.info().type;

        // Strings FIRST. std::string_view is itself standard-layout and
        // trivially copyable — a byte-copy dispatch placed before this check
        // would embed its {pointer, size} representation (a dangling host
        // pointer) into the module instead of the characters.
        if (obj.type()->hash == lux::cxx::type_hash<std::string>())
        {
            auto* str = static_cast<const std::string*>(obj.data());
            return global_string_constant_assign(bc, str->data(), str->size());
        }
        if (obj.type()->hash == lux::cxx::type_hash<std::string_view>())
        {
            auto* str = static_cast<const std::string_view*>(obj.data());
            return global_string_constant_assign(bc, str->data(), str->size());
        }

        // Scalar base types get first-class MLIR constants. Dispatch on
        // EBaseType, not on byte size: a size-only dispatch would catch
        // float/double in the integer branch and reinterpret their bytes,
        // and would turn small structs into bogus i32/i64 values.
        using lux::meta::EBaseType;
        switch (static_cast<EBaseType>(rt.qtype.base)) {
            case EBaseType::Bool:
            case EBaseType::Float:
            case EBaseType::Double:
            case EBaseType::Int8:
            case EBaseType::Uint8:
            case EBaseType::Int16:
            case EBaseType::Uint16:
            case EBaseType::Int32:
            case EBaseType::Uint32:
            case EBaseType::Int64:
            case EBaseType::Uint64:
                return buildScalarConstantValue(bc, rt, obj, is_seq);
            default:
                break;  // Record / Unknown — aggregate path below
        }

        // Aggregates: materialize the value bytes in constant module storage
        // and hand out a pointer. This matches refTypeToMLIR, which types
        // every non-scalar native-call parameter as !llvm.ptr. Only
        // trivially-copyable values are supported today: storage carries
        // the raw bytes, so no ctor/dtor wrappers are needed and the
        // constant is truly immutable. Non-trivial classes (where a
        // constructor must run over a buffer) are deferred — report a clear
        // diagnostic rather than materializing garbage.
        if (!rt.traits.is_trivially_copyable)
            LUX_FF_FAIL_AT_PIN(bc,
                "non-trivially-copyable class constants are not yet supported");
        const auto& obj_type = *obj.type();
        if (obj_type.size == 0)
            LUX_FF_FAIL_AT_PIN(bc, "aggregate constant has zero size");

        return materializeBytesConstant(bc, obj_type.hash,
            llvm::StringRef(static_cast<const char*>(obj.data()), obj_type.size));
    }

    // ============================================================================
    // materializeBytesConstant — one storage global per (type, value).
    //
    // Cache key is (type_hash, value_hash) so different VALUES of the same
    // type produce independent storage; a per-module ordinal in the symbol
    // name keeps hash collisions from ever aliasing two distinct globals.
    // ============================================================================
    mlir::Value MLIRBuilderImpl::materializeBytesConstant(
        BuilderContext& bc, uint64_t type_hash, llvm::StringRef bytes)
    {
        uint64_t value_hash = static_cast<uint64_t>(llvm::hash_value(bytes));
        std::pair<uint64_t, uint64_t> key{type_hash, value_hash};

        auto it = bc.class_globals.find(key);
        if (it == bc.class_globals.end()) {
            auto arrTy = mlir::LLVM::LLVMArrayType::get(
                bc.builder.getI8Type(), bytes.size());
            std::string g_name = makeGlobalSymbol(
                bc, "g", value_hash, bc.class_globals.size() + 1);

            // Initializer = the actual value bytes. StringAttr is the
            // idiomatic initializer for LLVM array-of-i8 globals.
            auto initBytes = mlir::StringAttr::get(bc.ctx, bytes);

            mlir::OpBuilder::InsertionGuard guard(bc.builder);
            bc.builder.setInsertionPointToStart(bc.module.getBody());

            auto gObj = bc.builder.create<mlir::LLVM::GlobalOp>(
                bc.loc, arrTy, /*isConstant=*/true,
                mlir::LLVM::Linkage::Internal, g_name, initBytes);

            it = bc.class_globals.try_emplace(key, gObj).first;
        }

        auto sym      = mlir::FlatSymbolRefAttr::get(bc.ctx, it->second.getSymName());
        auto ptr_type = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        return bc.builder.create<mlir::LLVM::AddressOfOp>(bc.loc, ptr_type, sym);
    }

    // ============================================================================
    // materializePureValue — on-demand expansion of the pure data subgraph.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::materializePureValue(
        const DataOutPin& src, ValueMaps& vm, BuilderContext& bc)
    {
        const Node* n = src.node();
        if (!isPureDataOp(n->operation()))
            LUX_FF_FAIL_AT_PIN(bc,
                "source value not materialised (its producer has not run "
                "on the exec chain yet)");
        if (!materializing_.insert(n).second)
            LUX_FF_FAIL(bc, "cycle detected in pure data graph");
        auto cycle_guard = llvm::make_scope_exit([&] { materializing_.erase(n); });

        // Error context: point at the pure node while we lower it.
        bc.current_node = n;

        switch (n->operation()) {
            case ENodeOperation::ADD:
            case ENodeOperation::SUBTRACT:
            case ENodeOperation::MULTIPLY:
            case ENodeOperation::DIVIDE:
            case ENodeOperation::MODULO:
            case ENodeOperation::LOGICAL_AND:
            case ENodeOperation::LOGICAL_OR:
            case ENodeOperation::CMP_EQ:
            case ENodeOperation::CMP_NE:
            case ENodeOperation::CMP_LT:
            case ENodeOperation::CMP_LE:
            case ENodeOperation::CMP_GT:
            case ENodeOperation::CMP_GE: {
                LUX_FF_TRY_VALUE(
                    v,
                    lowerBinaryOp(
                        static_cast<const BinaryOpNode&>(*n), vm, bc
                    )
                );
                vm.pure_scopes.back()[src.id()] = v;
                return v;
            }

            case ENodeOperation::NEGATE:
            case ENodeOperation::LOGICAL_NOT: {
                LUX_FF_TRY_VALUE(
                    v,
                    lowerUnaryOp(
                        static_cast<const UnaryOpNode&>(*n), vm, bc
                    )
                );
                vm.pure_scopes.back()[src.id()] = v;
                return v;
            }

            // Pseudo-pure memory read: re-load at EVERY use (never cached)
            // so a Set earlier on the exec chain is always observed.
            case ENodeOperation::GET_VARIABLE: {
                const auto& get = static_cast<const GetVariableNode&>(*n);
                const auto* var =
                    bc.graph ? bc.graph->findVariable(get.variableId()) : nullptr;
                if (!var || !var->type)
                    LUX_FF_FAIL(bc, "graph variable not found");
                LUX_FF_TRY_VALUE(
                    slot,
                    varSlotAddress(get.variableId(), bc)
                );
                return bc.builder.create<mlir::LLVM::LoadOp>(
                    bc.loc, refTypeToMLIR(bc, *var->type), slot);
            }

            // Pseudo-pure memory read off an engine object: byte-offset GEP
            // from the reflected field, re-loaded at every use.
            case ENodeOperation::GET_FIELD: {
                const auto& gf = static_cast<const GetFieldNode&>(*n);
                if (!gf.field())
                    LUX_FF_FAIL(bc, "field access has no reflected field");
                LUX_FF_TRY_VALUE(obj, getOperand(gf.objectPin(), vm, bc));
                if (!mlir::isa<mlir::LLVM::LLVMPointerType>(obj.getType()))
                    LUX_FF_FAIL_AT_PIN(bc,
                        "field access needs an object pointer");
                mlir::Type fty = refTypeToMLIR(bc, gf.field()->type);
                if (mlir::isa<mlir::LLVM::LLVMPointerType>(fty)
                    && !isPointerQual(gf.field()->type))
                    LUX_FF_FAIL(bc,
                        "record-typed fields are not supported yet");
                auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc.ctx);
                mlir::Value gep = bc.builder.create<mlir::LLVM::GEPOp>(
                    bc.loc, ptr_ty, bc.builder.getI8Type(), obj,
                    llvm::ArrayRef<mlir::LLVM::GEPArg>{
                        static_cast<int32_t>(gf.field()->offset) });
                return bc.builder.create<mlir::LLVM::LoadOp>(bc.loc, fty, gep);
            }

            default:
                LUX_FF_FAIL(bc, "no pure lowering registered for this node");
        }
    }

    // ============================================================================
    // coerceScalar — implicit scalar conversion toward a declared type.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::coerceScalar(
        BuilderContext& bc, mlir::Value v, const lux::meta::RefType& dst_rt)
    {
        mlir::Type dst = refTypeToMLIR(bc, dst_rt);
        mlir::Type src = v.getType();
        if (src == dst)
            return v;

        auto& b = bc.builder;
        auto src_int = mlir::dyn_cast<mlir::IntegerType>(src);
        auto dst_int = mlir::dyn_cast<mlir::IntegerType>(dst);
        auto src_flt = mlir::dyn_cast<mlir::FloatType>(src);
        auto dst_flt = mlir::dyn_cast<mlir::FloatType>(dst);
        const bool dst_unsigned = isUnsignedInt(dst_rt);

        // The for-loop induction variable is an MLIR `index`; wiring it into
        // integer arithmetic is the single most common pattern, so cast it.
        if (mlir::isa<mlir::IndexType>(src) && dst_int) {
            return dst_unsigned
                ? b.create<mlir::arith::IndexCastUIOp>(bc.loc, dst, v).getResult()
                : b.create<mlir::arith::IndexCastOp>(bc.loc, dst, v).getResult();
        }

        if (src_int && dst_int) {
            if (src_int.getWidth() < dst_int.getWidth()) {
                // i1 (bool) always zero-extends — sign-extending `true`
                // would produce -1.
                return (dst_unsigned || src_int.getWidth() == 1)
                    ? b.create<mlir::arith::ExtUIOp>(bc.loc, dst, v).getResult()
                    : b.create<mlir::arith::ExtSIOp>(bc.loc, dst, v).getResult();
            }
            return b.create<mlir::arith::TruncIOp>(bc.loc, dst, v).getResult();
        }
        if (src_int && dst_flt) {
            return dst_unsigned  // dst is float; use the source-ish signedness we have
                ? b.create<mlir::arith::UIToFPOp>(bc.loc, dst, v).getResult()
                : b.create<mlir::arith::SIToFPOp>(bc.loc, dst, v).getResult();
        }
        if (src_flt && dst_flt) {
            if (src_flt.getWidth() < dst_flt.getWidth())
                return b.create<mlir::arith::ExtFOp>(bc.loc, dst, v).getResult();
            return b.create<mlir::arith::TruncFOp>(bc.loc, dst, v).getResult();
        }

        LUX_FF_FAIL(bc,
            "no implicit conversion between the linked value's type and the "
            "pin's declared type");
    }

    // ============================================================================
    // lowerBinaryOp / lowerUnaryOp — arith-dialect emission for pure nodes.
    // Signedness comes from the node's declared operand RefType (MLIR
    // integers are signless; the op choice carries the signedness).
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::lowerBinaryOp(
        const BinaryOpNode& bin, ValueMaps& vm, BuilderContext& bc)
    {
        const auto* rt = bin.operandType();
        if (!rt)
            LUX_FF_FAIL(bc, "binary op node has no operand type");

        LUX_FF_TRY_VALUE(lhs_operand, getOperand(bin.lhs(), vm, bc));
        LUX_FF_TRY_VALUE(rhs_operand, getOperand(bin.rhs(), vm, bc));
        LUX_FF_TRY_VALUE(lhs, coerceScalar(bc, lhs_operand, *rt));
        LUX_FF_TRY_VALUE(rhs, coerceScalar(bc, rhs_operand, *rt));

        auto& b = bc.builder;
        auto loc = bc.loc;
        const bool flt = isFloatType(*rt);
        const bool uns = isUnsignedInt(*rt);

        using CmpI = mlir::arith::CmpIPredicate;
        using CmpF = mlir::arith::CmpFPredicate;
        auto cmpi = [&](CmpI s, CmpI u) {
            return b.create<mlir::arith::CmpIOp>(loc, uns ? u : s, lhs, rhs)
                .getResult();
        };
        auto cmpf = [&](CmpF p) {
            return b.create<mlir::arith::CmpFOp>(loc, p, lhs, rhs).getResult();
        };

        switch (bin.operation()) {
            case ENodeOperation::ADD:
                return flt ? b.create<mlir::arith::AddFOp>(loc, lhs, rhs).getResult()
                           : b.create<mlir::arith::AddIOp>(loc, lhs, rhs).getResult();
            case ENodeOperation::SUBTRACT:
                return flt ? b.create<mlir::arith::SubFOp>(loc, lhs, rhs).getResult()
                           : b.create<mlir::arith::SubIOp>(loc, lhs, rhs).getResult();
            case ENodeOperation::MULTIPLY:
                return flt ? b.create<mlir::arith::MulFOp>(loc, lhs, rhs).getResult()
                           : b.create<mlir::arith::MulIOp>(loc, lhs, rhs).getResult();
            case ENodeOperation::DIVIDE:
                if (flt) return b.create<mlir::arith::DivFOp>(loc, lhs, rhs).getResult();
                return uns ? b.create<mlir::arith::DivUIOp>(loc, lhs, rhs).getResult()
                           : b.create<mlir::arith::DivSIOp>(loc, lhs, rhs).getResult();
            case ENodeOperation::MODULO:
                if (flt) return b.create<mlir::arith::RemFOp>(loc, lhs, rhs).getResult();
                return uns ? b.create<mlir::arith::RemUIOp>(loc, lhs, rhs).getResult()
                           : b.create<mlir::arith::RemSIOp>(loc, lhs, rhs).getResult();

            case ENodeOperation::LOGICAL_AND:
                return b.create<mlir::arith::AndIOp>(loc, lhs, rhs).getResult();
            case ENodeOperation::LOGICAL_OR:
                return b.create<mlir::arith::OrIOp>(loc, lhs, rhs).getResult();

            case ENodeOperation::CMP_EQ:
                return flt ? cmpf(CmpF::OEQ) : cmpi(CmpI::eq,  CmpI::eq);
            case ENodeOperation::CMP_NE:
                return flt ? cmpf(CmpF::ONE) : cmpi(CmpI::ne,  CmpI::ne);
            case ENodeOperation::CMP_LT:
                return flt ? cmpf(CmpF::OLT) : cmpi(CmpI::slt, CmpI::ult);
            case ENodeOperation::CMP_LE:
                return flt ? cmpf(CmpF::OLE) : cmpi(CmpI::sle, CmpI::ule);
            case ENodeOperation::CMP_GT:
                return flt ? cmpf(CmpF::OGT) : cmpi(CmpI::sgt, CmpI::ugt);
            case ENodeOperation::CMP_GE:
                return flt ? cmpf(CmpF::OGE) : cmpi(CmpI::sge, CmpI::uge);

            default:
                LUX_FF_FAIL(bc, "not a binary operation");
        }
    }

    FlowForgeResult<mlir::Value> MLIRBuilderImpl::lowerUnaryOp(
        const UnaryOpNode& un, ValueMaps& vm, BuilderContext& bc)
    {
        const auto* rt = un.operandType();
        if (!rt)
            LUX_FF_FAIL(bc, "unary op node has no operand type");

        LUX_FF_TRY_VALUE(operand, getOperand(un.operand(), vm, bc));
        LUX_FF_TRY_VALUE(v, coerceScalar(bc, operand, *rt));
        auto& b = bc.builder;
        auto loc = bc.loc;

        switch (un.operation()) {
            case ENodeOperation::NEGATE: {
                if (isFloatType(*rt))
                    return b.create<mlir::arith::NegFOp>(loc, v).getResult();
                auto zero = b.create<mlir::arith::ConstantOp>(
                    loc, v.getType(), b.getIntegerAttr(v.getType(), 0));
                return b.create<mlir::arith::SubIOp>(loc, zero, v).getResult();
            }
            case ENodeOperation::LOGICAL_NOT: {
                auto one = b.create<mlir::arith::ConstantOp>(
                    loc, b.getI1Type(), b.getBoolAttr(true));
                return b.create<mlir::arith::XOrIOp>(loc, v, one).getResult();
            }
            default:
                LUX_FF_FAIL(bc, "not a unary operation");
        }
    }

    // ============================================================================
    // varSlotAddress — address of a graph variable inside the instance-state
    // block: `state_ptr + layout offset` (byte GEP). Variables are shared
    // across all of the graph's functions because every function receives
    // the same block pointer (Blueprint member-variable semantics), and the
    // binary carries no storage of its own — the HOST allocates the block
    // and initializes it from the layout's defaults blob before invoking.
    // Validation (scalar-ness / default value) already ran in
    // computeStateLayout at the top of generateMLIR.
    // ============================================================================
    FlowForgeResult<mlir::Value>
    MLIRBuilderImpl::varSlotAddress(uint64_t var_id, BuilderContext& bc)
    {
        const auto* field = bc.state_layout.find(var_id);
        if (!field)
            LUX_FF_FAIL(bc, "graph variable not found");
        if (!bc.state_ptr)
            LUX_FF_FAIL(bc, "function has no instance-state pointer");

        auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        return bc.builder.create<mlir::LLVM::GEPOp>(
            bc.loc, ptr_ty, bc.builder.getI8Type(), bc.state_ptr,
            llvm::ArrayRef<mlir::LLVM::GEPArg>{
                static_cast<int32_t>(field->offset) });
    }

    // ============================================================================
    // mergeExecTokens — see header comment on the member declaration.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::mergeExecTokens(
        BuilderContext& bc,
        const llvm::SmallVector<mlir::Value>& inputs) {
        if (inputs.empty())
            LUX_FF_FAIL(bc, "mergeExecTokens called with no inputs");

        // Fast path: all predecessors carry the same SSA value already
        // (no real divergence). Skip emitting a token_merge.
        mlir::Value unique;
        for (auto v : inputs) {
            if (!v) continue;
            if (!unique) { unique = v; continue; }
            if (v != unique) { unique = {}; break; }
        }
        if (unique)
            return unique;

        return bc.builder.create<mlir::flowforge::TokenMergeOp>(
            bc.loc, bc.token, mlir::ValueRange{inputs}).getMerged();
    }

    // ============================================================================
    // Sequential lowering helpers
    // ============================================================================
    FlowForgeResult<void> MLIRBuilderImpl::lowerReturnImpl(
        const Node& n,
        mlir::Value in_tok,
        ValueMaps& vm,
        BuilderContext& bc
    ) {
        // Collect return values from the node's data input pins (works for
        // both ReturnNode — none today — and FuncReturnNode). ReturnOp's
        // $rets is Variadic<AnyType> per FlowForgeOps.td. Scalars are
        // coerced to the pin's declared type, which matches the function's
        // result types by construction (FuncReturnNode mirrors the
        // FuncDefNode signature).
        llvm::SmallVector<mlir::Value, 4> operands;
        operands.push_back(in_tok);
        for (auto* pin : n.inPins()) {
            if (pin->kind() != EPinKind::DATA_IN) continue;
            auto* dpin = static_cast<DataInPin*>(pin);
            LUX_FF_TRY_VALUE(v, getOperand(*dpin, vm, bc));
            if (dpin->info().type
                && !mlir::isa<mlir::LLVM::LLVMPointerType>(
                       refTypeToMLIR(bc, *dpin->info().type))
                && !mlir::isa<mlir::LLVM::LLVMPointerType>(v.getType()))
            {
                LUX_FF_TRY_VALUE(
                    coerced,
                    coerceScalar(bc, v, *dpin->info().type)
                );
                v = coerced;
            }
            operands.push_back(v);
        }
        bc.builder.create<mlir::flowforge::ReturnOp>(
            bc.loc, mlir::TypeRange{}, operands);
        return {};
    }

    // ============================================================================
    // Native function call lowering — emits a func.func declaration at module
    // top and a func.call at the current insertion point, threading the token
    // through as a no-op (sequential calls don't fork the exec edge).
    // ============================================================================
    namespace {
        // Look up or create a func.func DECLARATION (empty body) at module top
        // for the given external symbol. Dedupes by name within one build, so
        // multiple call sites to the same symbol share one declaration.
        static mlir::func::FuncOp getOrDeclareExternFunc(BuilderContext& bc,
            llvm::StringRef name, mlir::FunctionType ty)
        {
            auto it = bc.extern_funcs.find(name);
            if (it != bc.extern_funcs.end()) return it->second;

            mlir::OpBuilder::InsertionGuard guard(bc.builder);
            bc.builder.setInsertionPointToStart(bc.module.getBody());
            // No entry block -> empty body region -> this is a declaration that
            // the JIT (or downstream linker) resolves externally. MLIR requires
            // body-less declarations to be PRIVATE ('symbol declaration cannot
            // have public visibility') — without this the func-to-LLVM lowering
            // verifier rejects the module.
            auto fn = bc.builder.create<mlir::func::FuncOp>(bc.loc, name, ty);
            fn.setPrivate();
            return bc.extern_funcs.try_emplace(name, fn).first->second;
        }
    } // anonymous namespace

    // Alloca hoisted to the entry-block prologue: call sites can sit inside
    // loop bodies, and an alloca AT the call site would grow the stack every
    // iteration. Entry-block allocas are the canonical LLVM idiom.
    static mlir::Value allocaAtEntry(BuilderContext& bc, mlir::Type elem_ty,
                                     int64_t count)
    {
        mlir::OpBuilder::InsertionGuard guard(bc.builder);
        auto* entry = &bc.main_func.getBody().front();
        bc.builder.setInsertionPointToStart(entry);
        auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        auto n = bc.builder.create<mlir::LLVM::ConstantOp>(
            bc.loc, bc.builder.getI32Type(),
            bc.builder.getI32IntegerAttr(static_cast<int32_t>(count)));
        return bc.builder.create<mlir::LLVM::AllocaOp>(
            bc.loc, ptr_ty, elem_ty, n);
    }

    FlowForgeResult<void> MLIRBuilderImpl::lowerNativeCallImpl(
        const Node& n,
        mlir::Value in_tok,
        ValueMaps& vm,
        BuilderContext& bc
    ) {
        const auto& call = static_cast<const NativeFuncCall&>(n);
        auto& info = call.info();
        auto& b    = bc.builder;
        auto  loc  = bc.loc;
        auto  ptr_ty = mlir::LLVM::LLVMPointerType::get(bc.ctx);

        const bool returns_void = static_cast<lux::meta::EBaseType>(
            info.return_type.qtype.base) == lux::meta::EBaseType::Void
            && !isPointerQual(info.return_type);

        // Collect + coerce operands from data input pins (shared by both
        // call flavors): scalars coerce to the declared parameter type,
        // pointer-typed parameters pass through untouched.
        llvm::SmallVector<mlir::Value, 4> operands;
        operands.reserve(call.dataInPins().size());
        for (size_t i = 0; i < call.dataInPins().size(); ++i) {
            LUX_FF_TRY_VALUE(
                v,
                getOperand(*call.dataInPins()[i], vm, bc)
            );
            if (i < info.parameters.size()) {
                const auto& prt = info.parameters[i].type;
                if (!mlir::isa<mlir::LLVM::LLVMPointerType>(refTypeToMLIR(bc, prt))
                    && !mlir::isa<mlir::LLVM::LLVMPointerType>(v.getType()))
                {
                    LUX_FF_TRY_VALUE(coerced, coerceScalar(bc, v, prt));
                    v = coerced;
                }
            }
            operands.push_back(v);
        }

        mlir::Value result_value;   // null when void

        if (info.invoker == nullptr) {
            // ---- hand-written RefFunction: direct C-ABI call -------------
            llvm::SmallVector<mlir::Type, 4> argTypes;
            argTypes.reserve(info.parameters.size());
            for (auto& param : info.parameters)
                argTypes.push_back(refTypeToMLIR(bc, param.type));
            llvm::SmallVector<mlir::Type, 1> retTypes;
            if (!returns_void)
                retTypes.push_back(refTypeToMLIR(bc, info.return_type));

            auto fnTy   = b.getFunctionType(argTypes, retTypes);
            auto funcOp = getOrDeclareExternFunc(bc, info.name, fnTy);
            auto callOp = b.create<mlir::func::CallOp>(loc, funcOp, operands);
            if (!returns_void && callOp.getNumResults() > 0)
                result_value = callOp.getResult(0);
        } else {
            // ---- reflected function: call through the type-erased invoker
            // trampoline `void(void* obj, void** args, void* ret)`. args[i]
            // points at the i-th argument's storage:
            //   * scalar parameter  -> an entry-block slot holding the value
            //   * pointer parameter -> a slot holding the pointer value
            //   * by-value record   -> the object pointer ITSELF (it already
            //     points at a T)
            // The trampoline's address is bound at JIT time under a
            // hash-based symbol (see FlowScriptInstance::invokerSymbol).
            const std::string sym = FlowScriptInstance::invokerSymbol(info);
            auto fnTy = b.getFunctionType({ptr_ty, ptr_ty, ptr_ty}, {});
            auto funcOp = getOrDeclareExternFunc(bc, sym, fnTy);

            auto null_ptr = b.create<mlir::LLVM::ZeroOp>(loc, ptr_ty).getResult();

            mlir::Value args_base = null_ptr;
            if (!operands.empty()) {
                args_base = allocaAtEntry(
                    bc, ptr_ty, static_cast<int64_t>(operands.size()));
                for (size_t i = 0; i < operands.size(); ++i) {
                    mlir::Value storage;
                    const bool operand_is_ptr =
                        mlir::isa<mlir::LLVM::LLVMPointerType>(operands[i].getType());
                    const bool param_is_ptr_qual =
                        i < info.parameters.size()
                        && isPointerQual(info.parameters[i].type);
                    if (operand_is_ptr && !param_is_ptr_qual) {
                        // by-value record: the operand already points at a T.
                        storage = operands[i];
                    } else {
                        // scalar or pointer parameter: spill into a slot.
                        storage = allocaAtEntry(bc, operands[i].getType(), 1);
                        b.create<mlir::LLVM::StoreOp>(loc, operands[i], storage);
                    }
                    auto slot = b.create<mlir::LLVM::GEPOp>(
                        loc, ptr_ty, ptr_ty, args_base,
                        llvm::ArrayRef<mlir::LLVM::GEPArg>{
                            static_cast<int32_t>(i) });
                    b.create<mlir::LLVM::StoreOp>(loc, storage, slot);
                }
            }

            mlir::Value ret_slot = null_ptr;
            mlir::Type  ret_ty;
            if (!returns_void) {
                ret_ty = refTypeToMLIR(bc, info.return_type);
                if (mlir::isa<mlir::LLVM::LLVMPointerType>(ret_ty)
                    && !isPointerQual(info.return_type))
                    LUX_FF_FAIL(bc,
                        "record-typed return values are not supported for "
                        "reflected calls yet");
                ret_slot = allocaAtEntry(bc, ret_ty, 1);
            }

            b.create<mlir::func::CallOp>(
                loc, funcOp, mlir::ValueRange{null_ptr, args_base, ret_slot});
            if (!returns_void)
                result_value = b.create<mlir::LLVM::LoadOp>(loc, ret_ty, ret_slot);
        }

        // Map return value to the result DataOutPin (if not void).
        if (result_value)
            vm.exec_data[call.result().id()] = result_value;

        // Thread exec token through (sequential call): outTok = inTok.
        vm.exec_tok[call.execOutPin().id()] = in_tok;
        return {};
    }

    // ============================================================================
    // MLIRBuilder thin wrapper
    // ============================================================================
    MLIRBuilder::MLIRBuilder(IRContext* ctx)
    {
        impl_ = std::make_unique<MLIRBuilderImpl>(
            static_cast<mlir::MLIRContext*>(ctx->context())
        );
    }

    MLIRBuilder::~MLIRBuilder() = default;
    MLIRBuilder::MLIRBuilder(MLIRBuilder&&) noexcept = default;
    MLIRBuilder& MLIRBuilder::operator=(MLIRBuilder&&) noexcept = default;
    FlowForgeResult<std::unique_ptr<IR>>
    MLIRBuilder::generateIR(const FlowGraph& g) {
        return impl_->generateMLIR(g);
    }

} // namespace lux::flowforge

#undef LUX_FF_TRY
#undef LUX_FF_TRY_VALUE
#undef LUX_FF_FAIL_AT_PIN
#undef LUX_FF_FAIL
