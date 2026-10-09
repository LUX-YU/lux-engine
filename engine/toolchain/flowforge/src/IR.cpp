#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/raw_ostream.h>
#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <memory>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/Diagnostics.h>
#include <mlir/IR/Verifier.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include FLOWFORGE_ARITH_DIALECT_INCLUDE
#include "lux/engine/flowforge/ControlNodes.hpp"
#include "lux/engine/flowforge/FunctionNodes.hpp"
#include "lux/engine/flowforge/ObjectNodes.hpp"
#include "lux/engine/flowforge/compiler/IRImpl.hpp"
#include "lux/engine/flowforge/compiler/ScriptInstance.hpp" // invokerSymbol / eventSymbol
#include "lux/engine/flowforge/compiler/TypeSizeMap.hpp"
#include "lux/engine/flowforge/graph/FlowGraph.hpp"
#include "lux/engine/flowforge/graph/StateLayout.hpp"
#include "lux/engine/flowforge/script/ScriptAbilityPayload.hpp"
#include "lux/engine/flowforge/script/ScriptEventPayload.hpp"
#include <lux/engine/flowforge/FlowControlFlow.hpp>
#include <lux/engine/flowforge/FlowExecutionCompiler.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/detail/ScalarLowering.hpp>

namespace lux::flowforge
{
    // ============================================================================
    // Per-build context — owns the module, the builder, and ALL per-build
    // state (constant pools, symbol-uniquification id). Lives here rather
    // than on MLIRBuilderImpl so each generateIR call gets fresh state and
    // pools aren't reused across builds (which would leak stale Op handles
    // from a previous module).
    // ============================================================================
    class BuilderContext
    {
    public:
        BuilderContext(mlir::MLIRContext* context)
            : ctx(context), builder(context), loc(builder.getUnknownLoc()),
              module_id(next_module_id_.fetch_add(1, std::memory_order_relaxed))
        {
            module = builder.create<mlir::ModuleOp>(loc);
            module_owner = mlir::OwningOpRef<mlir::ModuleOp>(module);
            token = mlir::flowforge::FLOWFORGE_TOKEN_TYPE::get(ctx);
        }

        mlir::MLIRContext* ctx;
        mlir::OpBuilder builder;
        mlir::Location loc;
        mlir::Type token; // flowforge::FLOWFORGE_TOKEN_TYPE

        // The module is created detached, so somebody must erase it — the
        // OwningOpRef reclaims a half-built module on every failure path.
        mlir::OwningOpRef<mlir::ModuleOp> module_owner;
        mlir::ModuleOp module;        // plain view of *module_owner
        mlir::func::FuncOp main_func; // the @main wrapper

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
        llvm::StringMap<mlir::LLVM::GlobalOp> string_globals;

        // Extern function declarations cache. Dedupes by symbol name so
        // multiple registered native-call nodes targeting the same function share
        // one func.func declaration in the module.
        llvm::StringMap<mlir::func::FuncOp> extern_funcs;

        // The graph being compiled (variable table lookups) and its
        // instance-state layout: variable accesses lower to
        // `state_ptr + offset` into a HOST-owned block (StateLayout.hpp),
        // so the compiled binary carries no variable storage of its own.
        const FlowGraph* graph = nullptr;
        StateLayout state_layout;

        // Leading block argument of the CURRENT function being lowered
        // (set by lowerFunction): the instance-state base pointer. Valid
        // inside nested regions too — they are not isolated from above.
        mlir::Value state_ptr;
        mlir::Value ability_runtime;
        std::unordered_map<std::uint64_t, std::uint32_t> ability_ordinals;
        std::unordered_map<std::uint64_t, std::uint32_t> event_wait_ordinals;

        NodeId current_node;
        PinId current_pin;

    private:
        static std::atomic<uint32_t> next_module_id_;
    };

    std::atomic<uint32_t> BuilderContext::next_module_id_{1};

    static FlowForgeFailure buildFailure(const BuilderContext& bc, std::string message, bool include_pin = false)
    {
        return FlowForgeFailure{
            .code = EFlowForgeError::GRAPH_INVALID,
            .message = std::move(message),
            .node_id = bc.current_node.value,
            .pin_id = include_pin ? bc.current_pin.value : 0U,
        };
    }

#define LUX_FF_FAIL(build_context, message) return lux::cxx::unexpected(buildFailure((build_context), (message)))
#define LUX_FF_FAIL_AT_PIN(build_context, message)                                                                     \
    return lux::cxx::unexpected(buildFailure((build_context), (message), true))
#define LUX_FF_TRY_VALUE(name, expression)                                                                             \
    auto name##_result = (expression);                                                                                 \
    if (!name##_result)                                                                                                \
        return lux::cxx::unexpected(std::move(name##_result.error()));                                                 \
    auto name = std::move(name##_result.value())
#define LUX_FF_TRY(expression)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        auto lux_ff_result = (expression);                                                                             \
        if (!lux_ff_result)                                                                                            \
            return lux::cxx::unexpected(std::move(lux_ff_result.error()));                                             \
    } while (false)

    // Format a unique LLVM global symbol name for this BuilderContext.
    //   _lf_<module_id>_<kind>_<a>[_<b>]   (all hex)
    //
    // kind: short tag ("g" = class storage, "s" = string).
    // a, b: opaque hashes (e.g. class_hash, value_hash). b=0 omits the suffix.
    static std::string makeGlobalSymbol(BuilderContext& bc, const char* kind, uint64_t a, uint64_t b = 0)
    {
        std::ostringstream oss;
        oss << "_lf_" << std::hex << bc.module_id << '_' << kind << '_' << a;
        if (b)
        {
            oss << '_' << b;
        }
        return oss.str();
    }

    static bool isPointerQual(const lux::meta::RefType& rt)
    {
        using lux::meta::ETypeQual;
        switch (static_cast<ETypeQual>(rt.qtype.qual))
        {
        case ETypeQual::PTR:
        case ETypeQual::PTR_TO_CONST:
        case ETypeQual::CONST_PTR:
        case ETypeQual::CONST_PTR_TO_CONST:
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
    // detail::isUnsignedScalar and registered scalar lowering.
    static mlir::Type refTypeToMLIR(BuilderContext& bc, const lux::meta::RefType& rt)
    {
        using lux::meta::EBaseType;
        auto& b = bc.builder;
        if (isPointerQual(rt))
        {
            return mlir::LLVM::LLVMPointerType::get(bc.ctx);
        }
        switch (static_cast<EBaseType>(rt.qtype.base))
        {
        case EBaseType::BOOL:
            return b.getI1Type();
        case EBaseType::INT8:
        case EBaseType::UINT8:
            return b.getI8Type();
        case EBaseType::INT16:
        case EBaseType::UINT16:
            return b.getIntegerType(16);
        case EBaseType::INT32:
        case EBaseType::UINT32:
            return b.getI32Type();
        case EBaseType::INT64:
        case EBaseType::UINT64:
            return b.getI64Type();
        case EBaseType::FLOAT:
            return b.getF32Type();
        case EBaseType::DOUBLE:
            return b.getF64Type();
        case EBaseType::VOID:
        case EBaseType::RECORD:
        case EBaseType::UNKNOWN:
        default:
            return mlir::LLVM::LLVMPointerType::get(bc.ctx);
        }
    }

    //==============================================================================
    // MLIRBuilderImpl — drives FlowGraph -> FlowForge dialect IR generation.
    //==============================================================================
    static mlir::Value emitScalarPrimitive(
        BuilderContext& bc,
        EScalarInstruction instruction,
        mlir::Value lhs,
        mlir::Value rhs
    ) noexcept
    {
        auto& b = bc.builder;
        auto loc = bc.loc;
        using I = EScalarInstruction;
        using CmpI = mlir::arith::CmpIPredicate;
        using CmpF = mlir::arith::CmpFPredicate;
        switch (instruction)
        {
        case I::ADD_INTEGER:
            return b.create<mlir::arith::AddIOp>(loc, lhs, rhs).getResult();
        case I::ADD_FLOAT:
            return b.create<mlir::arith::AddFOp>(loc, lhs, rhs).getResult();
        case I::SUBTRACT_INTEGER:
            return b.create<mlir::arith::SubIOp>(loc, lhs, rhs).getResult();
        case I::SUBTRACT_FLOAT:
            return b.create<mlir::arith::SubFOp>(loc, lhs, rhs).getResult();
        case I::MULTIPLY_INTEGER:
            return b.create<mlir::arith::MulIOp>(loc, lhs, rhs).getResult();
        case I::MULTIPLY_FLOAT:
            return b.create<mlir::arith::MulFOp>(loc, lhs, rhs).getResult();
        case I::DIVIDE_SIGNED:
            return b.create<mlir::arith::DivSIOp>(loc, lhs, rhs).getResult();
        case I::DIVIDE_UNSIGNED:
            return b.create<mlir::arith::DivUIOp>(loc, lhs, rhs).getResult();
        case I::DIVIDE_FLOAT:
            return b.create<mlir::arith::DivFOp>(loc, lhs, rhs).getResult();
        case I::REMAINDER_SIGNED:
            return b.create<mlir::arith::RemSIOp>(loc, lhs, rhs).getResult();
        case I::REMAINDER_UNSIGNED:
            return b.create<mlir::arith::RemUIOp>(loc, lhs, rhs).getResult();
        case I::REMAINDER_FLOAT:
            return b.create<mlir::arith::RemFOp>(loc, lhs, rhs).getResult();
        case I::AND:
            return b.create<mlir::arith::AndIOp>(loc, lhs, rhs).getResult();
        case I::OR:
            return b.create<mlir::arith::OrIOp>(loc, lhs, rhs).getResult();
        case I::EQUAL_INTEGER:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::eq, lhs, rhs).getResult();
        case I::NOT_EQUAL_INTEGER:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::ne, lhs, rhs).getResult();
        case I::LESS_SIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::slt, lhs, rhs).getResult();
        case I::LESS_UNSIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::ult, lhs, rhs).getResult();
        case I::LESS_EQUAL_SIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::sle, lhs, rhs).getResult();
        case I::LESS_EQUAL_UNSIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::ule, lhs, rhs).getResult();
        case I::GREATER_SIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::sgt, lhs, rhs).getResult();
        case I::GREATER_UNSIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::ugt, lhs, rhs).getResult();
        case I::GREATER_EQUAL_SIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::sge, lhs, rhs).getResult();
        case I::GREATER_EQUAL_UNSIGNED:
            return b.create<mlir::arith::CmpIOp>(loc, CmpI::uge, lhs, rhs).getResult();
        case I::EQUAL_ORDERED_FLOAT:
            return b.create<mlir::arith::CmpFOp>(loc, CmpF::OEQ, lhs, rhs).getResult();
        case I::NOT_EQUAL_ORDERED_FLOAT:
            return b.create<mlir::arith::CmpFOp>(loc, CmpF::ONE, lhs, rhs).getResult();
        case I::LESS_ORDERED_FLOAT:
            return b.create<mlir::arith::CmpFOp>(loc, CmpF::OLT, lhs, rhs).getResult();
        case I::LESS_EQUAL_ORDERED_FLOAT:
            return b.create<mlir::arith::CmpFOp>(loc, CmpF::OLE, lhs, rhs).getResult();
        case I::GREATER_ORDERED_FLOAT:
            return b.create<mlir::arith::CmpFOp>(loc, CmpF::OGT, lhs, rhs).getResult();
        case I::GREATER_EQUAL_ORDERED_FLOAT:
            return b.create<mlir::arith::CmpFOp>(loc, CmpF::OGE, lhs, rhs).getResult();
        case I::NEGATE_FLOAT:
            return b.create<mlir::arith::NegFOp>(loc, lhs).getResult();
        case I::NEGATE_INTEGER:
        {
            auto zero = b.create<mlir::arith::ConstantOp>(loc, lhs.getType(), b.getIntegerAttr(lhs.getType(), 0));
            return b.create<mlir::arith::SubIOp>(loc, zero, lhs).getResult();
        }
        case I::NOT_BOOLEAN:
        {
            auto one = b.create<mlir::arith::ConstantOp>(loc, b.getI1Type(), b.getBoolAttr(true));
            return b.create<mlir::arith::XOrIOp>(loc, lhs, one).getResult();
        }
        default:
            std::terminate(); // The domain checks instruction and arity before calling the backend.
        }
    }

    static FlowForgeResult<mlir::Value> variableAddress(uint64_t var_id, BuilderContext& bc)
    {
        const auto* field = bc.state_layout.find(var_id);
        if (!field)
        {
            LUX_FF_FAIL(bc, "graph variable not found");
        }
        if (!bc.state_ptr)
        {
            LUX_FF_FAIL(bc, "function has no instance-state pointer");
        }

        auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        return bc.builder.create<mlir::LLVM::GEPOp>(
            bc.loc,
            ptr_ty,
            bc.builder.getI8Type(),
            bc.state_ptr,
            llvm::ArrayRef<mlir::LLVM::GEPArg>{static_cast<int32_t>(field->offset)}
        );
    }

    class ValueCompiler final : public FlowValueCompiler
    {
    public:
        explicit ValueCompiler(BuilderContext& context) noexcept : context_(context) {}

        [[nodiscard]] FlowForgeResult<FlowValue> add(mlir::Value value, const meta::RefType& type) noexcept
        {
            if (values_.size() >= kInvalidFlowValue)
            {
                return cxx::unexpected(buildFailure(context_, "scalar value capacity exhausted"));
            }
            const auto id = static_cast<FlowValue>(values_.size());
            values_.push_back({value, &type});
            return id;
        }

        const meta::RefType* type(FlowValue id) const noexcept override
        {
            return id < values_.size() ? values_[id].type : nullptr;
        }

        [[nodiscard]] mlir::Value value(FlowValue id) const noexcept
        {
            return id < values_.size() ? values_[id].value : mlir::Value{};
        }

        FlowForgeResult<FlowValue> readVariable(std::uint64_t variable) noexcept override
        {
            auto& bc = context_;
            const auto* value = bc.graph->findVariable(variable);
            const bool has_type = value && value->type;
            if (!has_type)
            {
                LUX_FF_FAIL(bc, "graph variable not found");
            }
            LUX_FF_TRY_VALUE(slot, variableAddress(variable, bc));
            auto loaded = bc.builder.create<mlir::LLVM::LoadOp>(bc.loc, refTypeToMLIR(bc, *value->type), slot);
            return add(loaded, *value->type);
        }

        FlowForgeResult<FlowValue> readField(const meta::RefField& field, FlowValue object) noexcept override
        {
            auto& bc = context_;
            const auto address = value(object);
            const bool has_pointer = address && mlir::isa<mlir::LLVM::LLVMPointerType>(address.getType());
            if (!has_pointer)
            {
                LUX_FF_FAIL_AT_PIN(bc, "field access needs an object pointer");
            }
            const auto field_type = refTypeToMLIR(bc, field.type);
            const bool is_unsupported_record =
                mlir::isa<mlir::LLVM::LLVMPointerType>(field_type) && !isPointerQual(field.type);
            if (is_unsupported_record)
            {
                LUX_FF_FAIL(bc, "record-typed fields are not supported yet");
            }
            const auto pointer_type = mlir::LLVM::LLVMPointerType::get(bc.ctx);
            auto offset = bc.builder.create<mlir::LLVM::GEPOp>(
                bc.loc,
                pointer_type,
                bc.builder.getI8Type(),
                address,
                llvm::ArrayRef<mlir::LLVM::GEPArg>{static_cast<int32_t>(field.offset)}
            );
            auto loaded = bc.builder.create<mlir::LLVM::LoadOp>(bc.loc, field_type, offset);
            return add(loaded, field.type);
        }

    private:
        FlowForgeResult<FlowValue> emitScalarImpl(
            EScalarInstruction instruction,
            std::span<const FlowValue> operands,
            const meta::RefType& result_type
        ) noexcept override
        {
            const auto lhs = value(operands.front());
            const auto rhs = operands.size() == 2 ? value(operands.back()) : mlir::Value{};
            return add(emitScalarPrimitive(context_, instruction, lhs, rhs), result_type);
        }

        struct Value final
        {
            mlir::Value value;
            const meta::RefType* type;
        };

        BuilderContext& context_;
        std::vector<Value> values_;
    };

    static FlowForgeResult<std::vector<PinId>> nodePins(
        const FlowGraph& graph,
        NodeId node,
        graph::EPinDirection direction,
        EFlowPinRole role
    )
    {
        const auto* value = graph.node(node);
        const bool has_definition = value && value->definition;
        if (!has_definition)
        {
            return lux::cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "missing node definition"});
        }
        auto schema = value->definition->describePins(value->payload);
        if (!schema)
        {
            return lux::cxx::unexpected(std::move(schema.error()));
        }
        std::vector<PinId> result;
        for (const auto& declaration : *schema)
        {
            const bool matches = declaration.direction == direction && declaration.role == role;
            if (matches)
            {
                const auto id = graph.pinId(node, declaration.semantic);
                if (!id.valid())
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        EFlowForgeError::GRAPH_INVALID,
                        "registered pin declaration differs from graph pins",
                        node.value
                    });
                }
                result.push_back(id);
            }
        }
        return result;
    }

    static std::vector<PinId> linkedPins(const FlowGraph& graph, PinId pin)
    {
        std::vector<PinId> result;
        for (const auto& link : graph.topology().links())
        {
            if (link.from == pin)
            {
                result.push_back(link.to);
            }
            else if (link.to == pin)
            {
                result.push_back(link.from);
            }
        }
        return result;
    }

    class MLIRBuilderImpl
    {
        // Pin id -> SSA value mappings (per-build).
        struct ValueMaps
        {
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

            ValueMaps()
            {
                pure_scopes.emplace_back();
            }

            // RAII: one pure-value scope per lowered region.
            struct PureScope
            {
                ValueMaps& vm;

                explicit PureScope(ValueMaps& v) : vm(v)
                {
                    vm.pure_scopes.emplace_back();
                }

                ~PureScope()
                {
                    vm.pure_scopes.pop_back();
                }
            };

            // Explicit lookup helper. Use this instead of `vm.exec_tok[id]`:
            // operator[] silently default-constructs a null mlir::Value on
            // miss, which downstream code happily uses as an operand and
            // crashes during op verification with a confusing "operand 0
            // was null" message. requireExecTok reports the missing-link
            // site together with the offending node.
            FlowForgeResult<mlir::Value> requireExecTok(uint64_t pin_id, BuilderContext& bc) const
            {
                auto it = exec_tok.find(pin_id);
                const bool has_token = it != exec_tok.end() && it->second;
                if (!has_token)
                {
                    LUX_FF_FAIL(bc, "exec token not materialised");
                }
                return it->second;
            }

            FlowForgeResult<llvm::SmallVector<mlir::Value>> gatherPredTokens(PinId in, BuilderContext& bc) const
            {
                llvm::SmallVector<mlir::Value> preds;
                for (const auto ex : linkedPins(*bc.graph, in))
                {
                    auto it = exec_tok.find(ex.value);
                    const bool has_token = it != exec_tok.end() && it->second;
                    if (!has_token)
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
        static mlir::Block* addSingleBlockWithArgs(
            mlir::OpBuilder& b,
            mlir::Region& region,
            mlir::TypeRange argTys,
            mlir::Location loc
        )
        {
            mlir::OpBuilder::InsertionGuard guard(b);
            llvm::SmallVector<mlir::Location, 4> locs(argTys.size(), loc);
            return b.createBlock(&region, region.end(), argTys, locs);
        }

    public:
        explicit MLIRBuilderImpl(mlir::MLIRContext* ctx);
        FlowForgeResult<std::unique_ptr<IR>> generateMLIR(const FlowGraph&);

    private:
        FlowForgeResult<mlir::Value> getOperand(PinId, ValueMaps&, BuilderContext&, bool asIndex = false);
        FlowForgeResult<mlir::Value> buildConstant(
            const meta::RefType&,
            BuilderContext&,
            const lux::meta::RuntimeObject&,
            bool asIndex = false
        );

        // Pin-independent scalar-constant emission (bool / ints / floats).
        // Shared by buildConstant and variable default-value initialization.
        // Returns a structured failure for non-scalar base types.
        FlowForgeResult<mlir::Value> buildScalarConstantValue(
            BuilderContext&,
            const lux::meta::RefType&,
            const lux::meta::RuntimeObject&,
            bool asIndex = false
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
        FlowForgeResult<mlir::Value> materializePureValue(PinId src, ValueMaps&, BuilderContext&);

        // Implicit scalar conversion of `v` to the declared type `dst_rt`
        // (int widening/narrowing, int<->float, float widening). Extension
        // signedness follows the DESTINATION type. Anything non-scalar or
        // float->int returns a structured failure.
        FlowForgeResult<mlir::Value> coerceScalar(BuilderContext&, mlir::Value v, const lux::meta::RefType& dst_rt);

        // Merge convergent control tokens into a single SSA value.
        // - 0 inputs: build error (the caller's exec-in pin has no link).
        // - 1 input: return it directly.
        // - >1 inputs all identical: return the unique value.
        // - >1 inputs with distinct SSA: emit `flowforge.token_merge`. The
        //   FlowForge -> LLVM lowering pass realises this as a basic-block
        //   PHI in the join block.
        FlowForgeResult<mlir::Value> mergeExecTokens(BuilderContext&, const llvm::SmallVector<mlir::Value>&);

        template <size_t Bits>
        mlir::Value globalConstantAssign(BuilderContext&, const lux::meta::RuntimeObject&, bool asIndex);
        mlir::Value globalStringConstantAssign(BuilderContext&, const char*, size_t);

        class ExecutionCompiler final : public FlowExecutionCompiler
        {
        public:
            ExecutionCompiler(
                MLIRBuilderImpl& builder,
                BuilderContext& context,
                ValueMaps& values,
                NodeId node,
                mlir::Value token,
                std::unordered_set<NodeId>& lowered_nodes,
                const std::unordered_set<NodeId>& external_nodes,
                int depth
            ) noexcept
                : builder_(builder), bc_(context), vm_(values), node_(node), in_tok_(token), lowered_(lowered_nodes),
                  external_(external_nodes), loop_depth_(depth), token_(token)
            {
            }

            FlowForgeResult<void> branch(graph::PinSemanticId, graph::PinSemanticId, graph::PinSemanticId) noexcept
                override;
            FlowForgeResult<void> forLoop(
                graph::PinSemanticId,
                graph::PinSemanticId,
                graph::PinSemanticId,
                graph::PinSemanticId,
                graph::PinSemanticId
            ) noexcept override;
            FlowForgeResult<void> whileLoop(graph::PinSemanticId, graph::PinSemanticId, graph::PinSemanticId) noexcept
                override;
            FlowForgeResult<void> sequence(std::span<const graph::PinSemanticId>) noexcept override;
            FlowForgeResult<void> returnValues(std::span<const graph::PinSemanticId>) noexcept override;
            FlowForgeResult<void> breakLoop() noexcept override;
            FlowForgeResult<void> functionCall(
                NodeId,
                std::span<const graph::PinSemanticId>,
                std::span<const graph::PinSemanticId>,
                graph::PinSemanticId
            ) noexcept override;
            FlowForgeResult<void> nativeCall(
                const NativeCallDefinition&,
                std::span<const graph::PinSemanticId>,
                graph::PinSemanticId,
                graph::PinSemanticId
            ) noexcept override;
            FlowForgeResult<void> abilityCall(
                const ScriptAbilityPayload&,
                std::span<const graph::PinSemanticId>,
                std::span<const graph::PinSemanticId>,
                graph::PinSemanticId
            ) noexcept override;
            FlowForgeResult<void> eventWait(
                const ScriptEventPayload&,
                graph::PinSemanticId,
                graph::PinSemanticId
            ) noexcept override;
            FlowForgeResult<void> storeVariable(
                std::uint64_t,
                graph::PinSemanticId,
                graph::PinSemanticId,
                graph::PinSemanticId
            ) noexcept override;
            FlowForgeResult<void> storeField(
                const meta::RefField&,
                graph::PinSemanticId,
                graph::PinSemanticId,
                graph::PinSemanticId,
                graph::PinSemanticId
            ) noexcept override;

            [[nodiscard]] PinId next() const noexcept
            {
                return next_;
            }

            [[nodiscard]] mlir::Value token() const noexcept
            {
                return token_;
            }

        private:
            [[nodiscard]] FlowForgeResult<void> validatePins(
                std::span<const graph::PinSemanticId> semantics,
                graph::EPinDirection direction,
                EFlowPinRole role
            ) const noexcept
            {
                for (const auto semantic : semantics)
                {
                    const auto id = pin(semantic);
                    const auto* record = bc_.graph->topology().findPin(id);
                    const auto* value = bc_.graph->pin(id);
                    const bool has_pin = record && value;
                    const bool matches = has_pin && record->owner == node_ && record->direction == direction &&
                                         value->role == role && (role == EFlowPinRole::EXECUTION || value->type);
                    if (!matches)
                    {
                        return lux::cxx::unexpected(FlowForgeFailure{
                            EFlowForgeError::GRAPH_INVALID,
                            "execution compiler pin does not match the registered schema",
                            node_.value,
                            id.value
                        });
                    }
                }
                return {};
            }

            [[nodiscard]] PinId pin(graph::PinSemanticId semantic) const noexcept
            {
                return bc_.graph->pinId(node_, semantic);
            }

            void completed(graph::PinSemanticId semantic) noexcept
            {
                next_ = pin(semantic);
                token_ = in_tok_;
                vm_.exec_tok[next_.value] = token_;
            }

            MLIRBuilderImpl& builder_;
            BuilderContext& bc_;
            ValueMaps& vm_;
            NodeId node_;
            mlir::Value in_tok_;
            std::unordered_set<NodeId>& lowered_;
            const std::unordered_set<NodeId>& external_;
            int loop_depth_;
            PinId next_;
            mlir::Value token_;
        };

        // Per-region recursive lowering driver.
        //
        // Branch reachability and merge selection belong to the Flow module.
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
        FlowForgeResult<mlir::Value> lowerChain(
            BuilderContext& bc,
            ValueMaps& vm,
            PinId start_pin,
            std::unordered_set<NodeId>& lowered,
            const std::unordered_set<NodeId>& external,
            int loop_depth
        );

        // Lower one graph function: a START entry becomes @main, a
        // FUNC_DEF_START entry becomes a func.func named after the
        // FuncDefNode (signature from its arg/ret declarations). All
        // functions share one module, so graph calls are plain func.calls
        // and recursion is legal.
        FlowForgeResult<void> lowerFunction(BuilderContext& bc, NodeId entry_id);

        mlir::MLIRContext* context_;

        // Pure nodes currently on the materialization recursion stack —
        // re-entering one means the data graph has a cycle.
        std::unordered_set<NodeId> materializing_;
    };

    // ----------------------------------------------------------------------------
    // ctor — nothing to wire up; registered execution callbacks select the backend primitive.
    // ----------------------------------------------------------------------------
    MLIRBuilderImpl::MLIRBuilderImpl(mlir::MLIRContext* context) : context_(context) {}

    // =============================================================================
    // Public API
    //
    // Generation walks the exec graph recursively. Outer chain lives inside
    // a `func.func @main` wrapper at module body; control ops (Branch /
    // ForLoop / WhileLoop / Sequence) lower their sub-regions by re-entering
    // lowerChain with insertion point inside the region's block.
    // =============================================================================
    FlowForgeResult<std::unique_ptr<IR>> MLIRBuilderImpl::generateMLIR(const FlowGraph& g)
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
        llvm::SmallVector<NodeId, 4> entries;
        NodeId start;
        std::unordered_set<std::string> func_names;

        struct AbilityKey final
        {
            std::string_view contract;
            std::string_view method;
            std::uint64_t node{};
        };

        std::vector<AbilityKey> ability_keys;

        struct EventWaitKey final
        {
            std::uint64_t system{};
            std::uint64_t event{};
            std::uint8_t route{};
            std::uint64_t node{};
        };

        std::vector<EventWaitKey> event_wait_keys;
        for (const auto& [id, node] : g.nodes())
        {
            if (const auto* ability = node->payload.get<ScriptAbilityPayload>())
            {
                ability_keys.push_back({ability->contract().name(), ability->method().name(), id.value});
            }
            else if (const auto* wait = node->payload.get<ScriptEventPayload>())
            {
                const auto& event = wait->source();
                event_wait_keys.push_back(
                    {event.system_id, event.event_id, static_cast<std::uint8_t>(event.route), id.value}
                );
            }
            if (node->payload.get<StartPayload>())
            {
                if (start.valid())
                {
                    LUX_FF_FAIL(bc, "graph has more than one Start node");
                }
                start = id;
                entries.push_back(id);
            }
            else if (node->payload.get<FunctionPayload>())
            {
                bc.current_node = id;
                if (node->name.empty())
                {
                    LUX_FF_FAIL(bc, "graph function has no name");
                }
                if (node->name == "main")
                {
                    LUX_FF_FAIL(bc, "'main' is reserved for the Start entry");
                }
                if (!func_names.insert(node->name).second)
                {
                    LUX_FF_FAIL(bc, "duplicate graph function name");
                }
                entries.push_back(id);
            }
            else if (node->payload.get<EventEntryPayload>())
            {
                bc.current_node = id;
                if (node->name.empty())
                {
                    LUX_FF_FAIL(bc, "event entry has no name");
                }
                if (!func_names.insert(FlowScriptInstance::eventSymbol(node->name)).second)
                {
                    LUX_FF_FAIL(bc, "duplicate event name");
                }
                entries.push_back(id);
            }
        }
        std::ranges::sort(
            ability_keys,
            [](const auto& left, const auto& right)
            {
                return left.contract < right.contract ||
                       (left.contract == right.contract && left.method < right.method);
            }
        );
        std::uint32_t next_ordinal{};
        std::string_view previous_contract;
        std::string_view previous_method;
        bool has_previous{};
        for (const auto& key : ability_keys)
        {
            const bool is_new_ability =
                !has_previous || key.contract != previous_contract || key.method != previous_method;
            if (is_new_ability)
            {
                previous_contract = key.contract;
                previous_method = key.method;
                has_previous = true;
                ++next_ordinal;
            }
            bc.ability_ordinals.emplace(key.node, next_ordinal - 1U);
        }
        std::ranges::sort(
            event_wait_keys,
            [](const auto& left, const auto& right)
            {
                if (left.system != right.system)
                {
                    return left.system < right.system;
                }
                if (left.event != right.event)
                {
                    return left.event < right.event;
                }
                return left.route < right.route;
            }
        );
        next_ordinal = 0U;
        std::uint64_t previous_system{};
        std::uint64_t previous_event{};
        std::uint8_t previous_route{};
        has_previous = false;
        for (const auto& key : event_wait_keys)
        {
            const bool is_new_event = !has_previous || key.system != previous_system || key.event != previous_event ||
                                      key.route != previous_route;
            if (is_new_event)
            {
                previous_system = key.system;
                previous_event = key.event;
                previous_route = key.route;
                has_previous = true;
                ++next_ordinal;
            }
            bc.event_wait_ordinals.emplace(key.node, next_ordinal - 1U);
        }
        if (entries.empty())
        {
            LUX_FF_FAIL(bc, "no entry node found");
        }

        for (const auto entry : entries)
        {
            LUX_FF_TRY(lowerFunction(bc, entry));
        }

        // Verify before handing the module out and retain MLIR diagnostics.
        {
            std::string diag_text;
            mlir::ScopedDiagnosticHandler handler(
                context_,
                [&](mlir::Diagnostic& d)
                {
                    llvm::raw_string_ostream os(diag_text);
                    os << d.str() << '\n';
                    return mlir::success();
                }
            );
            if (mlir::failed(mlir::verify(bc.module)))
            {
                return lux::cxx::unexpected(FlowForgeFailure{
                    .code = EFlowForgeError::IR_VERIFICATION_FAILED,
                    .message = "generated MLIR module failed verification:\n" + diag_text,
                });
            }
        }

        auto ir = std::make_unique<IR>();
        ir->impl().top_module = std::move(bc.module_owner);
        ir->impl().state_size = bc.state_layout.size;
        ir->impl().state_hash = bc.state_layout.hash;
        ir->impl().state_align = bc.state_layout.align;
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
    FlowForgeResult<void> MLIRBuilderImpl::lowerFunction(BuilderContext& bc, NodeId entry_id)
    {
        bc.current_node = entry_id;
        const auto& entry = *bc.graph->node(entry_id);
        bc.builder.setInsertionPointToEnd(bc.module.getBody());

        std::string fn_name = "main";
        // EVERY generated function takes the instance-state base pointer as
        // its LEADING argument — graph variables live in a host-owned block
        // (StateLayout.hpp), and graph-internal calls forward the pointer so
        // all of an instance's functions share its variables. Declared /
        // payload arguments follow it.
        llvm::SmallVector<mlir::Type, 4> arg_tys;
        arg_tys.push_back(mlir::LLVM::LLVMPointerType::get(bc.ctx));
        arg_tys.push_back(mlir::LLVM::LLVMPointerType::get(bc.ctx));
        llvm::SmallVector<mlir::Type, 2> ret_tys;
        const auto* def = entry.payload.get<FunctionPayload>();
        const auto* event = entry.payload.get<EventEntryPayload>();
        LUX_FF_TRY_VALUE(
            entry_pins,
            nodePins(*bc.graph, entry_id, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION)
        );
        if (entry_pins.empty())
        {
            LUX_FF_FAIL(bc, "entry node has no execution output");
        }
        const auto entry_pin = entry_pins.front();

        if (entry.payload.get<StartPayload>())
        {
            // Start has only the hidden runtime arguments.
        }
        else if (def)
        {
            fn_name = entry.name;
            for (const auto& a : def->arguments)
            {
                if (!a.type)
                {
                    LUX_FF_FAIL(bc, "function argument has no type");
                }
                arg_tys.push_back(refTypeToMLIR(bc, *a.type));
            }
            for (const auto& r : def->results)
            {
                if (!r.type)
                {
                    LUX_FF_FAIL(bc, "function return value has no type");
                }
                ret_tys.push_back(refTypeToMLIR(bc, *r.type));
            }
        }
        else if (event)
        {
            fn_name = FlowScriptInstance::eventSymbol(entry.name);
            for (const auto& p : event->parameters)
            {
                if (!p.type)
                {
                    LUX_FF_FAIL(bc, "event parameter has no type");
                }
                arg_tys.push_back(refTypeToMLIR(bc, *p.type));
            }
        }
        else
        {
            LUX_FF_FAIL(bc, "unsupported entry node type");
        }

        auto fn = bc.builder.create<mlir::func::FuncOp>(bc.loc, fn_name, bc.builder.getFunctionType(arg_tys, ret_tys));
        // Event entries are invoked by the HOST through the packed/ciface
        // convention (FlowScriptInstance::invoke -> invokePacked), which
        // needs the _mlir_ciface wrapper.
        if (event)
        {
            fn->setAttr("llvm.emit_c_interface", mlir::UnitAttr::get(bc.ctx));
        }
        auto* entry_block = fn.addEntryBlock();
        bc.main_func = fn; // current function: alloca/return-type context
        bc.builder.setInsertionPointToStart(entry_block);

        ValueMaps vm;
        std::unordered_set<NodeId> lowered{entry_id};

        // Entry token + argument surfacing.
        auto entry_tok = bc.builder.create<mlir::flowforge::StartOp>(bc.loc, bc.token).getResult();
        vm.exec_tok[entry_pin.value] = entry_tok;
        bc.state_ptr = entry_block->getArgument(0);
        bc.ability_runtime = entry_block->getArgument(1);
        LUX_FF_TRY_VALUE(arguments, nodePins(*bc.graph, entry_id, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        for (std::size_t i = 0; i < arguments.size(); ++i)
        {
            vm.exec_data[arguments[i].value] = entry_block->getArgument(i + 2);
        }

        LUX_FF_TRY_VALUE(tail, lowerChain(bc, vm, entry_pin, lowered, /*external=*/{}, /*loop_depth=*/0));

        // A null tail means the chain ended in an explicit Return/Break
        // terminator. Otherwise the chain just stopped — synthesize the
        // implicit "fall off the end" return, which is only legal for a
        // void function.
        if (tail)
        {
            if (!ret_tys.empty())
            {
                LUX_FF_FAIL(
                    bc,
                    "graph function with return values must end in a "
                    "Function Return node on every path"
                );
            }
            bc.builder.create<mlir::flowforge::ReturnOp>(bc.loc, mlir::TypeRange{}, mlir::ValueRange{tail});
        }
        return {};
    }

    // ============================================================================
    // The recursive chain driver.
    //
    // Preconditions on entry:
    //   - bc.builder's insertion point is at the END of the target block.
    //   - vm.exec_tok[start_pin.value] holds the incoming token for this chain
    //     (the predecessor's out-token, or the enclosing region's block-arg).
    //
    // The loop follows the supplied topology and calls the registered execution compiler.
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
        BuilderContext& bc,
        ValueMaps& vm,
        PinId start_pin,
        std::unordered_set<NodeId>& lowered,
        const std::unordered_set<NodeId>& external,
        int loop_depth
    )
    {
        auto current = start_pin;
        LUX_FF_TRY_VALUE(token, vm.requireExecTok(current.value, bc));
        while (true)
        {
            const auto successors = linkedPins(*bc.graph, current);
            if (successors.empty())
            {
                return token;
            }
            const auto input = successors.front();
            const auto* record = bc.graph->topology().findPin(input);
            if (!record)
            {
                LUX_FF_FAIL(bc, "execution edge has no target pin");
            }
            const auto id = record->owner;
            const bool is_already_owned = external.contains(id) || lowered.contains(id);
            if (is_already_owned)
            {
                return token;
            }
            lowered.insert(id);
            bc.current_node = id;
            if (bc.graph->topology().linkCount(input) > 1)
            {
                LUX_FF_TRY_VALUE(predecessors, vm.gatherPredTokens(input, bc));
                LUX_FF_TRY_VALUE(merged, mergeExecTokens(bc, predecessors));
                token = merged;
            }
            const auto* node = bc.graph->node(id);
            const bool has_definition = node && node->definition;
            if (!has_definition)
            {
                LUX_FF_FAIL(bc, "execution node has no registered definition");
            }
            ExecutionCompiler compiler(*this, bc, vm, id, token, lowered, external, loop_depth);
            LUX_FF_TRY(node->definition->compileExecution(node->payload, compiler));
            token = compiler.token();
            current = compiler.next();
            if (!current.valid())
            {
                return token;
            }
        }
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::storeVariable(
        std::uint64_t variable,
        graph::PinSemanticId value,
        graph::PinSemanticId result,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(std::array{value}, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{result}, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        const auto* var = bc_.graph ? bc_.graph->findVariable(variable) : nullptr;
        const bool has_variable = var && var->type;
        if (!has_variable)
        {
            LUX_FF_FAIL(bc_, "graph variable not found");
        }

        LUX_FF_TRY_VALUE(operand, builder_.getOperand(pin(value), vm_, bc_));
        LUX_FF_TRY_VALUE(val, builder_.coerceScalar(bc_, operand, *var->type));
        LUX_FF_TRY_VALUE(slot, variableAddress(variable, bc_));
        bc_.builder.create<mlir::LLVM::StoreOp>(bc_.loc, val, slot);

        // Passthrough value + threaded exec token.
        vm_.exec_data[pin(result).value] = val;
        completed(done);
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::storeField(
        const meta::RefField& field,
        graph::PinSemanticId object,
        graph::PinSemanticId value,
        graph::PinSemanticId result,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(std::array{object, value}, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{result}, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        LUX_FF_TRY_VALUE(obj, builder_.getOperand(pin(object), vm_, bc_));
        if (!mlir::isa<mlir::LLVM::LLVMPointerType>(obj.getType()))
        {
            LUX_FF_FAIL(bc_, "field access needs an object pointer");
        }
        mlir::Type fty = refTypeToMLIR(bc_, field.type);
        const bool is_unsupported_record = mlir::isa<mlir::LLVM::LLVMPointerType>(fty) && !isPointerQual(field.type);
        if (is_unsupported_record)
        {
            LUX_FF_FAIL(bc_, "record-typed fields are not supported yet");
        }
        LUX_FF_TRY_VALUE(val, builder_.getOperand(pin(value), vm_, bc_));
        if (!mlir::isa<mlir::LLVM::LLVMPointerType>(fty))
        {
            LUX_FF_TRY_VALUE(coerced, builder_.coerceScalar(bc_, val, field.type));
            val = coerced;
        }
        auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc_.ctx);
        mlir::Value gep = bc_.builder.create<mlir::LLVM::GEPOp>(
            bc_.loc,
            ptr_ty,
            bc_.builder.getI8Type(),
            obj,
            llvm::ArrayRef<mlir::LLVM::GEPArg>{static_cast<int32_t>(field.offset)}
        );
        bc_.builder.create<mlir::LLVM::StoreOp>(bc_.loc, val, gep);

        // Object passthrough + threaded exec token.
        vm_.exec_data[pin(result).value] = obj;
        completed(done);
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::functionCall(
        NodeId callee_id,
        std::span<const graph::PinSemanticId> arguments,
        std::span<const graph::PinSemanticId> results,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(arguments, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(results, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        const auto* callee = bc_.graph->node(callee_id);
        const bool has_callee = callee && callee->payload.get<FunctionPayload>();
        if (!has_callee)
        {
            LUX_FF_FAIL(bc_, "graph call has no callee");
        }

        llvm::SmallVector<mlir::Value, 4> operands;
        // Callee shares THIS instance's variables: forward the
        // state pointer as the hidden leading argument.
        operands.push_back(bc_.state_ptr);
        operands.push_back(bc_.ability_runtime);
        for (const auto semantic : arguments)
        {
            const auto id = pin(semantic);
            const auto* input = bc_.graph->pin(id);
            LUX_FF_TRY_VALUE(v, builder_.getOperand(id, vm_, bc_));
            const bool needs_scalar_coercion =
                input->type && !mlir::isa<mlir::LLVM::LLVMPointerType>(refTypeToMLIR(bc_, *input->type)) &&
                !mlir::isa<mlir::LLVM::LLVMPointerType>(v.getType());
            if (needs_scalar_coercion)
            {
                LUX_FF_TRY_VALUE(coerced, builder_.coerceScalar(bc_, v, *input->type));
                v = coerced;
            }
            operands.push_back(v);
        }
        llvm::SmallVector<mlir::Type, 2> ret_tys;
        for (const auto semantic : results)
        {
            ret_tys.push_back(refTypeToMLIR(bc_, *bc_.graph->pin(pin(semantic))->type));
        }

        auto callOp = bc_.builder.create<mlir::func::CallOp>(bc_.loc, callee->name, ret_tys, operands);
        for (size_t i = 0; i < results.size(); ++i)
        {
            vm_.exec_data[pin(results[i]).value] = callOp.getResult(i);
        }

        completed(done);
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::forLoop(
        graph::PinSemanticId first_pin,
        graph::PinSemanticId last_pin,
        graph::PinSemanticId index,
        graph::PinSemanticId body_pin,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(std::array{first_pin, last_pin}, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{index}, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{body_pin, done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        auto idxTy = bc_.builder.getIndexType();
        // Constants come back as index directly (asIdx); linked
        // integer values need an explicit index_cast.
        auto toIndex = [&](mlir::Value v) -> FlowForgeResult<mlir::Value>
        {
            if (v.getType() == idxTy)
            {
                return v;
            }
            if (mlir::isa<mlir::IntegerType>(v.getType()))
            {
                return bc_.builder.create<mlir::arith::IndexCastOp>(bc_.loc, idxTy, v).getResult();
            }
            LUX_FF_FAIL(bc_, "for-loop bound is not an integer");
        };
        LUX_FF_TRY_VALUE(first_operand, builder_.getOperand(pin(first_pin), vm_, bc_, /*asIdx=*/true));
        LUX_FF_TRY_VALUE(last_operand, builder_.getOperand(pin(last_pin), vm_, bc_, /*asIdx=*/true));
        LUX_FF_TRY_VALUE(first, toIndex(first_operand));
        LUX_FF_TRY_VALUE(last, toIndex(last_operand));

        auto op = bc_.builder.create<mlir::flowforge::ForLoopOp>(
            bc_.loc,
            mlir::TypeRange{bc_.token, bc_.token, idxTy},
            mlir::ValueRange{in_tok_, first, last}
        );

        // body region — args = (per-iter token, iv). The yield
        // carries only the continuation token: the IV is a
        // loop-defined block-arg (scf.for model), not a value
        // that flows along region control-flow edges. A null
        // chain result means the body ended in Return/Break —
        // that terminator stands, no yield.
        {
            auto* blk =
                addSingleBlockWithArgs(bc_.builder, op.getBodyRegion(), mlir::TypeRange{bc_.token, idxTy}, bc_.loc);
            mlir::OpBuilder::InsertionGuard guard(bc_.builder);
            ValueMaps::PureScope pure_scope(vm_);
            bc_.builder.setInsertionPointToEnd(blk);
            auto body_arg = blk->getArgument(0);
            auto iv_arg = blk->getArgument(1);
            vm_.exec_tok[pin(body_pin).value] = body_arg;
            vm_.exec_data[pin(index).value] = iv_arg;
            LUX_FF_TRY_VALUE(
                body_end,
                builder_.lowerChain(bc_, vm_, pin(body_pin), lowered_, external_, loop_depth_ + 1)
            );
            if (body_end)
            {
                bc_.builder.create<mlir::flowforge::YieldOp>(bc_.loc, body_end);
            }
        }

        token_ = op.getResult(1);
        vm_.exec_tok[pin(done).value] = token_;
        next_ = pin(done);
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::whileLoop(
        graph::PinSemanticId condition,
        graph::PinSemanticId body,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(std::array{condition}, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{body, done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        auto op = bc_.builder.create<mlir::flowforge::WhileLoopOp>(
            bc_.loc,
            mlir::TypeRange{bc_.token, bc_.token},
            mlir::ValueRange{in_tok_}
        );

        // cond region — per-iteration condition evaluation.
        {
            auto* blk = addSingleBlockWithArgs(bc_.builder, op.getCondRegion(), mlir::TypeRange{bc_.token}, bc_.loc);
            mlir::OpBuilder::InsertionGuard guard(bc_.builder);
            ValueMaps::PureScope pure_scope(vm_);
            bc_.builder.setInsertionPointToEnd(blk);
            LUX_FF_TRY_VALUE(cond_val, builder_.getOperand(pin(condition), vm_, bc_));
            bc_.builder.create<mlir::flowforge::CondYieldOp>(bc_.loc, blk->getArgument(0), cond_val);
        }
        // body region — recursive
        {
            auto* blk = addSingleBlockWithArgs(bc_.builder, op.getBodyRegion(), mlir::TypeRange{bc_.token}, bc_.loc);
            mlir::OpBuilder::InsertionGuard guard(bc_.builder);
            ValueMaps::PureScope pure_scope(vm_);
            bc_.builder.setInsertionPointToEnd(blk);
            auto body_arg = blk->getArgument(0);
            vm_.exec_tok[pin(body).value] = body_arg;
            LUX_FF_TRY_VALUE(body_end, builder_.lowerChain(bc_, vm_, pin(body), lowered_, external_, loop_depth_ + 1));
            if (body_end)
            {
                bc_.builder.create<mlir::flowforge::YieldOp>(bc_.loc, body_end);
            }
        }

        token_ = op.getResult(1);
        vm_.exec_tok[pin(done).value] = token_;
        next_ = pin(done);
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::sequence(std::span<const graph::PinSemanticId> legs
    ) noexcept
    {
        LUX_FF_TRY(validatePins(legs, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        // Sequence is pure ordering: the runtime fires each leg
        // in order. The lowering therefore inlines the legs
        // sequentially into the CURRENT block, threading the
        // token from one leg's end to the next leg's start — no
        // dedicated op or region is needed.
        mlir::Value tok = in_tok_;
        for (const auto semantic : legs)
        {
            const auto leg = pin(semantic);
            vm_.exec_tok[leg.value] = tok;
            LUX_FF_TRY_VALUE(end, builder_.lowerChain(bc_, vm_, leg, lowered_, external_, loop_depth_));
            // A leg that ended in Return/Break terminates the
            // chain — remaining legs are unreachable (the
            // runtime aborts the sequence there too).
            if (!end)
            {
                token_ = {};
                return {};
            }
            tok = end;
        }

        // Sequence has no continuation pin of its own — each leg
        // already carried its chain to its end. Chain ends here;
        // `tok` is the token after the last leg completed.
        token_ = tok;
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::branch(
        graph::PinSemanticId condition,
        graph::PinSemanticId true_leg,
        graph::PinSemanticId false_leg
    ) noexcept
    {
        LUX_FF_TRY(validatePins(std::array{condition}, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{true_leg, false_leg}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION)
        );

        LUX_FF_TRY_VALUE(cond, builder_.getOperand(pin(condition), vm_, bc_));

        const auto pd = findBranchMerge(*bc_.graph, node_, pin(true_leg), pin(false_leg));
        auto inner_ext = external_;
        if (pd.valid())
        {
            inner_ext.insert(pd);
        }

        auto op = bc_.builder.create<mlir::flowforge::BranchOp>(
            bc_.loc,
            mlir::TypeRange{bc_.token, bc_.token},
            mlir::ValueRange{in_tok_, cond}
        );

        // then-region — block-arg(0) = the input token for this
        // leg. A null chain result means the leg ended in a
        // Return/Break terminator — no yield may follow it.
        bool then_terminated = false;
        {
            auto* blk =
                addSingleBlockWithArgs(bc_.builder, FLOWFORGE_GET_THEN_REGION(op), mlir::TypeRange{bc_.token}, bc_.loc);
            mlir::OpBuilder::InsertionGuard guard(bc_.builder);
            ValueMaps::PureScope pure_scope(vm_);
            bc_.builder.setInsertionPointToEnd(blk);
            auto blk_arg = blk->getArgument(0);
            vm_.exec_tok[pin(true_leg).value] = blk_arg;
            LUX_FF_TRY_VALUE(end, builder_.lowerChain(bc_, vm_, pin(true_leg), lowered_, inner_ext, loop_depth_));
            if (end)
            {
                bc_.builder.create<mlir::flowforge::YieldOp>(bc_.loc, end);
            }
            else
            {
                then_terminated = true;
            }
        }
        // else-region — same shape
        bool else_terminated = false;
        {
            auto* blk =
                addSingleBlockWithArgs(bc_.builder, FLOWFORGE_GET_ELSE_REGION(op), mlir::TypeRange{bc_.token}, bc_.loc);
            mlir::OpBuilder::InsertionGuard guard(bc_.builder);
            ValueMaps::PureScope pure_scope(vm_);
            bc_.builder.setInsertionPointToEnd(blk);
            auto blk_arg = blk->getArgument(0);
            vm_.exec_tok[pin(false_leg).value] = blk_arg;
            LUX_FF_TRY_VALUE(end, builder_.lowerChain(bc_, vm_, pin(false_leg), lowered_, inner_ext, loop_depth_));
            if (end)
            {
                bc_.builder.create<mlir::flowforge::YieldOp>(bc_.loc, end);
            }
            else
            {
                else_terminated = true;
            }
        }

        // Post-lowering: the BranchOp's results are the per-leg
        // out-tokens visible to OUTER scope. Re-publish:
        //  - the Branch's own out-pins (so any direct consumer
        //    sees the result, not the inner block-arg);
        //  - every exec_out pin of nodes reachable inside each leg
        //    (so PD's gatherPredTokens, which still reads the
        //    inside-leg exec_out_pin ids, sees Branch.result(i)
        //    rather than the now-out-of-scope inside-block SSA).
        vm_.exec_tok[pin(true_leg).value] = op.getResult(0);
        vm_.exec_tok[pin(false_leg).value] = op.getResult(1);
        auto up_reach = reachableExecution(*bc_.graph, pin(true_leg));
        auto down_reach = reachableExecution(*bc_.graph, pin(false_leg));
        auto remap = [&](const auto& reachable, mlir::Value token) -> FlowForgeResult<void>
        {
            for (const auto id : reachable)
            {
                if (id == pd)
                {
                    continue;
                }
                LUX_FF_TRY_VALUE(
                    outputs,
                    nodePins(*bc_.graph, id, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION)
                );
                for (const auto output : outputs)
                {
                    if (vm_.exec_tok.contains(output.value))
                    {
                        vm_.exec_tok[output.value] = token;
                    }
                }
            }
            return {};
        };
        LUX_FF_TRY(remap(up_reach, op.getResult(0)));
        LUX_FF_TRY(remap(down_reach, op.getResult(1)));

        if (!pd.valid())
        {
            // Legs never reconverge. If BOTH legs terminated
            // (Return/Break), control cannot flow past the
            // branch — but the containing block still needs a
            // terminator (BranchOp is not one). Emit the
            // unreachable marker; the CF lowering erases it
            // together with its provably-unreachable block.
            const bool all_legs_terminated = then_terminated && else_terminated;
            if (all_legs_terminated)
            {
                bc_.builder.create<mlir::flowforge::UnreachableOp>(bc_.loc, op.getResult(0));
                token_ = {};
                return {};
            }
            // Otherwise at least one leg falls through and the
            // outer chain simply has nothing more to lower.
            return {};
        }
        if (external_.contains(pd))
        {
            // The enclosing region owns this merge. Its other predecessors may not have
            // been lowered_ yet, or their tokens may belong to sibling regions. Complete
            // this region with the branch's outer token; only the owning chain gathers
            // the merge's predecessors after all participating regions have returned.
            token_ = op.getResult(0);
            return {};
        }
        // Pivot to the post-dominator. Any of its linked
        // predecessors works (they all map to the right Branch
        // result thanks to the remap above); the next iteration
        // will see PD as `node`, gather the (already-remapped)
        // pred tokens, and emit token_merge.
        LUX_FF_TRY_VALUE(inputs, nodePins(*bc_.graph, pd, graph::EPinDirection::INPUT, EFlowPinRole::EXECUTION));
        const auto predecessors = inputs.empty() ? std::vector<PinId>{} : linkedPins(*bc_.graph, inputs.front());
        if (predecessors.empty())
        {
            LUX_FF_FAIL(bc_, "post-dominator has no exec_in link");
        }
        next_ = predecessors.front();
        LUX_FF_TRY_VALUE(post_dom_token, vm_.requireExecTok(next_.value, bc_));
        token_ = post_dom_token;
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::breakLoop() noexcept
    {
        if (loop_depth_ == 0)
        {
            LUX_FF_FAIL(bc_, "Break is only valid inside a loop body");
        }
        bc_.builder.create<mlir::flowforge::BreakOp>(bc_.loc, in_tok_);
        token_ = {};
        return {};
    }

    // =============================================================================
    // getOperand — resolve a data input identity to an MLIR Value.
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
        PinId input,
        ValueMaps& vm,
        BuilderContext& bc,
        bool as_index
    )
    {
        bc.current_pin = input;
        const auto* record = bc.graph->topology().findPin(input);
        const auto* value = bc.graph->pin(input);
        const bool is_input = record && record->direction == graph::EPinDirection::INPUT;
        const bool is_data = value && value->role == EFlowPinRole::DATA && value->type;
        const bool is_invalid_input = !is_input || !is_data;
        if (is_invalid_input)
        {
            LUX_FF_FAIL_AT_PIN(bc, "data input pin is not part of the graph");
        }
        if (const auto link = bc.graph->topology().incoming(input))
        {
            if (auto it = vm.exec_data.find(link->from.value); it != vm.exec_data.end())
            {
                return it->second;
            }
            auto& scope = vm.pure_scopes.back();
            if (auto it = scope.find(link->from.value); it != scope.end())
            {
                return it->second;
            }
            return materializePureValue(link->from, vm, bc);
        }
        if (value->allow_default)
        {
            if (!value->default_value.isValid())
            {
                LUX_FF_FAIL_AT_PIN(bc, "pin has no link and no valid default constant");
            }
            LUX_FF_TRY_VALUE(constant, buildConstant(*value->type, bc, value->default_value, as_index));
            vm.pure_scopes.back()[input.value] = constant;
            return constant;
        }
        LUX_FF_FAIL_AT_PIN(bc, "DataInPin has no source and no default value");
    }

    template <size_t Bits>
    mlir::Value MLIRBuilderImpl::globalConstantAssign(
        BuilderContext& bc,
        const lux::meta::RuntimeObject& obj,
        bool is_seq
    )
    {
        using buffer_type = typename TTypeSizeMap<Bits>::type;
        auto& builder = bc.builder;

        if (is_seq)
        {
            return TTypeSizeMap<Bits>::getIndex(builder, obj);
        }

        auto llvm_type = TTypeSizeMap<Bits>::getLLVMType(builder);
        auto attr = TTypeSizeMap<Bits>::getAttr(builder, obj);
        return builder.create<mlir::LLVM::ConstantOp>(bc.loc, llvm_type, attr);
    }

    mlir::Value MLIRBuilderImpl::globalStringConstantAssign(BuilderContext& bc, const char* str_data, size_t str_size)
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
            std::string sym_name = makeGlobalSymbol(bc, "s", h, bc.string_globals.size() + 1);

            auto arr_ty = mlir::LLVM::LLVMArrayType::get(bc.builder.getI8Type(), terminated.size());
            mlir::OpBuilder::InsertionGuard guard(bc.builder);
            bc.builder.setInsertionPointToStart(bc.module.getBody());
            auto g_obj = bc.builder.create<mlir::LLVM::GlobalOp>(
                bc.loc,
                arr_ty,
                /*isConstant=*/true,
                mlir::LLVM::Linkage::Internal,
                sym_name,
                mlir::StringAttr::get(bc.ctx, terminated)
            );
            it = bc.string_globals.try_emplace(bytes, g_obj).first;
        }

        auto ptr_type = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        auto sym = mlir::FlatSymbolRefAttr::get(bc.ctx, it->second.getSymName());
        return bc.builder.create<mlir::LLVM::AddressOfOp>(bc.loc, ptr_type, sym);
    }

    // ============================================================================
    // buildScalarConstantValue — pin-independent scalar constants.
    // `as_index` (index type requested by for-loop bounds) is only
    // meaningful for integers and is honored inside globalConstantAssign<>.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::buildScalarConstantValue(
        BuilderContext& bc,
        const lux::meta::RefType& rt,
        const lux::meta::RuntimeObject& obj,
        bool as_index
    )
    {
        using lux::meta::EBaseType;
        auto& b = bc.builder;
        auto& loc = bc.loc;

        switch (static_cast<EBaseType>(rt.qtype.base))
        {
        case EBaseType::BOOL:
        {
            const bool* p = static_cast<const bool*>(obj.data());
            return b.create<mlir::arith::ConstantOp>(loc, b.getI1Type(), b.getBoolAttr(*p));
        }
        case EBaseType::FLOAT:
        {
            const float* p = static_cast<const float*>(obj.data());
            return b.create<mlir::arith::ConstantOp>(loc, b.getF32Type(), b.getF32FloatAttr(*p));
        }
        case EBaseType::DOUBLE:
        {
            const double* p = static_cast<const double*>(obj.data());
            return b.create<mlir::arith::ConstantOp>(loc, b.getF64Type(), b.getF64FloatAttr(*p));
        }
        case EBaseType::INT8:
        case EBaseType::UINT8:
            return globalConstantAssign<8>(bc, obj, as_index);
        case EBaseType::INT16:
        case EBaseType::UINT16:
            return globalConstantAssign<16>(bc, obj, as_index);
        case EBaseType::INT32:
        case EBaseType::UINT32:
            return globalConstantAssign<32>(bc, obj, as_index);
        case EBaseType::INT64:
        case EBaseType::UINT64:
            return globalConstantAssign<64>(bc, obj, as_index);
        default:
            LUX_FF_FAIL(bc, "not a scalar base type");
        }
    }

    // ============================================================================
    // buildConstant — scalars, strings, and trivially-copyable aggregates.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::buildConstant(
        const meta::RefType& rt,
        BuilderContext& bc,
        const lux::meta::RuntimeObject& obj,
        bool is_seq
    )
    {
        using namespace lux::meta;
        auto& b = bc.builder;
        auto& loc = bc.loc;

        // Strings FIRST. std::string_view is itself standard-layout and
        // trivially copyable — a byte-copy dispatch placed before this check
        // would embed its {pointer, size} representation (a dangling host
        // pointer) into the module instead of the characters.
        if (obj.type()->hash == lux::cxx::type_hash<std::string>())
        {
            auto* str = static_cast<const std::string*>(obj.data());
            return globalStringConstantAssign(bc, str->data(), str->size());
        }
        if (obj.type()->hash == lux::cxx::type_hash<std::string_view>())
        {
            auto* str = static_cast<const std::string_view*>(obj.data());
            return globalStringConstantAssign(bc, str->data(), str->size());
        }

        // Scalar base types get first-class MLIR constants. Dispatch on
        // EBaseType, not on byte size: a size-only dispatch would catch
        // float/double in the integer branch and reinterpret their bytes,
        // and would turn small structs into bogus i32/i64 values.
        using lux::meta::EBaseType;
        switch (static_cast<EBaseType>(rt.qtype.base))
        {
        case EBaseType::BOOL:
        case EBaseType::FLOAT:
        case EBaseType::DOUBLE:
        case EBaseType::INT8:
        case EBaseType::UINT8:
        case EBaseType::INT16:
        case EBaseType::UINT16:
        case EBaseType::INT32:
        case EBaseType::UINT32:
        case EBaseType::INT64:
        case EBaseType::UINT64:
            return buildScalarConstantValue(bc, rt, obj, is_seq);
        default:
            break; // Record / Unknown — aggregate path below
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
        {
            LUX_FF_FAIL_AT_PIN(bc, "non-trivially-copyable class constants are not yet supported");
        }
        const auto& obj_type = *obj.type();
        if (obj_type.size == 0)
        {
            LUX_FF_FAIL_AT_PIN(bc, "aggregate constant has zero size");
        }

        return materializeBytesConstant(
            bc,
            obj_type.hash,
            llvm::StringRef(static_cast<const char*>(obj.data()), obj_type.size)
        );
    }

    // ============================================================================
    // materializeBytesConstant — one storage global per (type, value).
    //
    // Cache key is (type_hash, value_hash) so different VALUES of the same
    // type produce independent storage; a per-module ordinal in the symbol
    // name keeps hash collisions from ever aliasing two distinct globals.
    // ============================================================================
    mlir::Value MLIRBuilderImpl::materializeBytesConstant(BuilderContext& bc, uint64_t type_hash, llvm::StringRef bytes)
    {
        uint64_t value_hash = static_cast<uint64_t>(llvm::hash_value(bytes));
        std::pair<uint64_t, uint64_t> key{type_hash, value_hash};

        auto it = bc.class_globals.find(key);
        if (it == bc.class_globals.end())
        {
            auto arrTy = mlir::LLVM::LLVMArrayType::get(bc.builder.getI8Type(), bytes.size());
            std::string g_name = makeGlobalSymbol(bc, "g", value_hash, bc.class_globals.size() + 1);

            // Initializer = the actual value bytes. StringAttr is the
            // idiomatic initializer for LLVM array-of-i8 globals.
            auto initBytes = mlir::StringAttr::get(bc.ctx, bytes);

            mlir::OpBuilder::InsertionGuard guard(bc.builder);
            bc.builder.setInsertionPointToStart(bc.module.getBody());

            auto gObj = bc.builder.create<mlir::LLVM::GlobalOp>(
                bc.loc,
                arrTy,
                /*isConstant=*/true,
                mlir::LLVM::Linkage::Internal,
                g_name,
                initBytes
            );

            it = bc.class_globals.try_emplace(key, gObj).first;
        }

        auto sym = mlir::FlatSymbolRefAttr::get(bc.ctx, it->second.getSymName());
        auto ptr_type = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        return bc.builder.create<mlir::LLVM::AddressOfOp>(bc.loc, ptr_type, sym);
    }

    // ============================================================================
    // materializePureValue — on-demand expansion of the pure data subgraph.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::materializePureValue(PinId source, ValueMaps& vm, BuilderContext& bc)
    {
        const auto* record = bc.graph->topology().findPin(source);
        const auto* stored = record ? bc.graph->node(record->owner) : nullptr;
        const bool has_definition = stored && stored->definition;
        if (!has_definition)
        {
            LUX_FF_FAIL_AT_PIN(bc, "registered value node has no definition or payload");
        }
        if (stored->definition->hasExecutionCompiler())
        {
            LUX_FF_FAIL_AT_PIN(
                bc,
                "source value not materialised (its producer has not run "
                "on the exec chain yet)"
            );
        }
        const auto owner = record->owner;
        if (!materializing_.insert(owner).second)
        {
            LUX_FF_FAIL(bc, "cycle detected in pure data graph");
        }
        auto cycle_guard = llvm::make_scope_exit([&] { materializing_.erase(owner); });
        bc.current_node = owner;
        const auto& definition = *stored->definition;
        LUX_FF_TRY_VALUE(schema, definition.describePins(stored->payload));
        ValueCompiler compiler(bc);
        std::vector<FlowValue> inputs;
        std::vector<PinId> output_pins;
        for (const auto& declaration : schema)
        {
            const auto id = bc.graph->pinId(owner, declaration.semantic);
            if (!id.valid())
            {
                LUX_FF_FAIL(bc, "registered pin declaration differs from graph pins");
            }
            if (declaration.direction == graph::EPinDirection::OUTPUT)
            {
                output_pins.push_back(id);
                continue;
            }
            LUX_FF_TRY_VALUE(input, getOperand(id, vm, bc));
            // Reflected object addresses keep their pointer representation; the memory primitive
            // owns the exact unsupported-object diagnostic rather than a scalar coercion failure.
            if (!mlir::isa<mlir::LLVM::LLVMPointerType>(refTypeToMLIR(bc, *declaration.type)))
            {
                LUX_FF_TRY_VALUE(coerced, coerceScalar(bc, input, *declaration.type));
                input = coerced;
            }
            LUX_FF_TRY_VALUE(value, compiler.add(input, *declaration.type));
            inputs.push_back(value);
        }
        bc.current_node = owner;
        LUX_FF_TRY_VALUE(outputs, definition.compile(stored->payload, inputs, compiler));
        if (outputs.size() != output_pins.size())
        {
            LUX_FF_FAIL(bc, "registered output count differs from graph pins");
        }
        const bool cache_values = definition.valueEvaluation() == EFlowValueEvaluation::PURE;
        mlir::Value requested;
        for (std::size_t i = 0; i != outputs.size(); ++i)
        {
            const auto value = compiler.value(outputs[i]);
            if (cache_values)
            {
                vm.pure_scopes.back()[output_pins[i].value] = value;
            }
            if (output_pins[i] == source)
            {
                requested = value;
            }
        }
        if (!requested)
        {
            LUX_FF_FAIL_AT_PIN(bc, "registered source output is not declared");
        }
        return requested;
    }

    // ============================================================================
    // coerceScalar — implicit scalar conversion toward a declared type.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::coerceScalar(
        BuilderContext& bc,
        mlir::Value v,
        const lux::meta::RefType& dst_rt
    )
    {
        mlir::Type dst = refTypeToMLIR(bc, dst_rt);
        mlir::Type src = v.getType();
        if (src == dst)
        {
            return v;
        }

        auto& b = bc.builder;
        auto src_int = mlir::dyn_cast<mlir::IntegerType>(src);
        auto dst_int = mlir::dyn_cast<mlir::IntegerType>(dst);
        auto src_flt = mlir::dyn_cast<mlir::FloatType>(src);
        auto dst_flt = mlir::dyn_cast<mlir::FloatType>(dst);
        const bool dst_unsigned = detail::isUnsignedScalar(dst_rt);

        // The for-loop induction variable is an MLIR `index`; wiring it into
        // integer arithmetic is the single most common pattern, so cast it.
        if (mlir::isa<mlir::IndexType>(src) && dst_int)
        {
            return dst_unsigned ? b.create<mlir::arith::IndexCastUIOp>(bc.loc, dst, v).getResult()
                                : b.create<mlir::arith::IndexCastOp>(bc.loc, dst, v).getResult();
        }

        if (src_int && dst_int)
        {
            if (src_int.getWidth() < dst_int.getWidth())
            {
                // i1 (bool) always zero-extends — sign-extending `true`
                // would produce -1.
                return (dst_unsigned || src_int.getWidth() == 1)
                           ? b.create<mlir::arith::ExtUIOp>(bc.loc, dst, v).getResult()
                           : b.create<mlir::arith::ExtSIOp>(bc.loc, dst, v).getResult();
            }
            return b.create<mlir::arith::TruncIOp>(bc.loc, dst, v).getResult();
        }
        if (src_int && dst_flt)
        {
            return dst_unsigned // dst is float; use the source-ish signedness we have
                       ? b.create<mlir::arith::UIToFPOp>(bc.loc, dst, v).getResult()
                       : b.create<mlir::arith::SIToFPOp>(bc.loc, dst, v).getResult();
        }
        if (src_flt && dst_flt)
        {
            if (src_flt.getWidth() < dst_flt.getWidth())
            {
                return b.create<mlir::arith::ExtFOp>(bc.loc, dst, v).getResult();
            }
            return b.create<mlir::arith::TruncFOp>(bc.loc, dst, v).getResult();
        }

        LUX_FF_FAIL(
            bc,
            "no implicit conversion between the linked value's type and the "
            "pin's declared type"
        );
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
    // ============================================================================
    // mergeExecTokens — see header comment on the member declaration.
    // ============================================================================
    FlowForgeResult<mlir::Value> MLIRBuilderImpl::mergeExecTokens(
        BuilderContext& bc,
        const llvm::SmallVector<mlir::Value>& inputs
    )
    {
        if (inputs.empty())
        {
            LUX_FF_FAIL(bc, "mergeExecTokens called with no inputs");
        }

        // Fast path: all predecessors carry the same SSA value already
        // (no real divergence). Skip emitting a token_merge.
        mlir::Value unique;
        for (auto v : inputs)
        {
            if (!v)
            {
                continue;
            }
            if (!unique)
            {
                unique = v;
                continue;
            }
            if (v != unique)
            {
                unique = {};
                break;
            }
        }
        if (unique)
        {
            return unique;
        }

        return bc.builder.create<mlir::flowforge::TokenMergeOp>(bc.loc, bc.token, mlir::ValueRange{inputs}).getMerged();
    }

    // ============================================================================
    // Sequential lowering helpers
    // ============================================================================
    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::returnValues(std::span<const graph::PinSemanticId> results
    ) noexcept
    {
        LUX_FF_TRY(validatePins(results, graph::EPinDirection::INPUT, EFlowPinRole::DATA));

        // Collect return values from the node's data input pins (works for
        // both ReturnNode — none today — and FuncReturnNode). ReturnOp's
        // $rets is Variadic<AnyType> per FlowForgeOps.td. Scalars are
        // coerced to the pin's declared type, which matches the function's
        // result types by construction (FuncReturnNode mirrors the
        // FuncDefNode signature).
        llvm::SmallVector<mlir::Value, 4> operands;
        operands.push_back(in_tok_);
        for (const auto semantic : results)
        {
            const auto id = pin(semantic);
            const auto* input = bc_.graph->pin(id);
            LUX_FF_TRY_VALUE(v, builder_.getOperand(id, vm_, bc_));
            const bool needs_scalar_coercion =
                input->type && !mlir::isa<mlir::LLVM::LLVMPointerType>(refTypeToMLIR(bc_, *input->type)) &&
                !mlir::isa<mlir::LLVM::LLVMPointerType>(v.getType());
            if (needs_scalar_coercion)
            {
                LUX_FF_TRY_VALUE(coerced, builder_.coerceScalar(bc_, v, *input->type));
                v = coerced;
            }
            operands.push_back(v);
        }
        bc_.builder.create<mlir::flowforge::ReturnOp>(bc_.loc, mlir::TypeRange{}, operands);
        token_ = {};
        return {};
    }

    // ============================================================================
    // Native function call lowering — emits a func.func declaration at module
    // top and a func.call at the current insertion point, threading the token
    // through as a no-op (sequential calls don't fork the exec edge).
    // ============================================================================
    namespace
    {
        // Look up or create a func.func DECLARATION (empty body) at module top
        // for the given external symbol. Dedupes by name within one build, so
        // multiple call sites to the same symbol share one declaration.
        static mlir::func::FuncOp getOrDeclareExternFunc(
            BuilderContext& bc,
            llvm::StringRef name,
            mlir::FunctionType ty
        )
        {
            auto it = bc.extern_funcs.find(name);
            if (it != bc.extern_funcs.end())
            {
                return it->second;
            }

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
    static mlir::Value allocaAtEntry(BuilderContext& bc, mlir::Type elem_ty, int64_t count)
    {
        mlir::OpBuilder::InsertionGuard guard(bc.builder);
        auto* entry = &bc.main_func.getBody().front();
        bc.builder.setInsertionPointToStart(entry);
        auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc.ctx);
        auto n = bc.builder.create<mlir::LLVM::ConstantOp>(
            bc.loc,
            bc.builder.getI32Type(),
            bc.builder.getI32IntegerAttr(static_cast<int32_t>(count))
        );
        return bc.builder.create<mlir::LLVM::AllocaOp>(bc.loc, ptr_ty, elem_ty, n);
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::abilityCall(
        const ScriptAbilityPayload& call,
        std::span<const graph::PinSemanticId> arguments,
        std::span<const graph::PinSemanticId> results,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(arguments, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(results, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        const auto node_id = node_.value;
        const auto ordinal = bc_.ability_ordinals.find(node_id);
        const bool is_invalid_shape = ordinal == bc_.ability_ordinals.end() || call.results().size() > 1U;
        if (is_invalid_shape)
        {
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::UNSUPPORTED_SCRIPT_ABILITY_TYPE,
                .message = "Script Ability node has an unsupported result shape",
                .node_id = node_id
            });
        }

        llvm::SmallVector<mlir::Value, 6> operands;
        llvm::SmallVector<mlir::Type, 6> argument_types;
        operands.push_back(bc_.ability_runtime);
        argument_types.push_back(bc_.ability_runtime.getType());
        for (const auto semantic : arguments)
        {
            const auto id = pin(semantic);
            const auto* input = bc_.graph->pin(id);
            bc_.current_pin = id;
            if (input->type == nullptr)
            {
                LUX_FF_FAIL_AT_PIN(bc_, "Script Ability parameter has no semantic type");
            }
            auto type = refTypeToMLIR(bc_, *input->type);
            if (mlir::isa<mlir::LLVM::LLVMPointerType>(type))
            {
                return lux::cxx::unexpected(FlowForgeFailure{
                    .code = EFlowForgeError::UNSUPPORTED_SCRIPT_ABILITY_TYPE,
                    .message = "record Script Ability parameters are not supported by FlowForge S3",
                    .node_id = node_id,
                    .pin_id = id.value
                });
            }
            LUX_FF_TRY_VALUE(value, builder_.getOperand(id, vm_, bc_));
            if (mlir::isa<mlir::LLVM::LLVMPointerType>(value.getType()))
            {
                LUX_FF_FAIL_AT_PIN(bc_, "Script Ability parameter cannot be coerced to its semantic type");
            }
            LUX_FF_TRY_VALUE(coerced, builder_.coerceScalar(bc_, value, *input->type));
            operands.push_back(coerced);
            argument_types.push_back(type);
        }

        llvm::SmallVector<mlir::Type, 1> result_types;
        if (!results.empty())
        {
            const auto* type = bc_.graph->pin(pin(results.front()))->type;
            const bool is_unsupported_type =
                type == nullptr || mlir::isa<mlir::LLVM::LLVMPointerType>(refTypeToMLIR(bc_, *type));
            if (is_unsupported_type)
            {
                return lux::cxx::unexpected(FlowForgeFailure{
                    .code = EFlowForgeError::UNSUPPORTED_SCRIPT_ABILITY_TYPE,
                    .message = "record Script Ability results are not supported by FlowForge S3",
                    .node_id = node_id,
                    .pin_id = pin(results.front()).value
                });
            }
            result_types.push_back(refTypeToMLIR(bc_, *type));
        }

        const auto name =
            call.methodKind() == lux::script::EScriptApiMethodKind::ASYNC_OPERATION
                ? "lux_ff_ability_async_" + std::to_string(ordinal->second) + "_node_" + std::to_string(node_id)
                : "lux_ff_ability_sync_" + std::to_string(ordinal->second);
        const auto function_type = bc_.builder.getFunctionType(argument_types, result_types);
        const auto function = getOrDeclareExternFunc(bc_, name, function_type);
        auto invoked = bc_.builder.create<mlir::func::CallOp>(bc_.loc, function, operands);
        if (!results.empty())
        {
            vm_.exec_data[pin(results.front()).value] = invoked.getResult(0);
        }
        completed(done);
        bc_.current_pin = {};
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::eventWait(
        const ScriptEventPayload&,
        graph::PinSemanticId payload,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(std::array{payload}, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        const auto node_id = node_.value;
        const auto ordinal = bc_.event_wait_ordinals.find(node_id);
        const auto* type = bc_.graph->pin(pin(payload))->type;
        const bool has_payload = ordinal != bc_.event_wait_ordinals.end() && type != nullptr;
        if (!has_payload)
        {
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH,
                .message = "Script Event wait has no canonical payload",
                .node_id = node_id
            });
        }
        const auto result_type = refTypeToMLIR(bc_, *type);
        if (mlir::isa<mlir::LLVM::LLVMPointerType>(result_type))
        {
            return lux::cxx::unexpected(FlowForgeFailure{
                .code = EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH,
                .message = "record Script Event payloads are not supported by FlowForge S5.1",
                .node_id = node_id,
                .pin_id = pin(payload).value
            });
        }
        const auto name = "lux_ff_event_wait_" + std::to_string(ordinal->second) + "_node_" + std::to_string(node_id);
        const auto function_type = bc_.builder.getFunctionType({}, {result_type});
        const auto function = getOrDeclareExternFunc(bc_, name, function_type);
        auto invoked = bc_.builder.create<mlir::func::CallOp>(bc_.loc, function, mlir::ValueRange{});
        vm_.exec_data[pin(payload).value] = invoked.getResult(0);
        completed(done);
        return {};
    }

    FlowForgeResult<void> MLIRBuilderImpl::ExecutionCompiler::nativeCall(
        const NativeCallDefinition& definition,
        std::span<const graph::PinSemanticId> arguments,
        graph::PinSemanticId result,
        graph::PinSemanticId done
    ) noexcept
    {
        LUX_FF_TRY(validatePins(arguments, graph::EPinDirection::INPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{result}, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA));
        LUX_FF_TRY(validatePins(std::array{done}, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION));

        const auto& info = definition.signature();
        auto& b = bc_.builder;
        auto loc = bc_.loc;
        auto ptr_ty = mlir::LLVM::LLVMPointerType::get(bc_.ctx);

        const bool returns_void =
            static_cast<lux::meta::EBaseType>(info.return_type.qtype.base) == lux::meta::EBaseType::VOID &&
            !isPointerQual(info.return_type);

        // Collect + coerce operands from data input pins (shared by both
        // call flavors): scalars coerce to the declared parameter type,
        // pointer-typed parameters pass through untouched.
        llvm::SmallVector<mlir::Value, 4> operands;
        operands.reserve(arguments.size());
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            LUX_FF_TRY_VALUE(v, builder_.getOperand(pin(arguments[i]), vm_, bc_));
            if (i < info.parameters.size())
            {
                const auto& prt = info.parameters[i].type;
                const bool needs_scalar_coercion = !mlir::isa<mlir::LLVM::LLVMPointerType>(refTypeToMLIR(bc_, prt)) &&
                                                   !mlir::isa<mlir::LLVM::LLVMPointerType>(v.getType());
                if (needs_scalar_coercion)
                {
                    LUX_FF_TRY_VALUE(coerced, builder_.coerceScalar(bc_, v, prt));
                    v = coerced;
                }
            }
            operands.push_back(v);
        }

        mlir::Value result_value; // null when void

        if (info.invoker == nullptr)
        {
            // ---- hand-written RefFunction: direct C-ABI call -------------
            llvm::SmallVector<mlir::Type, 4> argTypes;
            argTypes.reserve(info.parameters.size());
            for (auto& param : info.parameters)
            {
                argTypes.push_back(refTypeToMLIR(bc_, param.type));
            }
            llvm::SmallVector<mlir::Type, 1> retTypes;
            if (!returns_void)
            {
                retTypes.push_back(refTypeToMLIR(bc_, info.return_type));
            }

            auto fnTy = b.getFunctionType(argTypes, retTypes);
            auto funcOp = getOrDeclareExternFunc(bc_, info.name, fnTy);
            auto callOp = b.create<mlir::func::CallOp>(loc, funcOp, operands);
            const bool has_result = !returns_void && callOp.getNumResults() > 0;
            if (has_result)
            {
                result_value = callOp.getResult(0);
            }
        }
        else
        {
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
            auto funcOp = getOrDeclareExternFunc(bc_, sym, fnTy);

            auto null_ptr = b.create<mlir::LLVM::ZeroOp>(loc, ptr_ty).getResult();

            mlir::Value args_base = null_ptr;
            if (!operands.empty())
            {
                args_base = allocaAtEntry(bc_, ptr_ty, static_cast<int64_t>(operands.size()));
                for (size_t i = 0; i < operands.size(); ++i)
                {
                    mlir::Value storage;
                    const bool operand_is_ptr = mlir::isa<mlir::LLVM::LLVMPointerType>(operands[i].getType());
                    const bool param_is_ptr_qual = i < info.parameters.size() && isPointerQual(info.parameters[i].type);
                    const bool is_by_value_record = operand_is_ptr && !param_is_ptr_qual;
                    if (is_by_value_record)
                    {
                        // by-value record: the operand already points at a T.
                        storage = operands[i];
                    }
                    else
                    {
                        // scalar or pointer parameter: spill into a slot.
                        storage = allocaAtEntry(bc_, operands[i].getType(), 1);
                        b.create<mlir::LLVM::StoreOp>(loc, operands[i], storage);
                    }
                    auto slot = b.create<mlir::LLVM::GEPOp>(
                        loc,
                        ptr_ty,
                        ptr_ty,
                        args_base,
                        llvm::ArrayRef<mlir::LLVM::GEPArg>{static_cast<int32_t>(i)}
                    );
                    b.create<mlir::LLVM::StoreOp>(loc, storage, slot);
                }
            }

            mlir::Value ret_slot = null_ptr;
            mlir::Type ret_ty;
            if (!returns_void)
            {
                ret_ty = refTypeToMLIR(bc_, info.return_type);
                const bool is_unsupported_record =
                    mlir::isa<mlir::LLVM::LLVMPointerType>(ret_ty) && !isPointerQual(info.return_type);
                if (is_unsupported_record)
                {
                    LUX_FF_FAIL(
                        bc_,
                        "record-typed return values are not supported for "
                        "reflected calls yet"
                    );
                }
                ret_slot = allocaAtEntry(bc_, ret_ty, 1);
            }

            b.create<mlir::func::CallOp>(loc, funcOp, mlir::ValueRange{null_ptr, args_base, ret_slot});
            if (!returns_void)
            {
                result_value = b.create<mlir::LLVM::LoadOp>(loc, ret_ty, ret_slot);
            }
        }

        // Map return value to the result DataOutPin (if not void).
        if (result_value)
        {
            vm_.exec_data[pin(result).value] = result_value;
        }

        // Thread exec token through (sequential call): outTok = inTok.
        completed(done);
        return {};
    }

    // ============================================================================
    // MLIRBuilder thin wrapper
    // ============================================================================
    FlowForgeResult<MLIRBuilder> MLIRBuilder::create(IRContext& context) noexcept
    {
        try
        {
            auto impl = std::make_unique<MLIRBuilderImpl>(static_cast<mlir::MLIRContext*>(context.context()));
            return MLIRBuilder(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }

    MLIRBuilder::MLIRBuilder(std::unique_ptr<MLIRBuilderImpl> impl) noexcept : impl_(std::move(impl)) {}

    MLIRBuilder::~MLIRBuilder() = default;
    MLIRBuilder::MLIRBuilder(MLIRBuilder&&) noexcept = default;
    MLIRBuilder& MLIRBuilder::operator=(MLIRBuilder&&) noexcept = default;

    FlowForgeResult<std::unique_ptr<IR>> MLIRBuilder::generateIR(const FlowGraph& graph) noexcept
    {
        try
        {
            return impl_->generateMLIR(graph);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }

} // namespace lux::flowforge

#undef LUX_FF_TRY
#undef LUX_FF_TRY_VALUE
#undef LUX_FF_FAIL_AT_PIN
#undef LUX_FF_FAIL
