//===========================================
//  mlir_jit_add.cpp
//  -----------------------------------------
//  Minimal standalone example that
//  * builds an MLIR module in‑memory
//  * declares an external C function  `int add(int,int)`
//  * calls the function from MLIR (`func.call`)
//  * JIT‑executes via MLIR::ExecutionEngine and prints the result.
//  (Tested against LLVM/MLIR 18.x nightly)
//===========================================

#include <iostream>
#include <memory>
#include <chrono>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/ExecutionEngine/ExecutionEngine.h>
#include <mlir/ExecutionEngine/RunnerUtils.h>
#include <mlir/Target/LLVMIR/Export.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Conversion/Passes.h>
#include <mlir/Target/LLVMIR/LLVMTranslationInterface.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <mlir/Dialect/Func/Transforms/Passes.h>
#include <llvm/Support/Error.h> // Needed for llvm::Error
#include <mlir/IR/Attributes.h> // Needed for StringAttr, UnitAttr

using namespace mlir;

class A
{
public:
    int a;
    std::string b;
};

static void printCurrentTime()
{
    auto current_time = std::chrono::high_resolution_clock::now().time_since_epoch();
    auto nano_second = std::chrono::duration_cast<std::chrono::microseconds>(current_time).count();

    std::cout << nano_second << std::endl;
}

// Utility: create Module containing `@add` declaration and `@main` body.
extern "C" A* create_A()
{
    auto* p = new A;
    p->a = 777;
    p->b = "created in factory()";
    std::cout << "[native create_A]  new A @" << p << '\n';
    return p;
}

extern "C" void destroy_A(A* p)
{
    std::cout << "[native destroy_A] delete A @" << p << '\n';
    delete p;
}

extern "C" void test_func_with_cpparg(A* arg)
{
    std::cout << "test_func_with_cpparg" << std::endl;
    std::cout << "[native test] A.a = " << arg->a << ", A.b = " << arg->b << '\n';
}

extern "C" int add(int x, int y)
{
    printCurrentTime();
    std::cout << "[native add] " << x << " + " << y << " = " << (x + y) << '\n';
    return x + y;
}

// ──────────────────────────────
// Build the MLIR module
// ──────────────────────────────
static ModuleOp createTestModule(MLIRContext& ctx)
{
    ctx.getOrLoadDialect<func::FuncDialect>();
    ctx.getOrLoadDialect<arith::ArithDialect>();
    ctx.getOrLoadDialect<LLVM::LLVMDialect>();

    OpBuilder b(&ctx);
    auto module = ModuleOp::create(b.getUnknownLoc());

    // Basic types
    auto i32 = b.getI32Type();
    auto structA = LLVM::LLVMStructType::getOpaque("struct.A", &ctx);
    auto ptrA = LLVM::LLVMPointerType::get(&ctx);

    // --- External declarations --------------------------------------------------
    auto addDecl = func::FuncOp::create(b.getUnknownLoc(), "add", b.getFunctionType({i32, i32}, {i32}));
    module.push_back(addDecl);

    auto testDecl = func::FuncOp::create(b.getUnknownLoc(), "test_func_with_cpparg", b.getFunctionType({ptrA}, {}));
    module.push_back(testDecl);

    auto createDecl = func::FuncOp::create(b.getUnknownLoc(), "create_A", b.getFunctionType({}, {ptrA}));
    module.push_back(createDecl);

    auto destroyDecl = func::FuncOp::create(b.getUnknownLoc(), "destroy_A", b.getFunctionType({ptrA}, {}));
    module.push_back(destroyDecl);
    // --------------------------------------------------------------

    // --- @main(): entry point -------------------------------------------
    auto mainFunc = func::FuncOp::create(b.getUnknownLoc(), "main", b.getFunctionType({}, {i32}));
    mainFunc->setAttr("llvm.emit_c_interface", UnitAttr::get(&ctx));

    {
        auto& entry = *mainFunc.addEntryBlock();
        b.setInsertionPointToStart(&entry);

        // add(5,7)
        auto c5 = b.create<arith::ConstantIntOp>(b.getUnknownLoc(), 5, 32);
        auto c7 = b.create<arith::ConstantIntOp>(b.getUnknownLoc(), 7, 32);
        auto call = b.create<func::CallOp>(b.getUnknownLoc(), addDecl, ValueRange{c5, c7});

        // A* p = create_A()
        auto pA = b.create<func::CallOp>(b.getUnknownLoc(), createDecl, ValueRange{}).getResult(0);

        // test_func_with_cpparg(p)
        b.create<func::CallOp>(b.getUnknownLoc(), testDecl, ValueRange{pA});

        // destroy_A(p)
        b.create<func::CallOp>(b.getUnknownLoc(), destroyDecl, ValueRange{pA});

        // return 0
        // auto c0 = b.create<arith::ConstantIntOp>(b.getUnknownLoc(), 0, 32);
        auto ret = b.create<arith::ConstantIntOp>(b.getUnknownLoc(), 7, 32);
        b.create<func::ReturnOp>(b.getUnknownLoc(), ValueRange{ret});
    }
    module.push_back(mainFunc);
    // --------------------------------------------------------------

    return module;
}

int main(int argc, char** argv)
{
    llvm::InitLLVM x(argc, argv);
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // 1. Build IR module.line
    mlir::MLIRContext ctx;
    mlir::registerLLVMDialectTranslation(ctx); // enable LLVM lowering
    mlir::registerBuiltinDialectTranslation(ctx);
    auto module = createTestModule(ctx);

    // 2. Run lowering passes.
    mlir::PassManager pm(&ctx);
    pm.addPass(mlir::createConvertFuncToLLVMPass());
    if (mlir::failed(pm.run(module)))
    {
        std::cerr << "PassManager failed\n";
        return 1;
    }

    // ---- Add this line to print the IR ----
    std::cerr << "--- MLIR IR after passes ---\n";
    module.dump();
    std::cerr << "--------------------------\n";
    // ---------------------------------------

    // 3. Build JIT ExecutionEngine (Simpler creation)
    llvm::Expected<std::unique_ptr<mlir::ExecutionEngine>> maybeEngine =
        mlir::ExecutionEngine::create(module); // Use simpler overload

    if (!maybeEngine)
    {
        module.dump();
        llvm::errs() << "Failed to create ExecutionEngine: " << llvm::toString(maybeEngine.takeError()) << "\n";
        return 1;
    }
    auto& engine = *maybeEngine;

    // 4. Register native symbol `add` using registerSymbols
    engine->registerSymbols([&](llvm::orc::MangleAndInterner mangle) {
        using llvm::orc::ExecutorAddr;
        using llvm::orc::ExecutorSymbolDef;
        using llvm::JITSymbolFlags;
        llvm::orc::SymbolMap map;

        auto ins = [&](const char* name, void* addr) {
            map[mangle(name)] = ExecutorSymbolDef{ExecutorAddr::fromPtr(addr), JITSymbolFlags::Exported};
        };
        ins("add", (void*)&add);
        ins("create_A", (void*)&create_A);
        ins("destroy_A", (void*)&destroy_A);
        ins("test_func_with_cpparg", (void*)&test_func_with_cpparg);
        return map;
    });

    // 5. Invoke.
    int32_t result = 0;
    printCurrentTime();
    llvm::Error invocationResult = engine->invoke("main", result);
    if (invocationResult)
    {
        llvm::errs() << "JIT invocation failed: " << llvm::toString(std::move(invocationResult)) << "\n";
        return 1;
    }

    std::cout << "MLIR main() returned " << result << std::endl;
    return 0;
}
