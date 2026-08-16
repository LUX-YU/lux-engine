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
//   4. Per OnEvent entry `lux_event_X(state, a0..aN)`: a call_frame wrapper
//      `int lux_fnwrap_X(lux_script_call_frame*)` that pulls the state
//      block out of user_context, loads each argument from args[i].data,
//      calls the event function and returns 0. Frame/slot field offsets
//      come from offsetof() on the REAL C structs — both sides compile
//      against the same lux_script_abi.h.
//   5. A static lux_script_module_desc (function table = the wrappers,
//      names = event display names) + lux_script_get_module() returning
//      it. get_module and bind_host are dllexport'ed; nothing else is.
//   6. TargetMachine -> COFF object bytes; linkSharedLibrary runs lld-link
//      (or link.exe) out of process: /DLL /NOENTRY /NODEFAULTLIB.
//===========================================================================
#include "lux/engine/toolchain/flowforge/mlir/AOT.hpp"
#include "lux/engine/toolchain/flowforge/mlir/IR.hpp"
#include "lux/engine/toolchain/flowforge/mlir/IRImpl.hpp"
#include "lux/engine/toolchain/flowforge/mlir/Passes.hpp"
#include "lux/engine/toolchain/flowforge/mlir/ScriptInstance.hpp"
#include "lux/engine/authoring/flowforge/FlowGraph.hpp"
#include "lux/engine/authoring/flowforge/FunctionalNode.hpp"

#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/meta/Meta.hpp>

#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Export.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdlib>
#include <cstring>
#include <fstream>

// The descriptor globals are emitted as LLVM constant structs whose layout
// must byte-match the C structs the runtime reads. The natural LLVM layout
// of the element sequences used below does match MSVC/Itanium for these
// plain structs; the static_asserts pin the C side, and emitModuleDesc
// re-checks the LLVM side against the DataLayout at cook time.
static_assert(sizeof(lux_script_type_desc)     == 32, "ABI drift: type_desc");
static_assert(sizeof(lux_script_function_desc) == 56, "ABI drift: function_desc");
static_assert(sizeof(lux_script_module_desc)   == 32, "ABI drift: module_desc");
static_assert(sizeof(lux_script_value_slot)    == 24, "ABI drift: value_slot");

namespace lux::flowforge
{
    namespace
    {
        struct EventInfo
        {
            const OnEventNode* node;
            std::string        symbol;   // func symbol: lux_event_<sanitized>
        };

        uint8_t refTypeToValueKind(const lux::meta::RefType& rt)
        {
            using lux::meta::EBaseType;
            using lux::meta::ETypeQual;
            switch (static_cast<ETypeQual>(rt.qtype.qual)) {
                case ETypeQual::Ptr:
                case ETypeQual::PtrToConst:
                case ETypeQual::ConstPtr:
                case ETypeQual::ConstPtrToConst:
                    return LUX_SCRIPT_VK_OBJECT_PTR;
                default: break;
            }
            switch (static_cast<EBaseType>(rt.qtype.base)) {
                case EBaseType::Bool:   return LUX_SCRIPT_VK_BOOL;
                case EBaseType::Int8:
                case EBaseType::Int16:
                case EBaseType::Int32:  return LUX_SCRIPT_VK_INT32;
                case EBaseType::Uint8:
                case EBaseType::Uint16:
                case EBaseType::Uint32: return LUX_SCRIPT_VK_UINT32;
                case EBaseType::Int64:  return LUX_SCRIPT_VK_INT64;
                case EBaseType::Uint64: return LUX_SCRIPT_VK_UINT64;
                case EBaseType::Float:  return LUX_SCRIPT_VK_FLOAT;
                case EBaseType::Double: return LUX_SCRIPT_VK_DOUBLE;
                case EBaseType::Record: return LUX_SCRIPT_VK_STRUCT_REF;
                default:                return LUX_SCRIPT_VK_OBJECT_PTR;
            }
        }

        llvm::Constant* makeCStr(llvm::Module& m, llvm::StringRef s,
                                 const llvm::Twine& name)
        {
            auto* data = llvm::ConstantDataArray::getString(m.getContext(), s,
                                                            /*AddNull=*/true);
            auto* g = new llvm::GlobalVariable(
                m, data->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, data, name);
            return g;   // opaque-pointer world: the global IS a ptr constant
        }

        void exportSymbol(llvm::Function* f, const llvm::Triple& triple)
        {
            if (triple.isOSBinFormatCOFF())
                f->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
            else
                f->setVisibility(llvm::GlobalValue::DefaultVisibility);
        }

        // ---- step 2 + 3: import slots + bind_host --------------------------
        bool rewriteImportsToSlots(llvm::Module& m,
                                   std::vector<std::string>& imports_out,
                                   std::string& err)
        {
            auto& ctx = m.getContext();
            auto* ptr_ty = llvm::PointerType::get(ctx, 0);

            llvm::SmallVector<llvm::Function*, 16> externs;
            for (llvm::Function& f : m.functions())
                if (f.isDeclaration() && !f.isIntrinsic())
                    externs.push_back(&f);
            if (externs.empty())
                return true;

            struct Slot { std::string name; llvm::GlobalVariable* g; };
            std::vector<Slot> slots;
            slots.reserve(externs.size());

            for (llvm::Function* f : externs) {
                const std::string name = f->getName().str();
                auto* slot = new llvm::GlobalVariable(
                    m, ptr_ty, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantPointerNull::get(ptr_ty),
                    "_lfimp_" + name);

                for (llvm::User* u : llvm::make_early_inc_range(f->users())) {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(u);
                    if (!call || call->getCalledOperand() != f) {
                        err = "import '" + name + "' is referenced by a "
                              "non-call use (address taken?) — cannot slot it";
                        return false;
                    }
                    llvm::IRBuilder<> b(call);
                    auto* fp = b.CreateLoad(ptr_ty, slot, name + ".fp");
                    call->setCalledOperand(fp);
                }
                if (!f->use_empty()) {
                    err = "import '" + name + "' still has uses after rewrite";
                    return false;
                }
                f->eraseFromParent();
                slots.push_back(Slot{name, slot});
                imports_out.push_back(name);
            }

            // int lux_script_bind_host(lux_host_resolve_fn, void*, uint32_t)
            auto* i32 = llvm::Type::getInt32Ty(ctx);
            auto* bind_ft = llvm::FunctionType::get(
                i32, {ptr_ty, ptr_ty, i32}, /*vararg=*/false);
            auto* bind_fn = llvm::Function::Create(
                bind_ft, llvm::GlobalValue::ExternalLinkage,
                LUX_SCRIPT_BIND_HOST_ENTRY, m);

            auto* entry = llvm::BasicBlock::Create(ctx, "entry", bind_fn);
            llvm::IRBuilder<> b(entry);
            llvm::Value* resolve  = bind_fn->getArg(0);
            llvm::Value* host_ctx = bind_fn->getArg(1);
            auto* resolve_ft = llvm::FunctionType::get(
                ptr_ty, {ptr_ty, ptr_ty}, /*vararg=*/false);

            llvm::Value* missing = llvm::ConstantInt::get(i32, 0);
            for (const Slot& s : slots) {
                auto* name_c = makeCStr(m, s.name, "_lfimp_name");
                auto* addr = b.CreateCall(resolve_ft, resolve,
                                          {host_ctx, name_c});
                b.CreateStore(addr, s.g);
                auto* isnull = b.CreateICmpEQ(
                    addr, llvm::ConstantPointerNull::get(ptr_ty));
                missing = b.CreateAdd(missing, b.CreateZExt(isnull, i32));
            }
            b.CreateRet(missing);
            return true;
        }

        // ---- step 4: one call_frame wrapper per event ----------------------
        llvm::Function* emitEventWrapper(llvm::Module& m,
                                         const EventInfo& ev,
                                         std::string& err)
        {
            auto& ctx = m.getContext();
            auto* ptr_ty = llvm::PointerType::get(ctx, 0);
            auto* i32 = llvm::Type::getInt32Ty(ctx);
            auto* i64 = llvm::Type::getInt64Ty(ctx);

            llvm::Function* target = m.getFunction(ev.symbol);
            if (!target) {
                err = "event function '" + ev.symbol + "' missing in module";
                return nullptr;
            }
            llvm::FunctionType* tft = target->getFunctionType();
            // Leading param is the instance-state pointer; payload follows.
            const size_t payload_count = tft->getNumParams() - 1;

            auto* wrap_ft = llvm::FunctionType::get(i32, {ptr_ty}, false);
            auto* wrap = llvm::Function::Create(
                wrap_ft, llvm::GlobalValue::InternalLinkage,
                "lux_fnwrap_" + ev.symbol, m);

            auto* entry = llvm::BasicBlock::Create(ctx, "entry", wrap);
            llvm::IRBuilder<> b(entry);
            llvm::Value* frame = wrap->getArg(0);

            auto gepByte = [&](llvm::Value* base, uint64_t off) {
                return b.CreateGEP(b.getInt8Ty(), base,
                                   llvm::ConstantInt::get(i64, off));
            };

            // state = frame->user_context (the per-instance block).
            llvm::Value* state = b.CreateLoad(
                ptr_ty,
                gepByte(frame, offsetof(lux_script_call_frame, user_context)),
                "state");

            llvm::SmallVector<llvm::Value*, 8> call_args;
            call_args.push_back(state);
            if (payload_count > 0) {
                llvm::Value* args_base = b.CreateLoad(
                    ptr_ty,
                    gepByte(frame, offsetof(lux_script_call_frame, args)),
                    "args");
                for (size_t i = 0; i < payload_count; ++i) {
                    llvm::Value* slot =
                        gepByte(args_base, i * sizeof(lux_script_value_slot));
                    llvm::Value* data = b.CreateLoad(
                        ptr_ty,
                        gepByte(slot, offsetof(lux_script_value_slot, data)));
                    // args[i].data points AT the value's storage; load it
                    // with the event function's own parameter type.
                    call_args.push_back(
                        b.CreateLoad(tft->getParamType(
                                         static_cast<unsigned>(i + 1)),
                                     data));
                }
            }

            b.CreateCall(target, call_args);
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
            return wrap;
        }

        // ---- step 5: descriptor globals + lux_script_get_module ------------
        bool emitModuleDesc(llvm::Module& m,
                            const std::string& module_name,
                            const std::vector<EventInfo>& events,
                            const std::vector<llvm::Function*>& wrappers,
                            std::string& err)
        {
            auto& ctx = m.getContext();
            const llvm::DataLayout& dl = m.getDataLayout();
            auto* ptr_ty = llvm::PointerType::get(ctx, 0);
            auto* i8  = llvm::Type::getInt8Ty(ctx);
            auto* i32 = llvm::Type::getInt32Ty(ctx);
            auto* i64 = llvm::Type::getInt64Ty(ctx);
            auto* pad7 = llvm::ArrayType::get(i8, 7);

            // Mirrors of the C structs. checkLayout guards against any
            // DataLayout divergence from the host compiler's layout.
            auto* type_desc_ty = llvm::StructType::create(
                ctx, {ptr_ty, i64, i32, i32, i8, pad7}, "lux_script_type_desc");
            auto* func_desc_ty = llvm::StructType::create(
                ctx, {ptr_ty, i64, ptr_ty, i32, ptr_ty, i32, ptr_ty},
                "lux_script_function_desc");
            auto* module_desc_ty = llvm::StructType::create(
                ctx, {ptr_ty, i32, i32, ptr_ty, i32, i32},
                "lux_script_module_desc");

            const auto checkLayout = [&](llvm::StructType* t, size_t c_size,
                                         const char* what) {
                if (dl.getTypeAllocSize(t) != c_size) {
                    err = std::string("ABI layout mismatch for ") + what;
                    return false;
                }
                return true;
            };
            if (!checkLayout(type_desc_ty, sizeof(lux_script_type_desc), "type_desc")
                || !checkLayout(func_desc_ty, sizeof(lux_script_function_desc), "function_desc")
                || !checkLayout(module_desc_ty, sizeof(lux_script_module_desc), "module_desc"))
                return false;

            auto* null_ptr = llvm::ConstantPointerNull::get(ptr_ty);
            auto* pad_zero = llvm::ConstantAggregateZero::get(pad7);

            llvm::SmallVector<llvm::Constant*, 8> fn_descs;
            for (size_t e = 0; e < events.size(); ++e) {
                const auto& params = events[e].node->paramInfos();

                // Per-event argument type_desc array (may be empty).
                llvm::Constant* args_ptr = null_ptr;
                if (!params.empty()) {
                    llvm::SmallVector<llvm::Constant*, 8> arg_descs;
                    for (const auto& p : params) {
                        const auto* rt = p.type;
                        arg_descs.push_back(llvm::ConstantStruct::get(
                            type_desc_ty,
                            {
                                makeCStr(m, p.name, "_lfd_argname"),
                                llvm::ConstantInt::get(i64, rt ? rt->hash : 0),
                                llvm::ConstantInt::get(i32, rt ? rt->size : 0),
                                llvm::ConstantInt::get(
                                    i32, rt ? std::min<uint32_t>(rt->size, 8u) : 0),
                                llvm::ConstantInt::get(
                                    i8, rt ? refTypeToValueKind(*rt)
                                           : uint8_t(LUX_SCRIPT_VK_VOID)),
                                pad_zero,
                            }));
                    }
                    auto* arr_ty = llvm::ArrayType::get(
                        type_desc_ty, arg_descs.size());
                    args_ptr = new llvm::GlobalVariable(
                        m, arr_ty, /*isConstant=*/true,
                        llvm::GlobalValue::PrivateLinkage,
                        llvm::ConstantArray::get(arr_ty, arg_descs),
                        "_lfd_args");
                }

                fn_descs.push_back(llvm::ConstantStruct::get(
                    func_desc_ty,
                    {
                        makeCStr(m, events[e].node->name(), "_lfd_fnname"),
                        llvm::ConstantInt::get(i64, 0),   // symbol_id unused
                        args_ptr,
                        llvm::ConstantInt::get(
                            i32, static_cast<uint32_t>(params.size())),
                        null_ptr,                          // no returns today
                        llvm::ConstantInt::get(i32, 0),
                        wrappers[e],
                    }));
            }

            llvm::Constant* fns_ptr = null_ptr;
            if (!fn_descs.empty()) {
                auto* fns_ty = llvm::ArrayType::get(func_desc_ty, fn_descs.size());
                fns_ptr = new llvm::GlobalVariable(
                    m, fns_ty, /*isConstant=*/true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(fns_ty, fn_descs), "_lfd_functions");
            }

            auto* desc = new llvm::GlobalVariable(
                m, module_desc_ty, /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage,
                llvm::ConstantStruct::get(
                    module_desc_ty,
                    {
                        makeCStr(m, module_name, "_lfd_modname"),
                        llvm::ConstantInt::get(i32, LUX_SCRIPT_ABI_VERSION),
                        llvm::ConstantInt::get(i32, 0),
                        fns_ptr,
                        llvm::ConstantInt::get(
                            i32, static_cast<uint32_t>(fn_descs.size())),
                        llvm::ConstantInt::get(i32, 0),
                    }),
                "_lfd_module");

            // const lux_script_module_desc* lux_script_get_module(void)
            auto* get_ft = llvm::FunctionType::get(ptr_ty, {}, false);
            auto* get_fn = llvm::Function::Create(
                get_ft, llvm::GlobalValue::ExternalLinkage,
                LUX_SCRIPT_MODULE_ENTRY, m);
            auto* bb = llvm::BasicBlock::Create(ctx, "entry", get_fn);
            llvm::IRBuilder<> b(bb);
            b.CreateRet(desc);
            return true;
        }

        std::string findLinker(const AotOptions& options)
        {
            if (!options.linker.empty())
                return options.linker.string();
            if (const char* env = std::getenv("LUX_FLOWFORGE_LINKER");
                env && *env)
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

    bool compileToObject(IRContext& ctx, const FlowGraph& graph,
                         const AotOptions& options, AotArtifact& artifact_out,
                         std::string* error_out)
    {
        const auto fail = [&](std::string msg) {
            if (error_out) *error_out = std::move(msg);
            return false;
        };

        artifact_out = AotArtifact{};
        artifact_out.module_name = options.module_name;

        // Event table straight from the graph (same walk as the JIT host).
        std::vector<EventInfo> events;
        for (const auto& storage : graph.nodes()) {
            const Node* n = storage.node.get();
            if (!n || n->operation() != ENodeOperation::ON_EVENT) continue;
            const auto& ev = static_cast<const OnEventNode&>(*n);
            events.push_back(EventInfo{
                &ev, FlowScriptInstance::eventSymbol(ev.name()) });
            artifact_out.events.push_back(AotEventDesc{
                ev.name(), ev.paramInfos().size() });
        }

        // 1. The exact JIT lowering pipeline, then LLVM IR translation.
        MLIRBuilder builder(&ctx);
        auto built = builder.generateIR(graph);
        if (!built)
            return fail("compile failed: " + built.error().message);
        std::unique_ptr<IR> ir = std::move(built.value());
        auto lowered = lowerToLLVM(*ir);
        if (!lowered)
            return fail("compile failed: " + lowered.error().message);

        artifact_out.state_size     = ir->impl().state_size;
        artifact_out.state_hash     = ir->impl().state_hash;
        artifact_out.state_defaults = ir->impl().state_defaults;

        mlir::ModuleOp module = ir->impl().top_module.get();
        auto* mlir_ctx = module.getContext();
        mlir::registerBuiltinDialectTranslation(*mlir_ctx);
        mlir::registerLLVMDialectTranslation(*mlir_ctx);

        llvm::LLVMContext llctx;
        auto llmod = mlir::translateModuleToLLVMIR(module, llctx,
                                                   options.module_name);
        if (!llmod)
            return fail("MLIR -> LLVM IR translation failed");

        // Target setup first: emitModuleDesc validates struct layouts
        // against the DataLayout.
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        const std::string triple_str = llvm::sys::getDefaultTargetTriple();
        std::string lookup_err;
        const llvm::Target* target =
            llvm::TargetRegistry::lookupTarget(triple_str, lookup_err);
        if (!target)
            return fail("no LLVM target for '" + triple_str + "': " + lookup_err);
        std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
            triple_str, "generic", "", llvm::TargetOptions{},
            llvm::Reloc::PIC_));
        if (!tm)
            return fail("createTargetMachine failed");
        llmod->setTargetTriple(triple_str);
        llmod->setDataLayout(tm->createDataLayout());
        const llvm::Triple triple(triple_str);

        // 2 + 3. Imports -> slots + bind_host.
        {
            std::string err;
            if (!rewriteImportsToSlots(*llmod, artifact_out.imports, err))
                return fail(std::move(err));
            if (llvm::Function* bind =
                    llmod->getFunction(LUX_SCRIPT_BIND_HOST_ENTRY))
                exportSymbol(bind, triple);
        }

        // 4. Event wrappers.
        std::vector<llvm::Function*> wrappers;
        wrappers.reserve(events.size());
        for (const EventInfo& ev : events) {
            std::string err;
            llvm::Function* w = emitEventWrapper(*llmod, ev, err);
            if (!w) return fail(std::move(err));
            wrappers.push_back(w);
        }

        // 5. Module descriptor + entry.
        {
            std::string err;
            if (!emitModuleDesc(*llmod, options.module_name, events,
                                wrappers, err))
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
                *llmod, i32, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage,
                llvm::ConstantInt::get(i32, 0), "_fltused");
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
            if (tm->addPassesToEmitFile(pm, os, nullptr,
                                        llvm::CodeGenFileType::ObjectFile))
                return fail("target cannot emit object files");
            pm.run(*llmod);
        }
        artifact_out.object.assign(
            reinterpret_cast<const std::byte*>(obj.data()),
            reinterpret_cast<const std::byte*>(obj.data() + obj.size()));
        if (artifact_out.object.empty())
            return fail("object emission produced no bytes");
        return true;
    }

    bool linkSharedLibrary(const AotArtifact& artifact,
                           const std::filesystem::path& out_dll,
                           const AotOptions& options, std::string* error_out)
    {
        const auto fail = [&](std::string msg) {
            if (error_out) *error_out = std::move(msg);
            return false;
        };
        if (artifact.object.empty())
            return fail("artifact has no object bytes");

        const std::string linker = findLinker(options);
        if (linker.empty())
            return fail("no linker found (set LUX_FLOWFORGE_LINKER or put "
                        "lld-link / link on PATH)");
        const bool msvc_style =
            linker.find("lld-link") != std::string::npos
            || linker.find("link") != std::string::npos;
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
            os.write(reinterpret_cast<const char*>(artifact.object.data()),
                     static_cast<std::streamsize>(artifact.object.size()));
        }

        // Generated code is freestanding (no CRT): imports come through
        // bind_host slots, so the DLL needs neither an entry point nor a
        // default runtime library.
        const std::string out_arg = "/OUT:" + out_dll.string();
        const std::string obj_arg = obj_path.string();
        llvm::SmallVector<llvm::StringRef, 8> args{
            linker, "/DLL", "/NOENTRY", "/NODEFAULTLIB", out_arg, obj_arg };

        std::string exec_err;
        const int rc = llvm::sys::ExecuteAndWait(
            linker, args, /*Env=*/std::nullopt, /*Redirects=*/{},
            /*SecondsToWait=*/120, /*MemoryLimit=*/0, &exec_err);
        if (rc != 0)
            return fail("linker failed (rc=" + std::to_string(rc) + ") "
                        + exec_err + " [" + linker + "]");
        if (!std::filesystem::exists(out_dll))
            return fail("linker reported success but produced no output");
        return true;
    }
}
