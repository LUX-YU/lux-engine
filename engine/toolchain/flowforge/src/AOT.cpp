//===========================================================================
// AOT.cpp — FlowGraph -> native shared library speaking lux_script_abi.
//
// Pipeline:
//   1. generateIR + lowerToLLVM (the exact JIT pipeline) -> LLVM-dialect
//      MLIR, then translateModuleToLLVMIR -> llvm::Module.
//   2. IMPORT SLOTS: every declared-but-undefined function (reflected
//      `_lfi_<hash>` trampolines, hand-written C-ABI hosts) is rewritten
//      into an indirect call through an internal `ptr` global. No symbol
//      remains for the dynamic linker to resolve — binding is explicit.
//   3. lux_script_bind_host(resolve, ctx, ver): straight-line code filling
//      every slot via resolve(ctx, "<name>") and returning the number of
//      unresolved imports (0 = success). Only emitted when imports exist.
//   4. Per OnEvent entry `lux_event_X(state, abilities, a0..aN)`: a call_frame
//      wrapper pulls the explicit native instance context from the v4 frame,
//      loads each argument from args[i].data, calls the event function and
//      returns 0. Frame/slot field offsets
//      come from offsetof() on the REAL C structs — both sides compile
//      against the same lux_script_abi.h.
//   5. A static lux_script_module_desc (function table = the wrappers,
//      names = event display names) + lux_script_get_module() returning
//      it. get_module and bind_host are dllexport'ed; nothing else is.
//   6. TargetMachine -> COFF object bytes; linkSharedLibrary runs lld-link
//      (or link.exe) out of process: /DLL /NOENTRY /NODEFAULTLIB.
//===========================================================================
#include "lux/engine/flowforge/compiler/AOT.hpp"
#include "lux/engine/flowforge/compiler/IR.hpp"
#include "lux/engine/flowforge/compiler/IRImpl.hpp"
#include "lux/engine/flowforge/compiler/Passes.hpp"
#include "lux/engine/flowforge/compiler/ScriptInstance.hpp"
#include "lux/engine/flowforge/compiler/SuspensionAnalysis.hpp"
#include "lux/engine/flowforge/script/ScriptEventAwaitNode.hpp"
#include "lux/engine/flowforge/graph/FlowGraph.hpp"
#include "lux/engine/flowforge/graph/FunctionalNode.hpp"
#include "lux/engine/flowforge/script/ScriptAbilityNode.hpp"

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/description/Script.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/meta/Meta.hpp>

#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Export.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <unordered_set>

// The descriptor globals are emitted as LLVM constant structs whose layout
// must byte-match the C structs the runtime reads. The natural LLVM layout
// of the element sequences used below does match MSVC/Itanium for these
// plain structs; the static_asserts pin the C side, and emitModuleDesc
// re-checks the LLVM side against the DataLayout at cook time.
static_assert(sizeof(lux_script_type_desc) == 32, "ABI drift: type_desc");
static_assert(sizeof(lux_script_function_desc) == 64, "ABI drift: function_desc");
static_assert(sizeof(lux_script_module_desc) == 80, "ABI drift: module_desc");
static_assert(sizeof(lux_script_event_wait_import_desc) == 64, "ABI drift: event_wait_import_desc");
static_assert(sizeof(lux_script_value_slot) == 24, "ABI drift: value_slot");

namespace lux::flowforge
{
    namespace
    {
        struct EventInfo
        {
            const OnEventNode* node;
            std::string symbol;
            lux::script::ScriptSymbolId authored_symbol{};
        };

        struct AbilityImportInfo final
        {
            const ScriptAbilityNode* node{};
        };

        struct EventWaitImportInfo final
        {
            const ScriptEventAwaitNode* node{};
        };

        struct NativeStepInfo final
        {
            llvm::Function* start{};
            llvm::Function* resume{};
            llvm::Function* destroy{};
            std::uint32_t frame_size{};
            std::uint32_t frame_align{1U};
            std::uint64_t frame_hash{};
        };

        [[nodiscard]] std::vector<AbilityImportInfo> collectAbilityImports(const FlowGraph& graph)
        {
            std::vector<const ScriptAbilityNode*> nodes;
            for (const auto& storage : graph.nodes())
            {
                if (storage.node->operation() == ENodeOperation::SCRIPT_ABILITY_CALL)
                    nodes.push_back(static_cast<const ScriptAbilityNode*>(storage.node.get()));
            }
            std::ranges::sort(nodes, [](const auto* left, const auto* right) {
                return left->contract().name() < right->contract().name() ||
                    (left->contract().name() == right->contract().name() &&
                     left->method().name() < right->method().name());
            });
            std::vector<AbilityImportInfo> result;
            for (const auto* node : nodes)
            {
                if (!result.empty() && result.back().node->contract() == node->contract() &&
                    result.back().node->method() == node->method())
                {
                    continue;
                }
                result.push_back({node});
            }
            return result;
        }

        [[nodiscard]] std::vector<EventWaitImportInfo> collectEventWaitImports(const FlowGraph& graph)
        {
            std::vector<const ScriptEventAwaitNode*> nodes;
            for (const auto& storage : graph.nodes())
            {
                if (storage.node->operation() == ENodeOperation::SCRIPT_EVENT_WAIT)
                    nodes.push_back(static_cast<const ScriptEventAwaitNode*>(storage.node.get()));
            }
            std::ranges::sort(nodes, [](const auto* left, const auto* right) {
                const auto& a = left->source();
                const auto& b = right->source();
                if (a.system_id != b.system_id)
                    return a.system_id < b.system_id;
                if (a.event_id != b.event_id)
                    return a.event_id < b.event_id;
                return a.route < b.route;
            });
            std::vector<EventWaitImportInfo> result;
            for (const auto* node : nodes)
            {
                if (!result.empty())
                {
                    const auto& previous = result.back().node->source();
                    const auto& current = node->source();
                    if (previous.system_id == current.system_id && previous.event_id == current.event_id &&
                        previous.route == current.route)
                    {
                        continue;
                    }
                }
                result.push_back({node});
            }
            return result;
        }

        [[nodiscard]] std::optional<lux::rdesc::ScriptValueType> projectType(const lux::meta::RefType& type)
        {
            using lux::meta::EBaseType;
            using lux::meta::ETypeQual;
            const auto qualifier = static_cast<ETypeQual>(type.qtype.qual);
            const bool is_unsupported_qualifier = qualifier != ETypeQual::Value && qualifier != ETypeQual::LRefToConst;
            if (is_unsupported_qualifier)
            {
                return std::nullopt;
            }

            const auto builtin = [&type]() -> std::optional<lux::rdesc::ScriptValueType> {
                switch (static_cast<EBaseType>(type.qtype.base))
                {
                case EBaseType::Bool:
                    return lux::rdesc::makeScriptValueType<bool>();
                case EBaseType::Int32:
                    return lux::rdesc::makeScriptValueType<std::int32_t>();
                case EBaseType::Uint32:
                    return lux::rdesc::makeScriptValueType<std::uint32_t>();
                case EBaseType::Int64:
                    return lux::rdesc::makeScriptValueType<std::int64_t>();
                case EBaseType::Uint64:
                    return lux::rdesc::makeScriptValueType<std::uint64_t>();
                case EBaseType::Float:
                    return lux::rdesc::makeScriptValueType<float>();
                case EBaseType::Double:
                    return lux::rdesc::makeScriptValueType<double>();
                default:
                    return std::nullopt;
                }
            }();
            if (builtin)
            {
                const bool is_layout_mismatch = builtin->size != type.size || builtin->alignment != type.alignment;
                if (is_layout_mismatch)
                {
                    return std::nullopt;
                }
                return builtin;
            }

            if (static_cast<EBaseType>(type.qtype.base) != EBaseType::Record || type.name.empty() || type.size == 0U ||
                type.alignment == 0U || (type.alignment & (type.alignment - 1U)) != 0U)
            {
                return std::nullopt;
            }
            return lux::rdesc::ScriptValueType{
                std::string(type.name),
                lux::semantic::typeId(type.name),
                lux::semantic::EValuePass::CONST_REF,
                static_cast<std::uint8_t>(lux::semantic::EAbiKind::STRUCT_REF),
                type.size,
                type.alignment
            };
        }

        llvm::Constant* makeCStr(llvm::Module& m, llvm::StringRef s, const llvm::Twine& name)
        {
            auto* data = llvm::ConstantDataArray::getString(
                m.getContext(),
                s,
                /*AddNull=*/true
            );
            auto* g = new llvm::GlobalVariable(
                m,
                data->getType(),
                /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage,
                data,
                name
            );
            return g; // opaque-pointer world: the global IS a ptr constant
        }

        void exportSymbol(llvm::Function* f, const llvm::Triple& triple)
        {
            if (triple.isOSBinFormatCOFF())
                f->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
            else
                f->setVisibility(llvm::GlobalValue::DefaultVisibility);
        }

        void storeValueSlot(
            llvm::IRBuilder<>& builder,
            llvm::Value* slot,
            const lux::script::ScriptAbilityValueDescription& type,
            llvm::Value* data
        )
        {
            auto& context = builder.getContext();
            auto* i8 = llvm::Type::getInt8Ty(context);
            auto* i32 = llvm::Type::getInt32Ty(context);
            auto* i64 = llvm::Type::getInt64Ty(context);
            const auto field = [&](std::size_t offset) {
                return builder.CreateGEP(i8, slot, llvm::ConstantInt::get(i64, offset));
            };
            builder.CreateStore(
                llvm::ConstantInt::get(i8, type.abi_kind),
                field(offsetof(lux_script_value_slot, kind))
            );
            builder.CreateStore(llvm::ConstantInt::get(i32, type.size), field(offsetof(lux_script_value_slot, size)));
            builder.CreateStore(
                llvm::ConstantInt::get(i64, type.type_id),
                field(offsetof(lux_script_value_slot, type_id))
            );
            builder.CreateStore(data, field(offsetof(lux_script_value_slot, data)));
        }

        bool materializeSynchronousAbilityCalls(
            llvm::Module& module,
            const std::vector<AbilityImportInfo>& imports,
            std::string& error
        )
        {
            auto& context = module.getContext();
            auto* ptr_type = llvm::PointerType::get(context, 0);
            auto* i8 = llvm::Type::getInt8Ty(context);
            auto* i32 = llvm::Type::getInt32Ty(context);
            auto* i64 = llvm::Type::getInt64Ty(context);
            for (std::size_t ordinal{}; ordinal < imports.size(); ++ordinal)
            {
                const auto& description = *imports[ordinal].node;
                if (description.methodKind() == lux::script::EScriptApiMethodKind::ASYNC_OPERATION)
                    continue;
                const auto name = "lux_ff_ability_sync_" + std::to_string(ordinal);
                auto* function = module.getFunction(name);
                if (function == nullptr || !function->isDeclaration() ||
                    function->arg_size() != 1U + description.parameters().size())
                {
                    error = "missing or invalid synchronous Script Ability import '" + name + "'";
                    return false;
                }

                auto* block = llvm::BasicBlock::Create(context, "entry", function);
                llvm::IRBuilder<> builder(block);
                auto* runtime = function->getArg(0);
                const auto runtime_field = [&](std::size_t offset) {
                    return builder.CreateGEP(i8, runtime, llvm::ConstantInt::get(i64, offset));
                };
                auto* runtime_context =
                    builder.CreateLoad(ptr_type, runtime_field(offsetof(lux_script_ability_runtime, context)));
                auto* invoke =
                    builder.CreateLoad(ptr_type, runtime_field(offsetof(lux_script_ability_runtime, invoke)));

                llvm::Value* arguments = llvm::ConstantPointerNull::get(ptr_type);
                if (!description.parameters().empty())
                {
                    arguments = builder.CreateAlloca(
                        i8,
                        llvm::ConstantInt::get(i32, description.parameters().size() * sizeof(lux_script_value_slot))
                    );
                    for (std::size_t index{}; index < description.parameters().size(); ++index)
                    {
                        auto* storage = builder.CreateAlloca(function->getArg(index + 1U)->getType());
                        builder.CreateStore(function->getArg(index + 1U), storage);
                        auto* slot = builder.CreateGEP(
                            i8,
                            arguments,
                            llvm::ConstantInt::get(i64, index * sizeof(lux_script_value_slot))
                        );
                        storeValueSlot(builder, slot, description.parameters()[index].value, storage);
                    }
                }

                llvm::Value* results = llvm::ConstantPointerNull::get(ptr_type);
                llvm::Value* result_storage{};
                if (!description.results().empty())
                {
                    results = builder.CreateAlloca(i8, llvm::ConstantInt::get(i32, sizeof(lux_script_value_slot)));
                    result_storage = builder.CreateAlloca(function->getReturnType());
                    storeValueSlot(builder, results, description.results().front(), result_storage);
                }

                auto* invoke_type = llvm::FunctionType::get(i32, {ptr_type, i32, ptr_type, i32, ptr_type, i32}, false);
                const auto status = builder.CreateCall(
                    invoke_type,
                    invoke,
                    {runtime_context,
                     llvm::ConstantInt::get(i32, ordinal),
                     arguments,
                     llvm::ConstantInt::get(i32, description.parameters().size()),
                     results,
                     llvm::ConstantInt::get(i32, description.results().size())}
                );
                if (description.results().empty())
                {
                    builder.CreateRetVoid();
                }
                else
                {
                    const auto value = builder.CreateLoad(function->getReturnType(), result_storage);
                    const auto success = builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, 0));
                    builder.CreateRet(
                        builder.CreateSelect(success, value, llvm::Constant::getNullValue(value->getType()))
                    );
                }
            }
            return true;
        }

        [[nodiscard]] std::uint64_t appendFrameHash(std::uint64_t hash, std::uint64_t value) noexcept
        {
            for (std::uint32_t shift{}; shift < 64U; shift += 8U)
            {
                hash ^= static_cast<std::uint8_t>(value >> shift);
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        [[nodiscard]] std::uint64_t alignFrameOffset(std::uint64_t value, std::uint64_t alignment) noexcept
        {
            return (value + alignment - 1U) & ~(alignment - 1U);
        }

        void storeOutcomeField(llvm::IRBuilder<>& builder, llvm::Value* outcome, std::size_t offset, llvm::Value* value)
        {
            auto* address =
                builder.CreateGEP(builder.getInt8Ty(), outcome, llvm::ConstantInt::get(builder.getInt64Ty(), offset));
            builder.CreateStore(value, address);
        }

        bool lowerEventToStateMachine(
            llvm::Module& module,
            const EventInfo& event,
            const std::vector<AbilityImportInfo>& imports,
            const std::vector<EventWaitImportInfo>& event_imports,
            NativeStepInfo& result,
            std::string& error
        )
        {
            auto* target = module.getFunction(event.symbol);
            if (target == nullptr || target->getReturnType()->isVoidTy() == false)
            {
                error = "FlowForge async export has an invalid native function";
                return false;
            }

            struct AwaitSite final
            {
                llvm::CallInst* call{};
                std::uint32_t import_ordinal{};
                std::uint64_t node_id{};
                bool event_wait{};
            };
            std::vector<AwaitSite> awaits;
            for (auto& block : *target)
            {
                for (auto& instruction : block)
                {
                    auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                    const auto* callee = call != nullptr ? call->getCalledFunction() : nullptr;
                    if (callee == nullptr)
                        continue;
                    const bool is_ability = callee->getName().starts_with("lux_ff_ability_async_");
                    const bool is_event = callee->getName().starts_with("lux_ff_event_wait_");
                    if (!is_ability && !is_event)
                        continue;
                    const auto prefix_size = is_event ? std::strlen("lux_ff_event_wait_") :
                        std::strlen("lux_ff_ability_async_");
                    const auto suffix = callee->getName().drop_front(prefix_size);
                    const auto separator = suffix.find("_node_");
                    std::uint64_t ordinal{};
                    std::uint64_t node_id{};
                    if (separator == llvm::StringRef::npos || suffix.take_front(separator).getAsInteger(10, ordinal) ||
                        suffix.drop_front(separator + 6U).getAsInteger(10, node_id) ||
                        (is_event ? ordinal >= event_imports.size() : ordinal >= imports.size()))
                    {
                        error = "FlowForge suspension marker has an invalid stable identity";
                        return false;
                    }
                    awaits.push_back({call, static_cast<std::uint32_t>(ordinal), node_id, is_event});
                }
            }
            if (awaits.empty())
                return true;
            std::ranges::sort(awaits, {}, &AwaitSite::node_id);

            std::vector<llvm::PHINode*> phis;
            std::vector<llvm::Instruction*> registers;
            for (auto& block : *target)
            {
                for (auto& instruction : block)
                {
                    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction))
                        phis.push_back(phi);
                    else if (!instruction.getType()->isVoidTy() && !llvm::isa<llvm::AllocaInst>(instruction) &&
                             !instruction.isTerminator() && !instruction.use_empty())
                        registers.push_back(&instruction);
                }
            }
            auto* alloca_point = &*target->getEntryBlock().getFirstInsertionPt();
            for (auto* phi : phis)
                llvm::DemotePHIToStack(phi, alloca_point);
            for (auto* instruction : registers)
            {
                if (instruction->getParent() != nullptr && !instruction->use_empty())
                    llvm::DemoteRegToStack(*instruction, false, alloca_point);
            }

            auto& context = module.getContext();
            auto* ptr_type = llvm::PointerType::get(context, 0);
            auto* i8 = llvm::Type::getInt8Ty(context);
            auto* i32 = llvm::Type::getInt32Ty(context);
            auto* i64 = llvm::Type::getInt64Ty(context);
            std::vector<llvm::Type*> core_arguments(target->getFunctionType()->params());
            core_arguments.insert(core_arguments.end(), {ptr_type, ptr_type, ptr_type, ptr_type});
            auto* core_type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), core_arguments, false);
            auto* core = llvm::Function::Create(
                core_type,
                llvm::GlobalValue::InternalLinkage,
                "lux_step_core_" + event.symbol,
                module
            );
            core->setDSOLocal(true);
            llvm::ValueToValueMapTy value_map;
            auto core_argument = core->arg_begin();
            for (auto& argument : target->args())
            {
                value_map[&argument] = &*core_argument;
                ++core_argument;
            }
            auto* frame_argument = &*core_argument++;
            auto* host_argument = &*core_argument++;
            auto* packet_argument = &*core_argument++;
            auto* outcome_argument = &*core_argument;
            llvm::SmallVector<llvm::ReturnInst*, 8> cloned_returns;
            llvm::CloneFunctionInto(
                core,
                target,
                value_map,
                llvm::CloneFunctionChangeType::LocalChangesOnly,
                cloned_returns
            );
            core->setDSOLocal(true);

            struct FrameSlot final
            {
                llvm::AllocaInst* allocation{};
                std::uint64_t offset{};
                std::uint64_t size{};
                std::uint64_t alignment{};
            };
            std::vector<std::uint64_t> argument_offsets;
            std::vector<FrameSlot> slots;
            std::uint64_t frame_size{sizeof(std::uint32_t)};
            std::uint64_t frame_alignment{alignof(std::uint32_t)};
            const auto& layout = module.getDataLayout();
            for (auto* type : target->getFunctionType()->params())
            {
                const auto alignment = layout.getABITypeAlign(type).value();
                frame_size = alignFrameOffset(frame_size, alignment);
                argument_offsets.push_back(frame_size);
                frame_size += layout.getTypeAllocSize(type);
                frame_alignment = (std::max)(frame_alignment, static_cast<std::uint64_t>(alignment));
            }
            for (auto& block : *core)
            {
                for (auto& instruction : block)
                {
                    auto* allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
                    if (allocation == nullptr)
                        continue;
                    const auto* count = llvm::dyn_cast<llvm::ConstantInt>(allocation->getArraySize());
                    if (count == nullptr)
                    {
                        error = "FlowForge continuation frame contains a dynamic allocation";
                        return false;
                    }
                    const auto alignment = layout.getABITypeAlign(allocation->getAllocatedType()).value();
                    frame_size = alignFrameOffset(frame_size, alignment);
                    const auto size = layout.getTypeAllocSize(allocation->getAllocatedType()) * count->getZExtValue();
                    slots.push_back({allocation, frame_size, size, alignment});
                    frame_size += size;
                    frame_alignment = (std::max)(frame_alignment, static_cast<std::uint64_t>(alignment));
                }
            }
            std::uint64_t scratch_size{sizeof(lux_script_async_token)};
            std::uint64_t scratch_alignment{alignof(lux_script_async_token)};
            for (const auto& await : awaits)
            {
                if (await.event_wait)
                    continue;
                const auto& import = *imports[await.import_ordinal].node;
                std::uint64_t required = import.parameters().size() * sizeof(lux_script_value_slot);
                for (const auto& parameter : import.parameters())
                {
                    required = alignFrameOffset(required, parameter.value.alignment);
                    required += parameter.value.size;
                    scratch_alignment =
                        (std::max)(scratch_alignment, static_cast<std::uint64_t>(parameter.value.alignment));
                }
                required = alignFrameOffset(required, alignof(lux_script_async_token));
                required += sizeof(lux_script_async_token);
                scratch_size = (std::max)(scratch_size, required);
            }
            const auto scratch_offset = alignFrameOffset(frame_size, scratch_alignment);
            frame_size = scratch_offset + scratch_size;
            frame_alignment = (std::max)(frame_alignment, scratch_alignment);
            frame_size = alignFrameOffset(frame_size, frame_alignment);
            if (frame_size == 0U || frame_size > (std::numeric_limits<std::uint32_t>::max)() ||
                frame_alignment > (std::numeric_limits<std::uint32_t>::max)())
            {
                error = "FlowForge continuation frame layout exceeds the native ABI";
                return false;
            }

            for (auto& slot : slots)
            {
                llvm::SmallVector<llvm::Use*, 16> uses;
                for (auto& use : slot.allocation->uses())
                    uses.push_back(std::addressof(use));
                for (auto* use : uses)
                {
                    auto* user = llvm::cast<llvm::Instruction>(use->getUser());
                    llvm::IRBuilder<> builder(user);
                    auto* address = builder.CreateGEP(i8, frame_argument, llvm::ConstantInt::get(i64, slot.offset));
                    use->set(address);
                }
                slot.allocation->eraseFromParent();
            }

            auto* cloned_entry = llvm::cast<llvm::BasicBlock>(value_map[&target->getEntryBlock()]);
            auto* dispatch = llvm::BasicBlock::Create(context, "dispatch", core, cloned_entry);
            llvm::IRBuilder<> dispatch_builder(dispatch);
            auto* pc = dispatch_builder.CreateLoad(i32, frame_argument, "pc");
            auto* invalid_pc = llvm::BasicBlock::Create(context, "invalid.pc", core);
            auto* dispatch_switch = dispatch_builder.CreateSwitch(pc, invalid_pc, awaits.size() + 1U);
            dispatch_switch->addCase(llvm::ConstantInt::get(i32, 0U), cloned_entry);
            llvm::IRBuilder<> invalid_builder(invalid_pc);
            storeOutcomeField(
                invalid_builder,
                outcome_argument,
                offsetof(lux_script_step_outcome, state),
                llvm::ConstantInt::get(i8, LUX_SCRIPT_STEP_FAILED)
            );
            storeOutcomeField(
                invalid_builder,
                outcome_argument,
                offsetof(lux_script_step_outcome, status),
                llvm::ConstantInt::getSigned(i32, -1)
            );
            invalid_builder.CreateRetVoid();

            for (std::size_t await_index{}; await_index < awaits.size(); ++await_index)
            {
                auto* marker = llvm::cast<llvm::CallInst>(value_map[awaits[await_index].call]);
                llvm::StoreInst* result_store{};
                if (!marker->getType()->isVoidTy() && !marker->use_empty())
                {
                    for (auto* user : marker->users())
                    {
                        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                        {
                            result_store = store;
                            break;
                        }
                    }
                    if (result_store == nullptr)
                    {
                        error = "FlowForge async result has no continuation-frame spill";
                        return false;
                    }
                }
                auto* split_before = result_store != nullptr ? result_store->getNextNode() : marker->getNextNode();
                if (split_before == nullptr)
                {
                    error = "FlowForge async marker does not have a continuation block";
                    return false;
                }
                auto* suspend_block = marker->getParent();
                auto* continuation_block = llvm::SplitBlock(suspend_block, split_before);
                suspend_block->getTerminator()->eraseFromParent();

                llvm::SmallVector<llvm::Value*, 8> visible_arguments;
                auto* awaited_result_type = marker->getType();
                for (std::size_t index{1U}; index < marker->arg_size(); ++index)
                    visible_arguments.push_back(marker->getArgOperand(index));
                auto* result_storage = result_store != nullptr ? result_store->getPointerOperand() : nullptr;
                if (result_store != nullptr)
                    result_store->eraseFromParent();
                marker->eraseFromParent();

                llvm::IRBuilder<> suspend_builder(suspend_block);
                auto* scratch =
                    suspend_builder.CreateGEP(i8, frame_argument, llvm::ConstantInt::get(i64, scratch_offset));
                llvm::Value* arguments = llvm::ConstantPointerNull::get(ptr_type);
                const auto* ability_import = awaits[await_index].event_wait
                    ? nullptr
                    : imports[awaits[await_index].import_ordinal].node;
                std::uint64_t scratch_cursor = ability_import == nullptr
                    ? 0U
                    : ability_import->parameters().size() * sizeof(lux_script_value_slot);
                if (!visible_arguments.empty())
                {
                    arguments = scratch;
                    for (std::size_t index{}; index < visible_arguments.size(); ++index)
                    {
                        scratch_cursor = alignFrameOffset(
                            scratch_cursor,
                            ability_import->parameters()[index].value.alignment
                        );
                        auto* storage =
                            suspend_builder.CreateGEP(i8, scratch, llvm::ConstantInt::get(i64, scratch_cursor));
                        scratch_cursor += ability_import->parameters()[index].value.size;
                        suspend_builder.CreateStore(visible_arguments[index], storage);
                        auto* slot = suspend_builder.CreateGEP(
                            i8,
                            arguments,
                            llvm::ConstantInt::get(i64, index * sizeof(lux_script_value_slot))
                        );
                        storeValueSlot(suspend_builder, slot, ability_import->parameters()[index].value, storage);
                    }
                }
                scratch_cursor = alignFrameOffset(scratch_cursor, alignof(lux_script_async_token));
                auto* token = suspend_builder.CreateGEP(i8, scratch, llvm::ConstantInt::get(i64, scratch_cursor));
                const auto host_field = [&](std::size_t offset) {
                    return suspend_builder.CreateGEP(i8, host_argument, llvm::ConstantInt::get(i64, offset));
                };
                auto* host_context =
                    suspend_builder.CreateLoad(ptr_type, host_field(offsetof(lux_script_step_host, context)));
                llvm::Value* status{};
                if (awaits[await_index].event_wait)
                {
                    auto* start_event = suspend_builder.CreateLoad(
                        ptr_type,
                        host_field(offsetof(lux_script_step_host, start_event_wait))
                    );
                    auto* start_type = llvm::FunctionType::get(i32, {ptr_type, i32, ptr_type}, false);
                    status = suspend_builder.CreateCall(
                        start_type,
                        start_event,
                        {host_context, llvm::ConstantInt::get(i32, awaits[await_index].import_ordinal), token}
                    );
                }
                else
                {
                    auto* start_async = suspend_builder.CreateLoad(
                        ptr_type,
                        host_field(offsetof(lux_script_step_host, start_async))
                    );
                    auto* start_type = llvm::FunctionType::get(
                        i32,
                        {ptr_type, i32, ptr_type, i32, ptr_type},
                        false
                    );
                    status = suspend_builder.CreateCall(
                        start_type,
                        start_async,
                        {
                            host_context,
                            llvm::ConstantInt::get(i32, awaits[await_index].import_ordinal),
                            arguments,
                            llvm::ConstantInt::get(i32, visible_arguments.size()),
                            token
                        }
                    );
                }
                const auto next_pc = static_cast<std::uint32_t>(await_index + 1U);
                suspend_builder.CreateStore(llvm::ConstantInt::get(i32, next_pc), frame_argument);
                auto* admitted = suspend_builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, 0));
                storeOutcomeField(
                    suspend_builder,
                    outcome_argument,
                    offsetof(lux_script_step_outcome, state),
                    suspend_builder.CreateSelect(
                        admitted,
                        llvm::ConstantInt::get(i8, LUX_SCRIPT_STEP_SUSPENDED),
                        llvm::ConstantInt::get(i8, LUX_SCRIPT_STEP_FAILED)
                    )
                );
                auto* token_slot = suspend_builder.CreateLoad(i32, token);
                auto* token_generation_address = suspend_builder.CreateGEP(
                    i8,
                    token,
                    llvm::ConstantInt::get(i64, offsetof(lux_script_async_token, generation))
                );
                auto* token_generation = suspend_builder.CreateLoad(i32, token_generation_address);
                storeOutcomeField(
                    suspend_builder,
                    outcome_argument,
                    offsetof(lux_script_step_outcome, waiting_on) + offsetof(lux_script_async_token, slot),
                    suspend_builder.CreateSelect(admitted, token_slot, llvm::ConstantInt::get(i32, 0U))
                );
                storeOutcomeField(
                    suspend_builder,
                    outcome_argument,
                    offsetof(lux_script_step_outcome, waiting_on) + offsetof(lux_script_async_token, generation),
                    suspend_builder.CreateSelect(admitted, token_generation, llvm::ConstantInt::get(i32, 0U))
                );
                storeOutcomeField(
                    suspend_builder,
                    outcome_argument,
                    offsetof(lux_script_step_outcome, status),
                    suspend_builder.CreateSelect(admitted, llvm::ConstantInt::get(i32, 0U), status)
                );
                suspend_builder.CreateRetVoid();

                auto* resume_entry = llvm::BasicBlock::Create(context, "resume." + std::to_string(next_pc), core);
                auto* resume_ready = llvm::BasicBlock::Create(context, "resume.ready." + std::to_string(next_pc), core);
                auto* resume_failed =
                    llvm::BasicBlock::Create(context, "resume.failed." + std::to_string(next_pc), core);
                dispatch_switch->addCase(llvm::ConstantInt::get(i32, next_pc), resume_entry);
                llvm::IRBuilder<> resume_builder(resume_entry);
                auto* resume_state_address = resume_builder.CreateGEP(
                    i8,
                    packet_argument,
                    llvm::ConstantInt::get(i64, offsetof(lux_script_step_resume_packet, state))
                );
                auto* resume_state = resume_builder.CreateLoad(i8, resume_state_address);
                resume_builder.CreateCondBr(
                    resume_builder.CreateICmpEQ(resume_state, llvm::ConstantInt::get(i8, LUX_SCRIPT_RESUME_READY)),
                    resume_ready,
                    resume_failed
                );
                llvm::IRBuilder<> failed_builder(resume_failed);
                auto* resume_status_address = failed_builder.CreateGEP(
                    i8,
                    packet_argument,
                    llvm::ConstantInt::get(i64, offsetof(lux_script_step_resume_packet, status))
                );
                auto* resume_status = failed_builder.CreateLoad(i32, resume_status_address);
                storeOutcomeField(
                    failed_builder,
                    outcome_argument,
                    offsetof(lux_script_step_outcome, state),
                    llvm::ConstantInt::get(i8, LUX_SCRIPT_STEP_FAILED)
                );
                storeOutcomeField(
                    failed_builder,
                    outcome_argument,
                    offsetof(lux_script_step_outcome, status),
                    failed_builder.CreateSelect(
                        failed_builder.CreateICmpNE(resume_status, llvm::ConstantInt::get(i32, 0U)),
                        resume_status,
                        llvm::ConstantInt::getSigned(i32, -1)
                    )
                );
                failed_builder.CreateRetVoid();

                llvm::IRBuilder<> ready_builder(resume_ready);
                if (result_storage != nullptr)
                {
                    auto* value_data_address = ready_builder.CreateGEP(
                        i8,
                        packet_argument,
                        llvm::ConstantInt::get(
                            i64,
                            offsetof(lux_script_step_resume_packet, value) + offsetof(lux_script_value_slot, data)
                        )
                    );
                    auto* value_data = ready_builder.CreateLoad(ptr_type, value_data_address);
                    ready_builder.CreateStore(
                        ready_builder.CreateLoad(awaited_result_type, value_data),
                        result_storage
                    );
                }
                ready_builder.CreateBr(continuation_block);
            }

            for (auto* returned : cloned_returns)
            {
                llvm::IRBuilder<> builder(returned);
                storeOutcomeField(
                    builder,
                    outcome_argument,
                    offsetof(lux_script_step_outcome, state),
                    llvm::ConstantInt::get(i8, LUX_SCRIPT_STEP_COMPLETED)
                );
            }

            for (std::size_t repair{}; repair < 4096U; ++repair)
            {
                llvm::DominatorTree dominators(*core);
                llvm::Instruction* violation{};
                for (auto& block : *core)
                {
                    for (auto& instruction : block)
                    {
                        if (instruction.getType()->isVoidTy() || llvm::isa<llvm::AllocaInst>(instruction) ||
                            instruction.isTerminator())
                        {
                            continue;
                        }
                        for (const auto& use : instruction.uses())
                        {
                            const auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
                            if (user != nullptr && !dominators.dominates(&instruction, user))
                            {
                                violation = &instruction;
                                break;
                            }
                        }
                        if (violation != nullptr)
                            break;
                    }
                    if (violation != nullptr)
                        break;
                }
                if (violation == nullptr)
                    break;
                llvm::DemoteRegToStack(*violation, false, &*dispatch->getFirstInsertionPt());
                if (repair == 4095U)
                {
                    error = "FlowForge coroutine dominance repair exceeded its bounded pass count";
                    return false;
                }
            }

            std::vector<llvm::AllocaInst*> repair_allocations;
            for (auto& block : *core)
            {
                for (auto& instruction : block)
                {
                    if (auto* allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction))
                        repair_allocations.push_back(allocation);
                }
            }
            for (auto* allocation : repair_allocations)
            {
                const auto* count = llvm::dyn_cast<llvm::ConstantInt>(allocation->getArraySize());
                if (count == nullptr)
                {
                    error = "FlowForge dominance spill contains a dynamic allocation";
                    return false;
                }
                const auto alignment = layout.getABITypeAlign(allocation->getAllocatedType()).value();
                frame_size = alignFrameOffset(frame_size, alignment);
                const auto offset = frame_size;
                const auto size = layout.getTypeAllocSize(allocation->getAllocatedType()) * count->getZExtValue();
                frame_size += size;
                frame_alignment = (std::max)(frame_alignment, static_cast<std::uint64_t>(alignment));
                slots.push_back({nullptr, offset, size, alignment});
                llvm::SmallVector<llvm::Use*, 16> uses;
                for (auto& use : allocation->uses())
                    uses.push_back(std::addressof(use));
                for (auto* use : uses)
                {
                    auto* user = llvm::cast<llvm::Instruction>(use->getUser());
                    llvm::IRBuilder<> builder(user);
                    use->set(builder.CreateGEP(i8, frame_argument, llvm::ConstantInt::get(i64, offset)));
                }
                allocation->eraseFromParent();
            }
            frame_size = alignFrameOffset(frame_size, frame_alignment);

            auto* start_type = llvm::FunctionType::get(i32, {ptr_type, ptr_type, ptr_type, ptr_type}, false);
            result.start = llvm::Function::Create(
                start_type,
                llvm::GlobalValue::InternalLinkage,
                "lux_step_start_" + event.symbol,
                module
            );
            result.start->setDSOLocal(true);
            auto* start_block = llvm::BasicBlock::Create(context, "entry", result.start);
            llvm::IRBuilder<> start_builder(start_block);
            auto* call_frame = result.start->getArg(0);
            auto* start_host = result.start->getArg(1);
            auto* start_frame = result.start->getArg(2);
            auto* start_outcome = result.start->getArg(3);
            start_builder.CreateStore(llvm::ConstantInt::get(i32, 0U), start_frame);
            const auto start_field = [&](llvm::Value* base, std::size_t offset) {
                return start_builder.CreateGEP(i8, base, llvm::ConstantInt::get(i64, offset));
            };
            auto* native_instance = start_builder.CreateLoad(
                ptr_type,
                start_field(call_frame, offsetof(lux_script_call_frame, native_instance))
            );
            llvm::SmallVector<llvm::Value*, 12> start_core_arguments;
            auto* state = start_builder.CreateLoad(
                ptr_type,
                start_field(native_instance, offsetof(lux_script_native_instance_context, state))
            );
            auto* abilities = start_builder.CreateLoad(
                ptr_type,
                start_field(native_instance, offsetof(lux_script_native_instance_context, abilities))
            );
            start_core_arguments.push_back(state);
            start_core_arguments.push_back(abilities);
            if (target->arg_size() > 2U)
            {
                auto* argument_slots =
                    start_builder.CreateLoad(ptr_type, start_field(call_frame, offsetof(lux_script_call_frame, args)));
                for (std::size_t index{2U}; index < target->arg_size(); ++index)
                {
                    auto* slot = start_builder.CreateGEP(
                        i8,
                        argument_slots,
                        llvm::ConstantInt::get(i64, (index - 2U) * sizeof(lux_script_value_slot))
                    );
                    auto* data =
                        start_builder.CreateLoad(ptr_type, start_field(slot, offsetof(lux_script_value_slot, data)));
                    start_core_arguments.push_back(
                        start_builder.CreateLoad(target->getFunctionType()->getParamType(index), data)
                    );
                }
            }
            for (std::size_t index{}; index < start_core_arguments.size(); ++index)
            {
                auto* address =
                    start_builder.CreateGEP(i8, start_frame, llvm::ConstantInt::get(i64, argument_offsets[index]));
                start_builder.CreateStore(start_core_arguments[index], address);
            }
            start_core_arguments.insert(
                start_core_arguments.end(),
                {start_frame, start_host, llvm::ConstantPointerNull::get(ptr_type), start_outcome}
            );
            start_builder.CreateCall(core, start_core_arguments);
            start_builder.CreateRet(llvm::ConstantInt::get(i32, 0U));

            auto* resume_type = llvm::FunctionType::get(i32, {ptr_type, ptr_type, ptr_type, ptr_type}, false);
            result.resume = llvm::Function::Create(
                resume_type,
                llvm::GlobalValue::InternalLinkage,
                "lux_step_resume_" + event.symbol,
                module
            );
            result.resume->setDSOLocal(true);
            auto* resume_block = llvm::BasicBlock::Create(context, "entry", result.resume);
            llvm::IRBuilder<> resume_wrapper(resume_block);
            auto* resume_host = result.resume->getArg(0);
            auto* resume_frame = result.resume->getArg(1);
            auto* resume_packet = result.resume->getArg(2);
            auto* resume_outcome = result.resume->getArg(3);
            llvm::SmallVector<llvm::Value*, 12> resume_core_arguments;
            for (std::size_t index{}; index < target->arg_size(); ++index)
            {
                auto* address =
                    resume_wrapper.CreateGEP(i8, resume_frame, llvm::ConstantInt::get(i64, argument_offsets[index]));
                resume_core_arguments.push_back(
                    resume_wrapper.CreateLoad(target->getFunctionType()->getParamType(index), address)
                );
            }
            resume_core_arguments.insert(
                resume_core_arguments.end(),
                {resume_frame, resume_host, resume_packet, resume_outcome}
            );
            resume_wrapper.CreateCall(core, resume_core_arguments);
            resume_wrapper.CreateRet(llvm::ConstantInt::get(i32, 0U));

            auto* destroy_type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr_type}, false);
            result.destroy = llvm::Function::Create(
                destroy_type,
                llvm::GlobalValue::InternalLinkage,
                "lux_step_destroy_" + event.symbol,
                module
            );
            result.destroy->setDSOLocal(true);
            auto* destroy_block = llvm::BasicBlock::Create(context, "entry", result.destroy);
            llvm::IRBuilder<>(destroy_block).CreateRetVoid();

            if (frame_size == 0U || frame_size > (std::numeric_limits<std::uint32_t>::max)() ||
                frame_alignment > (std::numeric_limits<std::uint32_t>::max)())
            {
                error = "FlowForge continuation frame layout exceeds the native ABI after liveness spilling";
                return false;
            }
            result.frame_size = static_cast<std::uint32_t>(frame_size);
            result.frame_align = static_cast<std::uint32_t>(frame_alignment);
            std::uint64_t frame_hash{14695981039346656037ULL};
            frame_hash = appendFrameHash(frame_hash, result.frame_size);
            frame_hash = appendFrameHash(frame_hash, result.frame_align);
            for (std::size_t index{}; index < argument_offsets.size(); ++index)
            {
                frame_hash = appendFrameHash(frame_hash, argument_offsets[index]);
                auto* type = target->getFunctionType()->getParamType(index);
                frame_hash = appendFrameHash(frame_hash, layout.getTypeAllocSize(type).getFixedValue());
                frame_hash = appendFrameHash(frame_hash, layout.getABITypeAlign(type).value());
            }
            for (const auto& slot : slots)
            {
                frame_hash = appendFrameHash(frame_hash, slot.offset);
                frame_hash = appendFrameHash(frame_hash, slot.size);
                frame_hash = appendFrameHash(frame_hash, slot.alignment);
            }
            for (const auto& await : awaits)
            {
                frame_hash = appendFrameHash(frame_hash, await.node_id);
                if (await.event_wait)
                {
                    const auto& source = event_imports[await.import_ordinal].node->source();
                    frame_hash = appendFrameHash(frame_hash, source.system_id);
                    frame_hash = appendFrameHash(frame_hash, source.event_id);
                    frame_hash = appendFrameHash(frame_hash, static_cast<std::uint8_t>(source.route));
                    frame_hash = appendFrameHash(frame_hash, source.payload.type_id);
                    frame_hash = appendFrameHash(frame_hash, source.payload.size);
                    frame_hash = appendFrameHash(frame_hash, source.payload.alignment);
                    continue;
                }
                const auto& import = *imports[await.import_ordinal].node;
                for (const auto& parameter : import.parameters())
                {
                    frame_hash = appendFrameHash(frame_hash, parameter.value.type_id);
                    frame_hash = appendFrameHash(frame_hash, parameter.value.size);
                    frame_hash = appendFrameHash(frame_hash, parameter.value.alignment);
                    frame_hash = appendFrameHash(frame_hash, static_cast<std::uint8_t>(parameter.value.lifetime));
                }
                for (const auto& value : import.results())
                {
                    frame_hash = appendFrameHash(frame_hash, value.type_id);
                    frame_hash = appendFrameHash(frame_hash, value.size);
                    frame_hash = appendFrameHash(frame_hash, value.alignment);
                    frame_hash = appendFrameHash(frame_hash, static_cast<std::uint8_t>(value.lifetime));
                }
            }
            result.frame_hash = frame_hash == 0U ? 1U : frame_hash;

            target->deleteBody();
            auto* stub = llvm::BasicBlock::Create(context, "entry", target);
            llvm::IRBuilder<>(stub).CreateRetVoid();
            return true;
        }

        bool inlineGraphFunctions(llvm::Module& module, const std::vector<EventInfo>& events, std::string& error)
        {
            for (const auto& event : events)
            {
                auto* root = module.getFunction(event.symbol);
                if (root == nullptr)
                {
                    error = "FlowForge export function is missing before coroutine inlining";
                    return false;
                }
                for (std::size_t pass{}; pass < 1024U; ++pass)
                {
                    llvm::CallBase* candidate{};
                    for (auto& block : *root)
                    {
                        for (auto& instruction : block)
                        {
                            auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                            auto* callee = call != nullptr ? call->getCalledFunction() : nullptr;
                            if (callee != nullptr && !callee->isDeclaration() && callee != root)
                            {
                                candidate = call;
                                break;
                            }
                        }
                        if (candidate != nullptr)
                            break;
                    }
                    if (candidate == nullptr)
                        break;
                    llvm::InlineFunctionInfo information;
                    const auto inlined = llvm::InlineFunction(*candidate, information);
                    if (!inlined.isSuccess())
                    {
                        error = "FlowForge async graph function cannot be inlined into its export";
                        return false;
                    }
                    if (pass == 1023U)
                    {
                        error = "FlowForge async graph function recursion is not supported";
                        return false;
                    }
                }
            }
            return true;
        }

        bool lowerAsyncEvents(
            llvm::Module& module,
            const std::vector<EventInfo>& events,
            const std::vector<AbilityImportInfo>& imports,
            const std::vector<EventWaitImportInfo>& event_imports,
            const SuspensionAnalysis& suspension_analysis,
            std::vector<NativeStepInfo>& steps,
            std::string& error
        )
        {
            if (!inlineGraphFunctions(module, events, error))
                return false;
            steps.resize(events.size());
            for (std::size_t index{}; index < events.size(); ++index)
            {
                if (!lowerEventToStateMachine(
                        module,
                        events[index],
                        imports,
                        event_imports,
                        steps[index],
                        error
                    ))
                    return false;
                const bool expected_suspension =
                    suspension_analysis.firstSuspensionFrom(events[index].node->execOutPin()) != nullptr;
                const bool lowered_suspension = steps[index].start != nullptr;
                if (expected_suspension != lowered_suspension)
                {
                    error = "FlowForge suspension analysis does not match the lowered async markers";
                    return false;
                }
            }
            std::unordered_set<std::string> exported_symbols;
            for (const auto& event : events)
                exported_symbols.insert(event.symbol);
            for (auto iterator = module.begin(); iterator != module.end();)
            {
                auto& function = *iterator++;
                const bool is_generated_step = function.getName().starts_with("lux_step_");
                const bool is_export = exported_symbols.contains(function.getName().str());
                if (!function.isDeclaration() && function.use_empty() && !is_generated_step && !is_export)
                {
                    function.eraseFromParent();
                }
            }
            for (auto iterator = module.begin(); iterator != module.end();)
            {
                auto& function = *iterator++;
                const bool is_suspension_marker = function.getName().starts_with("lux_ff_ability_async_") ||
                    function.getName().starts_with("lux_ff_event_wait_");
                if (function.isDeclaration() && function.use_empty() && is_suspension_marker)
                {
                    function.eraseFromParent();
                }
            }
            return true;
        }

        // ---- step 2 + 3: import slots + bind_host --------------------------
        bool rewriteImportsToSlots(llvm::Module& m, std::vector<std::string>& imports_out, std::string& err)
        {
            auto& ctx = m.getContext();
            auto* ptr_ty = llvm::PointerType::get(ctx, 0);

            llvm::SmallVector<llvm::Function*, 16> externs;
            for (llvm::Function& f : m.functions())
                if (f.isDeclaration() && !f.isIntrinsic())
                    externs.push_back(&f);
            if (externs.empty())
                return true;

            struct Slot
            {
                std::string name;
                llvm::GlobalVariable* g;
            };
            std::vector<Slot> slots;
            slots.reserve(externs.size());

            for (llvm::Function* f : externs)
            {
                const std::string name = f->getName().str();
                auto* slot = new llvm::GlobalVariable(
                    m,
                    ptr_ty,
                    /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantPointerNull::get(ptr_ty),
                    "_lfimp_" + name
                );

                for (llvm::User* u : llvm::make_early_inc_range(f->users()))
                {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(u);
                    if (!call || call->getCalledOperand() != f)
                    {
                        err = "import '" + name +
                            "' is referenced by a "
                            "non-call use (address taken?) — cannot slot it";
                        return false;
                    }
                    llvm::IRBuilder<> b(call);
                    auto* fp = b.CreateLoad(ptr_ty, slot, name + ".fp");
                    call->setCalledOperand(fp);
                }
                if (!f->use_empty())
                {
                    err = "import '" + name + "' still has uses after rewrite";
                    return false;
                }
                f->eraseFromParent();
                slots.push_back(Slot{name, slot});
                imports_out.push_back(name);
            }

            // int lux_script_bind_host(lux_host_resolve_fn, void*, uint32_t)
            auto* i32 = llvm::Type::getInt32Ty(ctx);
            auto* bind_ft = llvm::FunctionType::get(i32, {ptr_ty, ptr_ty, i32}, /*vararg=*/false);
            auto* bind_fn =
                llvm::Function::Create(bind_ft, llvm::GlobalValue::ExternalLinkage, LUX_SCRIPT_BIND_HOST_ENTRY, m);

            auto* entry = llvm::BasicBlock::Create(ctx, "entry", bind_fn);
            llvm::IRBuilder<> b(entry);
            llvm::Value* resolve = bind_fn->getArg(0);
            llvm::Value* host_ctx = bind_fn->getArg(1);
            auto* resolve_ft = llvm::FunctionType::get(ptr_ty, {ptr_ty, ptr_ty}, /*vararg=*/false);

            llvm::Value* missing = llvm::ConstantInt::get(i32, 0);
            for (const Slot& s : slots)
            {
                auto* name_c = makeCStr(m, s.name, "_lfimp_name");
                auto* addr = b.CreateCall(resolve_ft, resolve, {host_ctx, name_c});
                b.CreateStore(addr, s.g);
                auto* isnull = b.CreateICmpEQ(addr, llvm::ConstantPointerNull::get(ptr_ty));
                missing = b.CreateAdd(missing, b.CreateZExt(isnull, i32));
            }
            b.CreateRet(missing);
            return true;
        }

        // ---- step 4: one call_frame wrapper per event ----------------------
        llvm::Function* emitEventWrapper(llvm::Module& m, const EventInfo& ev, std::string& err)
        {
            auto& ctx = m.getContext();
            auto* ptr_ty = llvm::PointerType::get(ctx, 0);
            auto* i32 = llvm::Type::getInt32Ty(ctx);
            auto* i64 = llvm::Type::getInt64Ty(ctx);

            llvm::Function* target = m.getFunction(ev.symbol);
            if (!target)
            {
                err = "event function '" + ev.symbol + "' missing in module";
                return nullptr;
            }
            llvm::FunctionType* tft = target->getFunctionType();
            // Leading params are instance state + prepared Ability runtime; payload follows.
            const size_t payload_count = tft->getNumParams() - 2;

            auto* wrap_ft = llvm::FunctionType::get(i32, {ptr_ty}, false);
            auto* wrap =
                llvm::Function::Create(wrap_ft, llvm::GlobalValue::InternalLinkage, "lux_fnwrap_" + ev.symbol, m);

            auto* entry = llvm::BasicBlock::Create(ctx, "entry", wrap);
            llvm::IRBuilder<> b(entry);
            llvm::Value* frame = wrap->getArg(0);

            auto gepByte = [&](llvm::Value* base, uint64_t off) {
                return b.CreateGEP(b.getInt8Ty(), base, llvm::ConstantInt::get(i64, off));
            };

            llvm::Value* native_instance = b.CreateLoad(
                ptr_ty,
                gepByte(frame, offsetof(lux_script_call_frame, native_instance)),
                "native.instance"
            );
            llvm::Value* state = b.CreateLoad(
                ptr_ty,
                gepByte(native_instance, offsetof(lux_script_native_instance_context, state)),
                "state"
            );
            llvm::Value* abilities = b.CreateLoad(
                ptr_ty,
                gepByte(native_instance, offsetof(lux_script_native_instance_context, abilities)),
                "abilities"
            );

            llvm::SmallVector<llvm::Value*, 8> call_args;
            call_args.push_back(state);
            call_args.push_back(abilities);
            if (payload_count > 0)
            {
                llvm::Value* args_base =
                    b.CreateLoad(ptr_ty, gepByte(frame, offsetof(lux_script_call_frame, args)), "args");
                for (size_t i = 0; i < payload_count; ++i)
                {
                    llvm::Value* slot = gepByte(args_base, i * sizeof(lux_script_value_slot));
                    llvm::Value* data = b.CreateLoad(ptr_ty, gepByte(slot, offsetof(lux_script_value_slot, data)));
                    // args[i].data points AT the value's storage; load it
                    // with the event function's own parameter type.
                    call_args.push_back(b.CreateLoad(tft->getParamType(static_cast<unsigned>(i + 2)), data));
                }
            }

            b.CreateCall(target, call_args);
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
            return wrap;
        }

        // ---- step 5: descriptor globals + lux_script_get_module ------------
        bool emitModuleDesc(
            llvm::Module& m,
            const std::string& module_name,
            const std::vector<EventInfo>& events,
            const std::vector<AbilityImportInfo>& ability_imports,
            const std::vector<EventWaitImportInfo>& event_imports,
            const std::vector<NativeStepInfo>& steps,
            const std::vector<lux::rdesc::ScriptFunction>& exports,
            const std::vector<llvm::Function*>& wrappers,
            uint64_t state_layout_hash,
            uint32_t state_size,
            uint32_t state_align,
            std::string& err
        )
        {
            auto& ctx = m.getContext();
            const llvm::DataLayout& dl = m.getDataLayout();
            auto* ptr_ty = llvm::PointerType::get(ctx, 0);
            auto* i8 = llvm::Type::getInt8Ty(ctx);
            auto* i32 = llvm::Type::getInt32Ty(ctx);
            auto* i64 = llvm::Type::getInt64Ty(ctx);
            auto* pad3 = llvm::ArrayType::get(i8, 3);
            auto* pad6 = llvm::ArrayType::get(i8, 6);

            // Mirrors of the C structs. checkLayout guards against any
            // DataLayout divergence from the host compiler's layout.
            auto* type_desc_ty =
                llvm::StructType::create(ctx, {ptr_ty, i64, i32, i32, i8, i8, pad6}, "lux_script_type_desc");
            auto* func_desc_ty = llvm::StructType::create(
                ctx,
                {ptr_ty, i64, ptr_ty, i32, ptr_ty, i32, ptr_ty, ptr_ty},
                "lux_script_function_desc"
            );
            auto* ability_import_ty = llvm::StructType::create(
                ctx,
                {ptr_ty, i64, ptr_ty, i64, i64, i32, i8, pad3, ptr_ty, i32, ptr_ty, i32},
                "lux_script_ability_import_desc"
            );
            auto* event_wait_import_ty = llvm::StructType::create(
                ctx,
                {i64, i64, i64, i32, i8, pad3, type_desc_ty},
                "lux_script_event_wait_import_desc"
            );
            auto* step_desc_ty =
                llvm::StructType::create(ctx, {i32, i32, i64, ptr_ty, ptr_ty, ptr_ty}, "lux_script_step_desc");
            auto* module_desc_ty = llvm::StructType::create(
                ctx,
                {
                    ptr_ty,
                    i32,
                    i32,
                    i64,
                    i32,
                    i32,
                    ptr_ty,
                    i32,
                    i32,
                    ptr_ty,
                    i32,
                    i32,
                    ptr_ty,
                    i32,
                    i32
                },
                "lux_script_module_desc"
            );

            const auto checkLayout = [&](llvm::StructType* t, size_t c_size, const char* what) {
                if (dl.getTypeAllocSize(t) != c_size)
                {
                    err = std::string("ABI layout mismatch for ") + what;
                    return false;
                }
                return true;
            };
            if (!checkLayout(type_desc_ty, sizeof(lux_script_type_desc), "type_desc") ||
                !checkLayout(func_desc_ty, sizeof(lux_script_function_desc), "function_desc") ||
                !checkLayout(ability_import_ty, sizeof(lux_script_ability_import_desc), "ability_import_desc") ||
                !checkLayout(
                    event_wait_import_ty,
                    sizeof(lux_script_event_wait_import_desc),
                    "event_wait_import_desc"
                ) ||
                !checkLayout(step_desc_ty, sizeof(lux_script_step_desc), "step_desc") ||
                !checkLayout(module_desc_ty, sizeof(lux_script_module_desc), "module_desc"))
                return false;

            auto* null_ptr = llvm::ConstantPointerNull::get(ptr_ty);
            auto* pad_zero = llvm::ConstantAggregateZero::get(pad6);
            auto* pad3_zero = llvm::ConstantAggregateZero::get(pad3);

            const auto emitTypeArray = [&](std::span<const lux::script::ScriptAbilityValueDescription> values,
                                           const llvm::Twine& name) -> llvm::Constant* {
                if (values.empty())
                    return null_ptr;
                llvm::SmallVector<llvm::Constant*, 8> descriptions;
                for (const auto& value : values)
                {
                    descriptions.push_back(llvm::ConstantStruct::get(
                        type_desc_ty,
                        {makeCStr(m, value.canonical_name, "_lfd_ability_type"),
                         llvm::ConstantInt::get(i64, value.type_id),
                         llvm::ConstantInt::get(i32, value.size),
                         llvm::ConstantInt::get(i32, value.alignment),
                         llvm::ConstantInt::get(i8, value.abi_kind),
                         llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(value.pass)),
                         pad_zero}
                    ));
                }
                auto* array_type = llvm::ArrayType::get(type_desc_ty, descriptions.size());
                return new llvm::GlobalVariable(
                    m,
                    array_type,
                    true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(array_type, descriptions),
                    name
                );
            };

            llvm::SmallVector<llvm::Constant*, 8> fn_descs;
            for (size_t e = 0; e < events.size(); ++e)
            {
                const auto& function = exports[e];
                const auto& params = function.args;

                // Per-event argument type_desc array (may be empty).
                llvm::Constant* args_ptr = null_ptr;
                if (!params.empty())
                {
                    llvm::SmallVector<llvm::Constant*, 8> arg_descs;
                    for (const auto& parameter : params)
                    {
                        arg_descs.push_back(llvm::ConstantStruct::get(
                            type_desc_ty,
                            {
                                makeCStr(m, parameter.canonical_name, "_lfd_argname"),
                                llvm::ConstantInt::get(i64, parameter.type_id),
                                llvm::ConstantInt::get(i32, parameter.size),
                                llvm::ConstantInt::get(i32, parameter.alignment),
                                llvm::ConstantInt::get(i8, parameter.abi_kind),
                                llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(parameter.pass)),
                                pad_zero,
                            }
                        ));
                    }
                    auto* arr_ty = llvm::ArrayType::get(type_desc_ty, arg_descs.size());
                    args_ptr = new llvm::GlobalVariable(
                        m,
                        arr_ty,
                        /*isConstant=*/true,
                        llvm::GlobalValue::PrivateLinkage,
                        llvm::ConstantArray::get(arr_ty, arg_descs),
                        "_lfd_args"
                    );
                }

                llvm::Constant* step_pointer = null_ptr;
                if (e < steps.size() && steps[e].start != nullptr)
                {
                    step_pointer = new llvm::GlobalVariable(
                        m,
                        step_desc_ty,
                        true,
                        llvm::GlobalValue::PrivateLinkage,
                        llvm::ConstantStruct::get(
                            step_desc_ty,
                            {llvm::ConstantInt::get(i32, steps[e].frame_size),
                             llvm::ConstantInt::get(i32, steps[e].frame_align),
                             llvm::ConstantInt::get(i64, steps[e].frame_hash),
                             steps[e].start,
                             steps[e].resume,
                             steps[e].destroy}
                        ),
                        "_lfd_step"
                    );
                }
                fn_descs.push_back(llvm::ConstantStruct::get(
                    func_desc_ty,
                    {
                        makeCStr(m, function.name, "_lfd_fnname"),
                        llvm::ConstantInt::get(i64, events[e].authored_symbol),
                        args_ptr,
                        llvm::ConstantInt::get(i32, static_cast<uint32_t>(params.size())),
                        null_ptr, // no returns today
                        llvm::ConstantInt::get(i32, 0),
                        wrappers[e],
                        step_pointer,
                    }
                ));
            }

            llvm::SmallVector<llvm::Constant*, 8> ability_descriptions;
            for (const auto& import : ability_imports)
            {
                const auto& node = *import.node;
                std::vector<lux::script::ScriptAbilityValueDescription> parameter_values;
                parameter_values.reserve(node.parameters().size());
                for (const auto& parameter : node.parameters())
                    parameter_values.push_back(parameter.value);
                auto* args = emitTypeArray(parameter_values, "_lfd_ability_args");
                auto* results = emitTypeArray(node.results(), "_lfd_ability_results");
                ability_descriptions.push_back(llvm::ConstantStruct::get(
                    ability_import_ty,
                    {makeCStr(m, node.contract().name(), "_lfd_ability_contract"),
                     llvm::ConstantInt::get(i64, node.contract().hash()),
                     makeCStr(m, node.method().name(), "_lfd_ability_method"),
                     llvm::ConstantInt::get(i64, node.method().hash()),
                     llvm::ConstantInt::get(i64, node.expectedSchemaHash()),
                     llvm::ConstantInt::get(i32, node.expectedSchemaVersion()),
                     llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(node.methodKind())),
                     pad3_zero,
                     args,
                     llvm::ConstantInt::get(i32, node.parameters().size()),
                     results,
                     llvm::ConstantInt::get(i32, node.results().size())}
                ));
            }
            llvm::Constant* ability_imports_ptr = null_ptr;
            if (!ability_descriptions.empty())
            {
                auto* array_type = llvm::ArrayType::get(ability_import_ty, ability_descriptions.size());
                ability_imports_ptr = new llvm::GlobalVariable(
                    m,
                    array_type,
                    true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(array_type, ability_descriptions),
                    "_lfd_ability_imports"
                );
            }

            llvm::SmallVector<llvm::Constant*, 8> event_wait_descriptions;
            for (const auto& import : event_imports)
            {
                const auto& source = import.node->source();
                const auto& payload = source.payload;
                auto* payload_description = llvm::ConstantStruct::get(
                    type_desc_ty,
                    {
                        makeCStr(m, payload.canonical_name, "_lfd_event_payload"),
                        llvm::ConstantInt::get(i64, payload.type_id),
                        llvm::ConstantInt::get(i32, payload.size),
                        llvm::ConstantInt::get(i32, payload.alignment),
                        llvm::ConstantInt::get(i8, payload.abi_kind),
                        llvm::ConstantInt::get(i8, LUX_SCRIPT_PASS_VALUE),
                        pad_zero
                    }
                );
                event_wait_descriptions.push_back(llvm::ConstantStruct::get(
                    event_wait_import_ty,
                    {
                        llvm::ConstantInt::get(i64, source.system_id),
                        llvm::ConstantInt::get(i64, source.event_id),
                        llvm::ConstantInt::get(i64, source.payload_schema_hash),
                        llvm::ConstantInt::get(i32, source.payload_schema_version),
                        llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(source.route)),
                        pad3_zero,
                        payload_description
                    }
                ));
            }
            llvm::Constant* event_wait_imports_ptr = null_ptr;
            if (!event_wait_descriptions.empty())
            {
                auto* array_type = llvm::ArrayType::get(event_wait_import_ty, event_wait_descriptions.size());
                event_wait_imports_ptr = new llvm::GlobalVariable(
                    m,
                    array_type,
                    true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(array_type, event_wait_descriptions),
                    "_lfd_event_wait_imports"
                );
            }

            llvm::Constant* fns_ptr = null_ptr;
            if (!fn_descs.empty())
            {
                auto* fns_ty = llvm::ArrayType::get(func_desc_ty, fn_descs.size());
                fns_ptr = new llvm::GlobalVariable(
                    m,
                    fns_ty,
                    /*isConstant=*/true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(fns_ty, fn_descs),
                    "_lfd_functions"
                );
            }

            auto* desc = new llvm::GlobalVariable(
                m,
                module_desc_ty,
                /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage,
                llvm::ConstantStruct::get(
                    module_desc_ty,
                    {
                        makeCStr(m, module_name, "_lfd_modname"),
                        llvm::ConstantInt::get(i32, LUX_SCRIPT_ABI_VERSION),
                        llvm::ConstantInt::get(i32, 0),
                        llvm::ConstantInt::get(i64, state_layout_hash),
                        llvm::ConstantInt::get(i32, state_size),
                        llvm::ConstantInt::get(i32, state_align),
                        fns_ptr,
                        llvm::ConstantInt::get(i32, static_cast<uint32_t>(fn_descs.size())),
                        llvm::ConstantInt::get(i32, 0),
                        ability_imports_ptr,
                        llvm::ConstantInt::get(i32, ability_descriptions.size()),
                        llvm::ConstantInt::get(i32, 0),
                        event_wait_imports_ptr,
                        llvm::ConstantInt::get(i32, event_wait_descriptions.size()),
                        llvm::ConstantInt::get(i32, 0)
                    }
                ),
                "_lfd_module"
            );

            // const lux_script_module_desc* lux_script_get_module(void)
            auto* get_ft = llvm::FunctionType::get(ptr_ty, {}, false);
            auto* get_fn =
                llvm::Function::Create(get_ft, llvm::GlobalValue::ExternalLinkage, LUX_SCRIPT_MODULE_ENTRY, m);
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", get_fn);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(desc);
            return true;
        }

        std::string findLinker(const FlowForgeCompileOptions& options)
        {
            if (!options.linker.empty())
                return options.linker.string();
            if (const char* env = std::getenv("LUX_FLOWFORGE_LINKER"); env && *env)
                return env;
#ifdef LUX_FLOWFORGE_LLD_LINK
            if (llvm::sys::fs::exists(LUX_FLOWFORGE_LLD_LINK))
                return LUX_FLOWFORGE_LLD_LINK;
#endif
            if (auto p = llvm::sys::findProgramByName("lld-link"))
                return *p;
            if (auto p = llvm::sys::findProgramByName("link"))
                return *p;
            return {};
        }
    } // anonymous namespace

    static bool compileToObjectImpl(
        IRContext& ctx,
        const FlowGraph& graph,
        const FlowForgeCompileOptions& options,
        const SuspensionAnalysis& suspension_analysis,
        AotArtifact& artifact_out,
        std::string* error_out
    )
    {
        const auto fail = [&](std::string msg) {
            if (error_out)
                *error_out = std::move(msg);
            return false;
        };

        artifact_out = AotArtifact{};
        artifact_out.module_name = options.module_name;

        if (!validFlowForgeExports(graph))
        {
            return fail("invalid FlowGraph export declarations");
        }

        std::vector<EventInfo> events;
        const auto ability_imports = collectAbilityImports(graph);
        const auto event_wait_imports = collectEventWaitImports(graph);
        events.reserve(graph.exports().size());
        artifact_out.exports.reserve(graph.exports().size());
        for (const auto& exported : graph.exports())
        {
            const auto* event = static_cast<const OnEventNode*>(graph.findNodeById(exported.entry_node_id));
            lux::rdesc::ScriptFunction function;
            function.name = std::string(event->name());
            function.symbol_id = exported.symbol;
            function.args.reserve(event->paramInfos().size());
            for (const auto& parameter : event->paramInfos())
            {
                if (parameter.type == nullptr)
                {
                    return fail("export parameter has no semantic type");
                }
                auto projected = projectType(*parameter.type);
                if (!projected)
                {
                    return fail("export parameter cannot be projected to the Script ABI");
                }
                function.args.push_back(std::move(*projected));
            }
            events.push_back(EventInfo{event, FlowScriptInstance::eventSymbol(event->name()), exported.symbol});
            artifact_out.exports.push_back(std::move(function));
        }

        // 1. The exact JIT lowering pipeline, then LLVM IR translation.
        auto builder = MLIRBuilder::create(ctx);
        if (!builder)
            return fail("MLIR builder creation failed");
        auto built = builder->generateIR(graph);
        if (!built)
            return fail("compile failed: " + built.error().message);
        std::unique_ptr<IR> ir = std::move(built.value());
        auto lowered = lowerToLLVM(*ir);
        if (!lowered)
            return fail("compile failed: " + lowered.error().message);

        artifact_out.state_size = ir->impl().state_size;
        artifact_out.state_hash = ir->impl().state_hash;
        artifact_out.state_align = ir->impl().state_align;
        artifact_out.state_defaults = ir->impl().state_defaults;

        mlir::ModuleOp module = ir->impl().top_module.get();
        auto* mlir_ctx = module.getContext();
        mlir::registerBuiltinDialectTranslation(*mlir_ctx);
        mlir::registerLLVMDialectTranslation(*mlir_ctx);

        llvm::LLVMContext llctx;
        auto llmod = mlir::translateModuleToLLVMIR(module, llctx, options.module_name);
        if (!llmod)
            return fail("MLIR -> LLVM IR translation failed");

        // Target setup first: emitModuleDesc validates struct layouts
        // against the DataLayout.
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        const std::string triple_str = llvm::sys::getDefaultTargetTriple();
        std::string lookup_err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple_str, lookup_err);
        if (!target)
            return fail("no LLVM target for '" + triple_str + "': " + lookup_err);
        std::unique_ptr<llvm::TargetMachine> tm(
            target->createTargetMachine(triple_str, "generic", "", llvm::TargetOptions{}, llvm::Reloc::PIC_)
        );
        if (!tm)
            return fail("createTargetMachine failed");
        llmod->setTargetTriple(triple_str);
        llmod->setDataLayout(tm->createDataLayout());
        const llvm::Triple triple(triple_str);

        std::vector<NativeStepInfo> steps;
        {
            std::string err;
            if (!lowerAsyncEvents(
                    *llmod,
                    events,
                    ability_imports,
                    event_wait_imports,
                    suspension_analysis,
                    steps,
                    err
                ))
                return fail(std::move(err));
        }

        // Materialize per-instance Ability imports before generic module-global host import binding.
        {
            std::string err;
            if (!materializeSynchronousAbilityCalls(*llmod, ability_imports, err))
                return fail(std::move(err));
        }

        // 2 + 3. Imports -> slots + bind_host.
        {
            std::string err;
            if (!rewriteImportsToSlots(*llmod, artifact_out.imports, err))
                return fail(std::move(err));
            if (llvm::Function* bind = llmod->getFunction(LUX_SCRIPT_BIND_HOST_ENTRY))
                exportSymbol(bind, triple);
        }

        // 4. Event wrappers.
        std::vector<llvm::Function*> wrappers;
        wrappers.reserve(events.size());
        for (const EventInfo& ev : events)
        {
            std::string err;
            llvm::Function* w = emitEventWrapper(*llmod, ev, err);
            if (!w)
                return fail(std::move(err));
            wrappers.push_back(w);
        }

        // 5. Module descriptor + entry.
        {
            std::string err;
            if (!emitModuleDesc(
                    *llmod,
                    options.module_name,
                    events,
                    ability_imports,
                    event_wait_imports,
                    steps,
                    artifact_out.exports,
                    wrappers,
                    artifact_out.state_hash,
                    static_cast<uint32_t>(artifact_out.state_size),
                    artifact_out.state_align,
                    err
                ))
                return fail(std::move(err));
            exportSymbol(llmod->getFunction(LUX_SCRIPT_MODULE_ENTRY), triple);
        }

        // 5b. CRT-free floating point on MSVC targets: any float use makes
        // the compiler reference `_fltused` (a marker the CRT normally
        // defines). The artifact links with NO CRT, so define it ourselves —
        // exactly what /NODEFAULTLIB binaries do. Int-only graphs never
        // reference it and the unused global is dropped by the linker.
        if (triple.isOSWindows() && !llmod->getNamedGlobal("_fltused"))
        {
            auto* i32 = llvm::Type::getInt32Ty(llctx);
            new llvm::GlobalVariable(
                *llmod,
                i32,
                /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage,
                llvm::ConstantInt::get(i32, 0),
                "_fltused"
            );
        }

        {
            std::string verr;
            llvm::raw_string_ostream os(verr);
            if (llvm::verifyModule(*llmod, &os))
                return fail("generated LLVM module is invalid:\n" + os.str());
        }

        // 6. Codegen to a COFF/ELF object in memory.
        llvm::SmallVector<char, 0> obj;
        {
            llvm::raw_svector_ostream os(obj);
            llvm::legacy::PassManager pm;
            if (tm->addPassesToEmitFile(pm, os, nullptr, llvm::CodeGenFileType::ObjectFile))
                return fail("target cannot emit object files");
            pm.run(*llmod);
        }
        artifact_out.object.assign(
            reinterpret_cast<const std::byte*>(obj.data()),
            reinterpret_cast<const std::byte*>(obj.data() + obj.size())
        );
        if (artifact_out.object.empty())
            return fail("object emission produced no bytes");
        return true;
    }

    static bool linkSharedLibraryImpl(
        const AotArtifact& artifact,
        const std::filesystem::path& out_dll,
        const FlowForgeCompileOptions& options,
        std::string* error_out
    )
    {
        const auto fail = [&](std::string msg) {
            if (error_out)
                *error_out = std::move(msg);
            return false;
        };
        if (artifact.object.empty())
            return fail("artifact has no object bytes");

        const std::string linker = findLinker(options);
        if (linker.empty())
            return fail("no linker found (set LUX_FLOWFORGE_LINKER or put "
                        "lld-link / link on PATH)");
        const bool msvc_style =
            linker.find("lld-link") != std::string::npos || linker.find("link") != std::string::npos;
        if (!msvc_style)
            return fail("unsupported linker flavor: " + linker);

        std::error_code ec;
        std::filesystem::create_directories(out_dll.parent_path(), ec);
        std::filesystem::path obj_path = out_dll;
        obj_path.replace_extension(".obj");
        {
            std::ofstream os(obj_path, std::ios::binary | std::ios::trunc);
            if (!os)
                return fail("cannot write " + obj_path.string());
            os.write(
                reinterpret_cast<const char*>(artifact.object.data()),
                static_cast<std::streamsize>(artifact.object.size())
            );
        }

        // Generated code is freestanding (no CRT): imports come through
        // bind_host slots, so the DLL needs neither an entry point nor a
        // default runtime library.
        const std::string out_arg = "/OUT:" + out_dll.string();
        const std::string obj_arg = obj_path.string();
        llvm::SmallVector<llvm::StringRef, 8>
            args{linker, "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/Brepro", out_arg, obj_arg};

        std::string exec_err;
        const int rc = llvm::sys::ExecuteAndWait(
            linker,
            args,
            /*Env=*/std::nullopt,
            /*Redirects=*/{},
            /*SecondsToWait=*/120,
            /*MemoryLimit=*/0,
            &exec_err
        );
        if (rc != 0)
            return fail("linker failed (rc=" + std::to_string(rc) + ") " + exec_err + " [" + linker + "]");
        if (!std::filesystem::exists(out_dll))
            return fail("linker reported success but produced no output");
        return true;
    }

    FlowForgeResult<AotArtifact> compileToObject(
        IRContext& context,
        const FlowGraph& graph,
        const FlowForgeCompileOptions& options
    ) noexcept
    {
        auto suspension_analysis = SuspensionAnalysis::create(graph);
        if (!suspension_analysis)
            return lux::cxx::unexpected(std::move(suspension_analysis.error()));
        return compileToObject(context, graph, options, *suspension_analysis);
    }

    FlowForgeResult<AotArtifact> compileToObject(
        IRContext& context,
        const FlowGraph& graph,
        const FlowForgeCompileOptions& options,
        const SuspensionAnalysis& suspension_analysis
    ) noexcept
    {
        try
        {
            AotArtifact artifact;
            std::string message;
            if (!compileToObjectImpl(context, graph, options, suspension_analysis, artifact, &message))
            {
                auto code = EFlowForgeError::AOT_CODEGEN_FAILED;
                if (message.find("continuation frame") != std::string::npos ||
                    message.find("coroutine dominance") != std::string::npos)
                {
                    code = EFlowForgeError::INVALID_CONTINUATION_FRAME_LAYOUT;
                }
                else if (message.find("async graph function") != std::string::npos)
                {
                    code = EFlowForgeError::UNSUPPORTED_COROUTINE_CONTROL_FLOW;
                }
                else if (message.find("suspension analysis") != std::string::npos)
                {
                    code = EFlowForgeError::UNSUPPORTED_COROUTINE_CONTROL_FLOW;
                }
                return lux::cxx::unexpected(FlowForgeFailure{.code = code, .message = std::move(message)});
            }
            return artifact;
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

    FlowForgeResult<void> linkSharedLibrary(
        const AotArtifact& artifact,
        const std::filesystem::path& out_dll,
        const FlowForgeCompileOptions& options
    ) noexcept
    {
        try
        {
            std::string message;
            if (!linkSharedLibraryImpl(artifact, out_dll, options, &message))
            {
                return lux::cxx::unexpected(
                    FlowForgeFailure{.code = EFlowForgeError::LINK_FAILED, .message = std::move(message)}
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
