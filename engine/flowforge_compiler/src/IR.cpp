#include <atomic>
#include <functional>
#include <queue>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringMap.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <llvm/Support/raw_ostream.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/IR/BuiltinTypes.h>
#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE
#include "lux/engine/flowforge/mlir/IRImpl.hpp"
#include "lux/engine/flowforge/FlowGraph.hpp"
#include "lux/engine/flowforge/NodeBase.hpp"
#include "lux/engine/flowforge/ControlNode.hpp"
#include "lux/engine/flowforge/FunctionalNode.hpp"

#define THROW_BUILD_ERROR(build_context, msg)                                                       \
    throw BuildTimeError(msg, reinterpret_cast<uintptr_t>(build_context.current_node),              \
                         reinterpret_cast<uintptr_t>(nullptr))
#define THROW_BUILD_ERROR_WITH_PIN(build_context, msg)                                              \
    throw BuildTimeError(msg, reinterpret_cast<uintptr_t>(build_context.current_node),              \
                         reinterpret_cast<uintptr_t>(build_context.current_pin))

// Provide std::hash specializations for unordered containers
namespace std {
    template<>
    struct hash<lux::flowforge::ENodeOperation> {
        size_t operator()(lux::flowforge::ENodeOperation v) const noexcept {
            return static_cast<size_t>(v);
        }
    };
}

namespace lux::flowforge {
    template<size_t Bits> struct TypeSizeMap;
    template<> struct TypeSizeMap<8> {
        using type = uint8_t;
        static auto getLLVMType(mlir::OpBuilder& b) { return b.getI8Type(); }
        static auto getAttr(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint8_t* byte = static_cast<const uint8_t*>(obj.data());
            return b.getIntegerAttr(b.getI8Type(), *byte);
        }
        static auto getIndex(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint8_t* byte = static_cast<const uint8_t*>(obj.data());
            return b.create<mlir::arith::ConstantIndexOp>(b.getUnknownLoc(), *byte);
        }
    };
    template<> struct TypeSizeMap<16> {
        using type = uint16_t;
        static auto getLLVMType(mlir::OpBuilder& b) { return FLOWFORGE_GET_I16_TYPE(b); }
        static auto getAttr(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint16_t* byte = static_cast<const uint16_t*>(obj.data());
            return b.getIntegerAttr(FLOWFORGE_GET_I16_TYPE(b), *byte);
        }
        static auto getIndex(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint16_t* byte = static_cast<const uint16_t*>(obj.data());
            return b.create<mlir::arith::ConstantIndexOp>(b.getUnknownLoc(), *byte);
        }
    };
    template<> struct TypeSizeMap<32> {
        using type = uint32_t;
        static auto getLLVMType(mlir::OpBuilder& b) { return b.getI32Type(); }
        static auto getAttr(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint32_t* byte = static_cast<const uint32_t*>(obj.data());
            return b.getIntegerAttr(b.getI32Type(), *byte);
        }
        static auto getIndex(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint32_t* byte = static_cast<const uint32_t*>(obj.data());
            return b.create<mlir::arith::ConstantIndexOp>(b.getUnknownLoc(), *byte);
        }
    };
    template<> struct TypeSizeMap<64> {
        using type = uint64_t;
        static auto getLLVMType(mlir::OpBuilder& b) { return b.getI64Type(); }
        static auto getAttr(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint64_t* byte = static_cast<const uint64_t*>(obj.data());
            return b.getIntegerAttr(b.getI64Type(), *byte);
        }
        static auto getIndex(mlir::OpBuilder& b, const lux::meta::RuntimeObject& obj) {
            const uint64_t* byte = static_cast<const uint64_t*>(obj.data());
            return b.create<mlir::arith::ConstantIndexOp>(b.getUnknownLoc(), *byte);
        }
    };


    // ============================================================================
    // IR proxy
    // ============================================================================
    IR::IR() { impl_ = std::make_unique<IRImpl>(); }
    IR::~IR() = default;
    IRImpl& IR::impl() { return *impl_; }
    const IRImpl& IR::impl() const { return *impl_; }
    std::string IR::toString() const {
        std::string s;
        llvm::raw_string_ostream os(s);

        // Check if top_module is valid before printing
        if (impl_->top_module && impl_->top_module.getOperation()) {
            try {
                impl_->top_module.print(os);
            } catch (...) {
                os << "// Error printing IR - invalid module\n";
            }
        } else {
            os << "// Empty IR - module not generated yet\n";
        }
        return s;
    }

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
            module = builder.create<mlir::ModuleOp>(loc);
            token  = mlir::flowforge::FLOWFORGE_TOKEN_TYPE::get(ctx);
        }

        mlir::MLIRContext* ctx;
        mlir::OpBuilder    builder;
        mlir::Location     loc;
        mlir::Type         token;             // flowforge::FLOWFORGE_TOKEN_TYPE
        mlir::ModuleOp     module;
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

        const Node* current_node = nullptr;
        const Pin*  current_pin  = nullptr;

    private:
        static std::atomic<uint32_t> next_module_id;
    };

    std::atomic<uint32_t> BuilderContext::next_module_id{1};

    // Format a unique LLVM global symbol name for this BuilderContext.
    //   _lf_<module_id>_<kind>_<a>[_<b>]   (all hex)
    //
    // kind: short tag ("g" = class storage, "s" = string).
    // a, b: opaque hashes (e.g. class_hash, value_hash). b=0 omits the suffix.
    static std::string makeGlobalSymbol(BuilderContext& bc, const char* kind,
                                        uint64_t a, uint64_t b = 0) {
        std::ostringstream oss;
        oss << "_lf_" << std::hex << bc.module_id << '_' << kind << '_' << a;
        if (b) oss << '_' << b;
        return oss.str();
    }

    IRContext::IRContext()
    {
        context_ = new mlir::MLIRContext;
        auto* mlir_context = static_cast<mlir::MLIRContext*>(context_);
        mlir_context->loadDialect<mlir::flowforge::FlowForgeDialect>();
        mlir_context->loadDialect<mlir::LLVM::LLVMDialect>();
        mlir_context->loadDialect<mlir::func::FuncDialect>();
        mlir_context->loadDialect<mlir::arith::ArithDialect>();
    }

    IRContext::IRContext(IRContext&& o) noexcept
        : context_(o.context_)
    {
        o.context_ = nullptr;
    }

    IRContext& IRContext::operator=(IRContext&& o) noexcept
    {
        if (this != &o) {
            delete static_cast<mlir::MLIRContext*>(context_);
            context_ = o.context_;
            o.context_ = nullptr;
        }
        return *this;
    }

    IRContext::~IRContext()
    {
        delete static_cast<mlir::MLIRContext*>(context_);
    }

    //==============================================================================
    // MLIRBuilderImpl — drives FlowGraph -> FlowForge dialect IR generation.
    //==============================================================================
    class MLIRBuilderImpl {
        // ExecOutPin id -> SSA value mapping (per-build). Tokens for exec
        // edges, real values for data edges.
        struct ValueMaps {
            llvm::DenseMap<uint64_t, mlir::Value> exec_tok;
            llvm::DenseMap<uint64_t, mlir::Value> data_val;

            // Explicit lookup helper. Use this instead of `vm.exec_tok[id]`:
            // operator[] silently default-constructs a null mlir::Value on
            // miss, which downstream code happily uses as an operand and
            // crashes during op verification with a confusing "operand 0
            // was null" message. requireExecTok fails loudly at the actual
            // missing-link site with a BuildTimeError that points back to
            // the offending node.
            mlir::Value requireExecTok(uint64_t pin_id, BuilderContext& bc) const {
                auto it = exec_tok.find(pin_id);
                if (it == exec_tok.end() || !it->second)
                    THROW_BUILD_ERROR(bc, "exec token not materialised");
                return it->second;
            }

            llvm::SmallVector<mlir::Value>
                gatherPredTokens(const ExecInPin& in, BuilderContext& bc) const {
                llvm::SmallVector<mlir::Value> preds;
                for (auto* ex : in.linkedPins()) {
                    auto it = exec_tok.find(ex->id());
                    if (it == exec_tok.end() || !it->second)
                        THROW_BUILD_ERROR_WITH_PIN(bc, "exec token not materialised");
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
        std::unique_ptr<IR> generateMLIR(const FlowGraph&);

    private:
        mlir::Value getOperand(const DataInPin&, ValueMaps&, BuilderContext&, bool asIndex = false);
        mlir::Value buildConstant(const DataInPin&, BuilderContext&, const lux::meta::RuntimeObject&, bool asIndex = false);
        mlir::Value materializeClassConstant(BuilderContext&, const lux::meta::RefClass&, const lux::meta::RuntimeObject&);

        // Merge convergent control tokens into a single SSA value.
        // - 0 inputs: build error (the caller's exec-in pin has no link).
        // - 1 input: return it directly.
        // - >1 inputs all identical: return the unique value.
        // - >1 inputs with distinct SSA: emit `flowforge.token_merge`. The
        //   FlowForge -> LLVM lowering pass realises this as a basic-block
        //   PHI in the join block.
        mlir::Value mergeExecTokens(BuilderContext&,
                                    const llvm::SmallVector<mlir::Value>&);

        template<size_t Bits>
        mlir::Value global_constant_assign(BuilderContext&, const lux::meta::RuntimeObject&, bool asIndex);
        mlir::Value global_big_constant_assign(size_t bits, BuilderContext&, const lux::meta::RuntimeObject&);
        mlir::Value global_string_constant_assign(BuilderContext&, const char*, size_t);

        // Sequential per-node lowering helpers — no region recursion. Control
        // ops (Branch / ForLoop / WhileLoop / Sequence) are inlined in
        // lowerChain's switch since each needs to set up its own region(s)
        // and recurse.
        void lowerReturnImpl(const Node&, mlir::Value in_tok, ValueMaps&, BuilderContext&);
        void lowerNativeCallImpl(const Node&, mlir::Value in_tok, ValueMaps&, BuilderContext&);

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
        //   region's yield should use.
        std::unordered_set<const Node*>
            reachableFromPin(const ExecOutPin* pin) const;
        const Node* findPostDom(const Node* branch) const;
        mlir::Value lowerChain(BuilderContext& bc, ValueMaps& vm,
                               const ExecOutPin& start_pin,
                               std::unordered_set<const Node*>& lowered,
                               const std::unordered_set<const Node*>& external);

        mlir::MLIRContext* context_;
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
    std::unique_ptr<IR> MLIRBuilderImpl::generateMLIR(const FlowGraph& g)
    {
        BuilderContext bc(context_);
        bc.builder.setInsertionPointToStart(bc.module.getBody());

        // Locate the single entry node (multi-entry rejected).
        const Node* start = nullptr;
        for (auto& storage : g.nodes()) {
            auto op = storage.node->operation();
            if (op == ENodeOperation::START || op == ENodeOperation::FUNC_DEF_START) {
                if (start) THROW_BUILD_ERROR(bc, "graph has more than one entry node");
                start = storage.node.get();
            }
        }
        if (!start) THROW_BUILD_ERROR(bc, "no start node found");
        bc.current_node = start;

        // Wrap the flow-graph body in a func.func @main so the emitted IR
        // is structurally compatible with the standard MLIR lowering
        // pipeline (FlowForge -> SCF -> LLVM, then JIT). Without the
        // wrapper, ops would land directly under builtin.module, which is
        // NoTerminator while flowforge.return is a Terminator — any
        // subsequent conversion pass rejects that immediately.
        //
        // Return type today is void: the FlowGraph runtime's ReturnNode
        // exposes no DataInPin, so the function has no values to return.
        // When that runtime gap closes, the function type will be derived
        // from ReturnNode's data-pin types.
        auto mainFnTy = bc.builder.getFunctionType({}, {});
        bc.main_func = bc.builder.create<mlir::func::FuncOp>(
            bc.loc, "main", mainFnTy);
        auto* entry_block = bc.main_func.addEntryBlock();
        bc.builder.setInsertionPointToStart(entry_block);

        ValueMaps vm;
        std::unordered_set<const Node*> lowered{start};

        // Emit the entry token. Both START and FUNC_DEF_START currently
        // lower to flowforge.start (FUNC_DEF_START's func-arg surfacing is
        // still TBD). The single out-pin carries the initial exec token.
        auto entry_tok = bc.builder.create<mlir::flowforge::StartOp>(
            bc.loc, bc.token).getResult();

        // No RTTI: the entry node is identified by its operation() tag (1:1 with
        // the concrete type) then static_cast; a null/other node falls through to
        // the error, exactly as the dynamic_casts did.
        const ExecOutPin* entry_pin = nullptr;
        const ENodeOperation entry_op =
            start ? start->operation() : ENodeOperation::INVALID;
        if (entry_op == ENodeOperation::START) {
            entry_pin = &static_cast<const StartNode*>(start)->execOutPin();
        } else if (entry_op == ENodeOperation::FUNC_DEF_START) {
            entry_pin = &static_cast<const FuncDefNode*>(start)->execOutPin();
        } else {
            THROW_BUILD_ERROR(bc, "unsupported entry node type");
        }
        vm.exec_tok[entry_pin->id()] = entry_tok;

        lowerChain(bc, vm, *entry_pin, lowered, /*external=*/{});

        auto ir = std::make_unique<IR>();
        ir->impl().top_module = bc.module;
        return ir;
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
        if (!pin) return reach;
        const ExecInPin* in = pin->nextPin();
        if (!in) return reach;

        std::function<void(const Node*)> dfs = [&](const Node* n) {
            if (!reach.insert(n).second) return;
            for (const Pin* p : n->outPins()) {
                if (p->kind() != EPinKind::EXEC_OUT) continue;
                auto* ex = static_cast<const ExecOutPin*>(p);
                if (auto* dst = ex->nextPin()) dfs(dst->node());
            }
        };
        dfs(in->node());
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
    mlir::Value MLIRBuilderImpl::lowerChain(
        BuilderContext& bc, ValueMaps& vm,
        const ExecOutPin& start_pin,
        std::unordered_set<const Node*>& lowered,
        const std::unordered_set<const Node*>& external)
    {
        const ExecOutPin* cur_pin = &start_pin;
        mlir::Value cur_tok = vm.requireExecTok(cur_pin->id(), bc);

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
                auto preds = vm.gatherPredTokens(*next_in, bc);
                in_tok = mergeExecTokens(bc, preds);
            }

            switch (node->operation()) {
                // ------------------------- RETURN ----------------------------
                case ENodeOperation::RETURN: {
                    lowerReturnImpl(*node, in_tok, vm, bc);
                    return cur_tok;     // terminator — chain has no post-token
                }

                // ----------------------- NATIVE_CALL -------------------------
                case ENodeOperation::NATIVE_FUNC_CALL: {
                    lowerNativeCallImpl(*node, in_tok, vm, bc);
                    const auto& call = static_cast<const NativeFuncCall&>(*node);
                    cur_tok = vm.requireExecTok(call.execOutPin().id(), bc);
                    cur_pin = &call.execOutPin();
                    break;
                }

                // ------------------------- BRANCH ----------------------------
                case ENodeOperation::BRANCH: {
                    const auto& br = static_cast<const BranchNode&>(*node);
                    auto cond = getOperand(br.dataInPin(), vm, bc);

                    const Node* pd = findPostDom(node);
                    auto inner_ext = external;
                    if (pd) inner_ext.insert(pd);

                    auto op = bc.builder.create<mlir::flowforge::BranchOp>(
                        bc.loc, mlir::TypeRange{bc.token, bc.token},
                        mlir::ValueRange{in_tok, cond});

                    // then-region — block-arg(0) = the input token for this leg
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, FLOWFORGE_GET_THEN_REGION(op),
                            mlir::TypeRange{bc.token}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto blk_arg = blk->getArgument(0);
                        vm.exec_tok[br.execOutPinUp().id()] = blk_arg;
                        mlir::Value end = lowerChain(
                            bc, vm, br.execOutPinUp(), lowered, inner_ext);
                        if (!end) end = blk_arg;
                        bc.builder.create<mlir::flowforge::YieldOp>(bc.loc, end);
                    }
                    // else-region — same shape
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, FLOWFORGE_GET_ELSE_REGION(op),
                            mlir::TypeRange{bc.token}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto blk_arg = blk->getArgument(0);
                        vm.exec_tok[br.execOutPinDown().id()] = blk_arg;
                        mlir::Value end = lowerChain(
                            bc, vm, br.execOutPinDown(), lowered, inner_ext);
                        if (!end) end = blk_arg;
                        bc.builder.create<mlir::flowforge::YieldOp>(bc.loc, end);
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
                        // Legs never reconverge — outer chain ends here.
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
                        THROW_BUILD_ERROR(bc, "post-dominator has no exec_in link");
                    cur_pin = pd_exec_in->linkedPins().front();
                    cur_tok = vm.requireExecTok(cur_pin->id(), bc);
                    break;
                }

                // ----------------------- FOR_LOOP ----------------------------
                case ENodeOperation::FOR_LOOP: {
                    const auto& loop = static_cast<const ForLoopNode&>(*node);
                    auto first = getOperand(loop.first_index(), vm, bc, /*asIdx=*/true);
                    auto last  = getOperand(loop.lastIndex(),   vm, bc, /*asIdx=*/true);
                    auto idxTy = bc.builder.getIndexType();

                    auto op = bc.builder.create<mlir::flowforge::ForLoopOp>(
                        bc.loc,
                        mlir::TypeRange{bc.token, bc.token, idxTy},
                        mlir::ValueRange{in_tok, first, last});

                    // body region — args = (per-iter token, iv).
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, op.getBodyRegion(),
                            mlir::TypeRange{bc.token, idxTy}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto body_arg = blk->getArgument(0);
                        auto iv_arg   = blk->getArgument(1);
                        vm.exec_tok[loop.loopBody().id()] = body_arg;
                        vm.data_val[loop.indexPin().id()] = iv_arg;
                        mlir::Value body_end = lowerChain(
                            bc, vm, loop.loopBody(), lowered, external);
                        if (!body_end) body_end = body_arg;
                        bc.builder.create<mlir::flowforge::YieldOp>(
                            bc.loc, mlir::ValueRange{body_end, iv_arg});
                    }

                    cur_tok = op.getResult(1);
                    vm.exec_tok[loop.completed().id()] = cur_tok;
                    cur_pin = &loop.completed();
                    break;
                }

                // ----------------------- WHILE_LOOP --------------------------
                case ENodeOperation::WHILE_LOOP: {
                    const auto& loop = static_cast<const WhileLoopNode&>(*node);
                    auto initCond = getOperand(loop.dataInPin(), vm, bc);

                    auto op = bc.builder.create<mlir::flowforge::WhileLoopOp>(
                        bc.loc,
                        mlir::TypeRange{bc.token, bc.token},
                        mlir::ValueRange{in_tok, initCond});

                    // cond region — passes through (no per-iter cond
                    // expression in FlowGraph yet; the initial cond is the
                    // loop's whole-of-life condition).
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, FLOWFORGE_GET_COND_REGION(op),
                            mlir::TypeRange{bc.token, bc.builder.getI1Type()}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto tokArg  = blk->getArgument(0);
                        auto condArg = blk->getArgument(1);
                        bc.builder.create<mlir::flowforge::CondYieldOp>(
                            bc.loc, tokArg, condArg);
                    }
                    // body region — recursive
                    {
                        auto* blk = addSingleBlockWithArgs(
                            bc.builder, op.getBodyRegion(),
                            mlir::TypeRange{bc.token}, bc.loc);
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto body_arg = blk->getArgument(0);
                        vm.exec_tok[loop.loopBody().id()] = body_arg;
                        mlir::Value body_end = lowerChain(
                            bc, vm, loop.loopBody(), lowered, external);
                        if (!body_end) body_end = body_arg;
                        bc.builder.create<mlir::flowforge::YieldOp>(bc.loc, body_end);
                    }

                    cur_tok = op.getResult(1);
                    vm.exec_tok[loop.completed().id()] = cur_tok;
                    cur_pin = &loop.completed();
                    break;
                }

                // ------------------------ SEQUENCE ---------------------------
                case ENodeOperation::SEQUENCE: {
                    // SequenceOp's body is ONE region yielding N tokens; each
                    // result is one leg's out-token. The legs themselves are
                    // independent sub-chains; we lower them all in the single
                    // body block sequentially (which matches the runtime
                    // "fan-out N successors in order" semantics).
                    const auto& seq = static_cast<const SequenceNode&>(*node);
                    const size_t total = 1 + seq.execOutPins().size();
                    llvm::SmallVector<mlir::Type> rtys(total, bc.token);

                    auto op = bc.builder.create<mlir::flowforge::SequenceOp>(
                        bc.loc, rtys, mlir::ValueRange{in_tok});

                    auto* blk = addSingleBlockWithArgs(
                        bc.builder, op.getRegion(),
                        mlir::TypeRange{bc.token}, bc.loc);
                    {
                        mlir::OpBuilder::InsertionGuard guard(bc.builder);
                        bc.builder.setInsertionPointToEnd(blk);
                        auto seqTok = blk->getArgument(0);
                        llvm::SmallVector<mlir::Value> ends;
                        ends.reserve(total);

                        vm.exec_tok[seq.execOutPin().id()] = seqTok;
                        mlir::Value e = lowerChain(
                            bc, vm, seq.execOutPin(), lowered, external);
                        ends.push_back(e ? e : seqTok);
                        for (auto& extra : seq.execOutPins()) {
                            vm.exec_tok[extra->id()] = seqTok;
                            mlir::Value xe = lowerChain(
                                bc, vm, *extra, lowered, external);
                            ends.push_back(xe ? xe : seqTok);
                        }
                        bc.builder.create<mlir::flowforge::YieldOp>(bc.loc, ends);
                    }

                    // Re-publish each leg's pin -> per-leg op.getResult so
                    // outer-scope consumers see the right token.
                    vm.exec_tok[seq.execOutPin().id()] = op.getResult(0);
                    for (size_t i = 0; i < seq.execOutPins().size(); ++i)
                        vm.exec_tok[seq.execOutPins()[i]->id()] = op.getResult(i + 1);

                    // Sequence has no single canonical "primary continuation"
                    // at the outer scope — each leg already produced its own
                    // continuation, and any consumer must reach that consumer
                    // node directly (which lowerChain's `lowered`-set guards
                    // anyway). Outer chain ends here.
                    return cur_tok;
                }

                // -------------------- START / FUNC_DEF -----------------------
                // Both are entry-only. Encountering them mid-chain means the
                // graph contains a back-edge from a later node to the entry,
                // which the runtime should already have rejected. Reject
                // loudly here too.
                case ENodeOperation::START:
                case ENodeOperation::FUNC_DEF_START:
                    THROW_BUILD_ERROR(bc, "entry node reached mid-chain");

                default:
                    THROW_BUILD_ERROR(bc, "no lowering registered");
            }
        }
    }

    // =============================================================================
    // getOperand — resolve a DataInPin to an MLIR Value, materializing
    // constants on demand.
    // =============================================================================
    mlir::Value MLIRBuilderImpl::getOperand(
        const DataInPin& in, ValueMaps& vm, BuilderContext& bc, bool asIdx)
    {
        bc.current_pin = &in;

        // Linked pin must already be materialized.
        if (auto* src = in.linkedPin()) {
            auto it = vm.data_val.find(src->id());
            if (it == vm.data_val.end())
                THROW_BUILD_ERROR_WITH_PIN(bc, "source value not materialised");
            return it->second;
        }

        // Constant / default value path.
        if (in.allowDefault()) {
            if (!in.validConstant() && in.isNecessary())
                THROW_BUILD_ERROR_WITH_PIN(bc, "necessary pin missing");
            auto cst = buildConstant(in, bc, in.constantData(), asIdx);
            vm.data_val[in.id()] = cst;
            return cst;
        }

        THROW_BUILD_ERROR_WITH_PIN(bc, "DataInPin has no source and no default value");
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

    mlir::Value MLIRBuilderImpl::global_big_constant_assign(size_t bits, BuilderContext& bc, const lux::meta::RuntimeObject& obj)
    {
        auto& builder = bc.builder;
        size_t byte_num = bits / 8;
        auto llvm_type = mlir::LLVM::LLVMArrayType::get(builder.getI8Type(), byte_num);
        mlir::SmallVector<mlir::Attribute> attributes;
        attributes.reserve(byte_num);
        const uint8_t* byte = static_cast<const uint8_t*>(obj.data());
        for (size_t i = 0; i < byte_num; ++i) {
            auto attr = builder.getIntegerAttr(builder.getI8Type(), byte[i]);
            attributes.push_back(attr);
        }
        auto attr = builder.getArrayAttr(attributes);

        return builder.create<mlir::LLVM::ConstantOp>(bc.loc, llvm_type, attr);
    }

    mlir::Value MLIRBuilderImpl::global_string_constant_assign(BuilderContext& bc, const char* str_data, size_t str_size)
    {
        llvm::StringRef bytes(str_data, str_size);
        auto it = bc.string_globals.find(bytes);
        if (it == bc.string_globals.end())
        {
            // Symbol name is hash-based (NOT the string content) — short,
            // safe for non-ASCII / long values, avoids leaking the literal
            // into the binary's symbol table.
            uint64_t h = static_cast<uint64_t>(llvm::hash_value(bytes));
            std::string sym_name = makeGlobalSymbol(bc, "s", h);

            auto addrof = mlir::LLVM::createGlobalString(
                bc.loc, bc.builder, sym_name, bytes, mlir::LLVM::Linkage::Internal
            );
            auto g_obj = llvm::cast<mlir::LLVM::GlobalOp>(addrof.getDefiningOp());
            it = bc.string_globals.try_emplace(bytes, g_obj).first;
        }

        auto ptr_type = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        auto sym = mlir::FlatSymbolRefAttr::get(bc.ctx, it->second.getSymName());
        return bc.builder.create<mlir::LLVM::AddressOfOp>(bc.loc, ptr_type, sym);
    }

    // ============================================================================
    // buildConstant — POD types and non-trivial classes.
    // ============================================================================
    mlir::Value MLIRBuilderImpl::buildConstant(const DataInPin& in, BuilderContext& bc, const lux::meta::RuntimeObject& obj, bool is_seq) {
        using namespace lux::meta;
        auto& b   = bc.builder;
        auto& loc = bc.loc;
        auto& rt  = *in.info().type;

        if (rt.traits.is_standard_layout && rt.traits.is_trivially_copyable)
        {
            // Dispatch on EBaseType FIRST so float/double get f32/f64
            // arith.constant. A size-only dispatch would catch them in the
            // integer branch and reinterpret the bytes as i32/i64.
            using lux::meta::EBaseType;
            auto base = static_cast<EBaseType>(rt.qtype.base);

            if (base == EBaseType::Bool)
            {
                const bool* p = static_cast<const bool*>(obj.data());
                return b.create<mlir::arith::ConstantOp>(
                    loc, b.getI1Type(), b.getBoolAttr(*p));
            }
            if (base == EBaseType::Float)
            {
                const float* p = static_cast<const float*>(obj.data());
                return b.create<mlir::arith::ConstantOp>(
                    loc, b.getF32Type(), b.getF32FloatAttr(*p));
            }
            if (base == EBaseType::Double)
            {
                const double* p = static_cast<const double*>(obj.data());
                return b.create<mlir::arith::ConstantOp>(
                    loc, b.getF64Type(), b.getF64FloatAttr(*p));
            }

            // Integer / generic-bytes path. Note `is_seq` (index requested by
            // for-loop bounds) only meaningful for integer types and is honored
            // inside global_constant_assign<>.
            if (rt.size == 1) return global_constant_assign<8>(bc, obj, is_seq);
            if (rt.size == 2) return global_constant_assign<16>(bc, obj, is_seq);
            if (rt.size == 4) return global_constant_assign<32>(bc, obj, is_seq);
            if (rt.size == 8) return global_constant_assign<64>(bc, obj, is_seq);
            if (rt.size  > 0) return global_big_constant_assign(rt.size * 8 /* bytes -> bits */, bc, obj);
            THROW_BUILD_ERROR(bc, "POD type has zero size");
        }

        if (obj.type()->hash == lux::cxx::type_hash<std::string>())
        {
            auto* str = static_cast<const std::string*>(obj.data());
            return global_string_constant_assign(bc, str->data(), str->size());
        }
        else if (obj.type()->hash == lux::cxx::type_hash<std::string_view>())
        {
            auto* str = static_cast<const std::string_view*>(obj.data());
            return global_string_constant_assign(bc, str->data(), str->size());
        }

        // class / non-trivial ext
        auto* cls = static_cast<const RefClass*>(rt.ptr);
        if (!cls)
            THROW_BUILD_ERROR_WITH_PIN(bc, "RefType.ptr is null (expect RefClass)");

        return materializeClassConstant(bc, *cls, obj);
    }

    // ============================================================================
    // materializeClassConstant — create one storage global per (class, value).
    //
    // Only trivially-copyable classes are supported today: storage carries
    // the value bytes directly via a StringAttr initializer (valid for an
    // LLVM [N x i8] array), so no ctor/dtor wrappers are needed and the
    // constant is truly immutable. Non-trivial classes (where
    // construct_symbol must be called on a buffer) need bytes-injection +
    // ctor invocation and are deferred — we throw a clear diagnostic rather
    // than silently producing a default-constructed singleton.
    //
    // Cache key is (class_hash, value_hash) so different VALUES of the same
    // class produce independent storage.
    // ============================================================================
    mlir::Value MLIRBuilderImpl::materializeClassConstant(
        BuilderContext& bc,
        const lux::meta::RefClass& cls,
        const lux::meta::RuntimeObject& obj)
    {
        if (!cls.type.traits.is_trivially_copyable)
            THROW_BUILD_ERROR_WITH_PIN(bc,
                "non-trivially-copyable class constants are not yet supported");

        auto& b   = bc.builder;
        auto  loc = bc.loc;
        auto  ctx = bc.ctx;

        uint64_t value_hash = static_cast<uint64_t>(llvm::hash_value(
            llvm::StringRef(static_cast<const char*>(obj.data()), cls.type.size)));
        std::pair<uint64_t, uint64_t> key{cls.hash, value_hash};

        auto it = bc.class_globals.find(key);
        if (it == bc.class_globals.end()) {
            auto arrTy = mlir::LLVM::LLVMArrayType::get(b.getI8Type(), cls.type.size);
            std::string g_name = makeGlobalSymbol(bc, "g", cls.hash, value_hash);

            // Initializer = the actual object bytes. StringAttr is the
            // idiomatic initializer for LLVM array-of-i8 globals (same form
            // mlir::LLVM::createGlobalString uses internally).
            auto initBytes = mlir::StringAttr::get(ctx,
                llvm::StringRef(static_cast<const char*>(obj.data()), cls.type.size));

            mlir::OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(bc.module.getBody());

            auto gObj = b.create<mlir::LLVM::GlobalOp>(
                loc, arrTy, /*isConstant=*/true,
                mlir::LLVM::Linkage::Internal, g_name, initBytes);

            it = bc.class_globals.try_emplace(key, gObj).first;
        }

        auto sym      = mlir::FlatSymbolRefAttr::get(ctx, it->second.getSymName());
        auto ptr_type = mlir::LLVM::LLVMPointerType::get(ctx);
        return bc.builder.create<mlir::LLVM::AddressOfOp>(loc, ptr_type, sym);
    }

    // ============================================================================
    // mergeExecTokens — see header comment on the member declaration.
    // ============================================================================
    mlir::Value MLIRBuilderImpl::mergeExecTokens(
        BuilderContext& bc,
        const llvm::SmallVector<mlir::Value>& inputs) {
        if (inputs.empty())
            THROW_BUILD_ERROR(bc, "mergeExecTokens called with no inputs");

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
    void MLIRBuilderImpl::lowerReturnImpl(const Node& n, mlir::Value in_tok,
                                          ValueMaps& vm, BuilderContext& bc) {
        const auto& ret = static_cast<const ReturnNode&>(n);
        // Collect return values from the node's data input pins. ReturnOp's
        // $rets is Variadic<AnyType> per FlowForgeOps.td.
        llvm::SmallVector<mlir::Value, 4> operands;
        operands.push_back(in_tok);
        for (auto* pin : ret.inPins()) {
            if (pin->kind() != EPinKind::DATA_IN) continue;
            operands.push_back(getOperand(*static_cast<DataInPin*>(pin), vm, bc));
        }
        bc.builder.create<mlir::flowforge::ReturnOp>(
            bc.loc, mlir::TypeRange{}, operands);
    }

    // ============================================================================
    // Native function call lowering — emits a func.func declaration at module
    // top and a func.call at the current insertion point, threading the token
    // through as a no-op (sequential calls don't fork the exec edge).
    // ============================================================================
    namespace {
        // Convert a reflected RefType to an MLIR type. Only the primitive
        // bases get specific MLIR types; pointers/records and anything
        // unknown fall back to llvm.ptr. Signedness (int vs uint) is not
        // encoded in the MLIR integer type itself.
        static mlir::Type refTypeToMLIR(BuilderContext& bc, const lux::meta::RefType& rt) {
            using lux::meta::EBaseType;
            auto& b = bc.builder;
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

    void MLIRBuilderImpl::lowerNativeCallImpl(const Node& n, mlir::Value in_tok,
                                              ValueMaps& vm, BuilderContext& bc) {
        const auto& call = static_cast<const NativeFuncCall&>(n);
        auto& info = call.info();
        auto& b    = bc.builder;

        // Build MLIR FunctionType from RefInvokable.
        llvm::SmallVector<mlir::Type, 4> argTypes;
        argTypes.reserve(info.parameters.size());
        for (auto& param : info.parameters)
            argTypes.push_back(refTypeToMLIR(bc, param.type));

        const bool returns_void = static_cast<lux::meta::EBaseType>(
            info.return_type.qtype.base) == lux::meta::EBaseType::Void;

        llvm::SmallVector<mlir::Type, 1> retTypes;
        if (!returns_void)
            retTypes.push_back(refTypeToMLIR(bc, info.return_type));

        auto fnTy   = b.getFunctionType(argTypes, retTypes);
        auto funcOp = getOrDeclareExternFunc(bc, info.name, fnTy);

        // Collect operands from data input pins.
        llvm::SmallVector<mlir::Value, 4> operands;
        operands.reserve(call.dataInPins().size());
        for (auto& pin : call.dataInPins())
            operands.push_back(getOperand(*pin, vm, bc));

        // Emit the call.
        auto callOp = b.create<mlir::func::CallOp>(bc.loc, funcOp, operands);

        // Map return value to the result DataOutPin (if not void).
        if (!returns_void && callOp.getNumResults() > 0)
            vm.data_val[call.result().id()] = callOp.getResult(0);

        // Thread exec token through (sequential call): outTok = inTok.
        vm.exec_tok[call.execOutPin().id()] = in_tok;
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
    std::unique_ptr<IR> MLIRBuilder::generateIR(const FlowGraph& g) {
        return impl_->generateMLIR(g);
    }

} // namespace lux::flowforge
