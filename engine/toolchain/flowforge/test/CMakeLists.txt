if(NOT LUX_ENABLE_FLOWFORGE_MLIR)
    message(STATUS "FlowForge MLIR tests skipped: LUX_ENABLE_FLOWFORGE_MLIR=OFF")
    return()
endif()

find_package(LLVM CONFIG REQUIRED)
find_package(MLIR REQUIRED CONFIG)

set(_FFM_DIALECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../dialect)

add_executable(
    flowforge_dialect_test
    flowforge_dialect_test.cpp
)

target_include_directories(
    flowforge_dialect_test
    PRIVATE
        ${_FFM_DIALECT_DIR}
)

target_link_libraries(
    flowforge_dialect_test
    PRIVATE
    toolchain_flowforge
    toolchain_flowforge_dialect

    MLIRIR           # Core IR components (Operations, Types, etc.)
    MLIRSupport      # Core support utilities

    # Dialects used
    MLIRFuncDialect  # For func.func, func.call, func.return
    MLIRArithDialect # For arith.constant (used in createTestModule)

    MLIRPass         # Core Pass infrastructure (PassManager)
    MLIRTransforms   # General transformations (includes FuncToLLVM)
    MLIRFuncToLLVM   # Specifically needed for createConvertFuncToLLVMPass

    # Execution and LLVM Interaction
    MLIRExecutionEngine # For mlir::ExecutionEngine

    LLVMCore         # Core LLVM infrastructure
    LLVMSupport      # Support libraries (StringRef, Error, etc.)

    LLVMExecutionEngine # For JIT capabilities
    LLVMOrcJIT          # For ORC JIT components (MangleAndInterner, SymbolMap)
    LLVMRuntimeDyld     # Dynamic linker needed by ExecutionEngine

    LLVMipo          # Interprocedural optimizations (sometimes needed by passes)
    LLVMPasses       # Standard LLVM passes
)

add_executable(
    mlir_jit_add
    mlir_jit_add.cpp
)

target_include_directories(
    mlir_jit_add
    PRIVATE
        ${_FFM_DIALECT_DIR}
)

target_link_libraries(
    mlir_jit_add
    PRIVATE
    toolchain_flowforge_dialect
    
    # === MLIR Libraries ===
    # Core
    MLIRIR           # Core IR components (Operations, Types, etc.)
    MLIRSupport      # Core support utilities

    # Dialects used
    MLIRFuncDialect  # For func.func, func.call, func.return
    MLIRArithDialect # For arith.constant (used in createTestModule)
    # MLIRLLVMIR

    # Passes and Transformations
    MLIRPass         # Core Pass infrastructure (PassManager)
    MLIRTransforms   # General transformations (includes FuncToLLVM)
    MLIRFuncToLLVM   # Specifically needed for createConvertFuncToLLVMPass

    # Execution and LLVM Interaction
    MLIRExecutionEngine # For mlir::ExecutionEngine
    # MLIRTargetLLVMIR

    # === LLVM Libraries ===
    # Core
    LLVMCore         # Core LLVM infrastructure
    LLVMSupport      # Support libraries (StringRef, Error, etc.)

    # JIT and Execution
    LLVMExecutionEngine # For JIT capabilities
    LLVMOrcJIT          # For ORC JIT components (MangleAndInterner, SymbolMap)
    LLVMRuntimeDyld     # Dynamic linker needed by ExecutionEngine

    # Target Initialization (Replace X86 with your native target or use generic ones)
    # LLVMNativeTarget      # Pulls in info for the host target (e.g., LLVMX86TargetInfo)
    # LLVMNativeCodeGen     # Pulls in codegen for the host target (e.g., LLVMX86CodeGen)
    # LLVMNativeAsmPrinter # Pulls in assembler printer (e.g., LLVMX86AsmPrinter)
    # Alternatively, be specific:
    # LLVMX86Info
    # LLVMX86CodeGen
    # LLVMX86AsmPrinter
    # LLVMTarget # General target library

    # Other potentially needed LLVM libs
    LLVMipo          # Interprocedural optimizations (sometimes needed by passes)
    LLVMPasses       # Standard LLVM passes
)

add_executable(
    flowforge_end_to_end_test
    flowforge_end_to_end_test.cpp
)

target_include_directories(
    flowforge_end_to_end_test
    PRIVATE
        ${_FFM_DIALECT_DIR}
)

target_link_libraries(
    flowforge_end_to_end_test
    PRIVATE
    toolchain_flowforge
    toolchain_flowforge_dialect

    MLIRIR           # Core IR components (Operations, Types, etc.)
    MLIRSupport      # Core support utilities

    # Dialects used
    MLIRFuncDialect  # For func.func, func.call, func.return
    MLIRArithDialect # For arith.constant (used in createTestModule)

    MLIRPass         # Core Pass infrastructure (PassManager)
    MLIRTransforms   # General transformations (includes FuncToLLVM)
    MLIRFuncToLLVM   # Specifically needed for createConvertFuncToLLVMPass
    MLIRArithToLLVM  # For arithmetic to LLVM conversion

    # Execution and LLVM Interaction
    MLIRExecutionEngine # For mlir::ExecutionEngine

    LLVMCore         # Core LLVM infrastructure
    LLVMSupport      # Support libraries (StringRef, Error, etc.)

    LLVMExecutionEngine # For JIT capabilities
    LLVMOrcJIT          # For ORC JIT components (MangleAndInterner, SymbolMap)
    LLVMRuntimeDyld     # Dynamic linker needed by ExecutionEngine

    LLVMipo          # Interprocedural optimizations (sometimes needed by passes)
    LLVMPasses       # Standard LLVM passes
)

# ── Draw-line demo-node test (Milestone D2) ────────────────────────────────────
# The EXACT "Draw Line" node FlowGraphPanel registers: NATIVE_FUNC_CALL over a
# hand-built 9-float signature with every argument as an unlinked pin constant,
# JIT-executed via the symbol-binding runMainJIT. Guards the demo node headlessly.
add_executable(
    flowforge_draw_line_test
    flowforge_draw_line_test.cpp
)

target_link_libraries(
    flowforge_draw_line_test
    PRIVATE
    toolchain_flowforge

    LLVMExecutionEngine
    LLVMOrcJIT
    LLVMRuntimeDyld
    LLVMX86Info
    LLVMX86Desc
    LLVMX86CodeGen
    LLVMX86AsmParser
    LLVMSupport
    LLVMCore
    LLVMipo
    LLVMPasses
)

# ── Hello-world editor-parity test ─────────────────────────────────────────────
# The EXACT default graph FlowGraphPanel seeds (Start -> Print(42) -> Return,
# Print = NATIVE_FUNC_CALL of a hand-built reflected signature), driven through
# generateIR + the symbol-binding runMainJIT overload. Guards the editor's
# run-button path headlessly.
add_executable(
    flowforge_hello_world_test
    flowforge_hello_world_test.cpp
)

target_link_libraries(
    flowforge_hello_world_test
    PRIVATE
    toolchain_flowforge

    LLVMExecutionEngine
    LLVMOrcJIT
    LLVMRuntimeDyld
    LLVMX86Info
    LLVMX86Desc
    LLVMX86CodeGen
    LLVMX86AsmParser
    LLVMSupport
    LLVMCore
    LLVMipo
    LLVMPasses
)

# ── Pipeline smoke test — FlowGraph -> MLIRBuilder regression baseline ─────────
# Exercises the full FlowGraph -> FlowForge dialect -> SCF -> LLVM -> JIT
# pipeline. Designed to fail loudly when any stage reshapes the output IR so
# the baseline gets consciously updated rather than silently drifting.
add_executable(
    flowforge_pipeline_smoke_test
    flowforge_pipeline_smoke_test.cpp
)

target_link_libraries(
    flowforge_pipeline_smoke_test
    PRIVATE
    toolchain_flowforge  # pulls in Authoring graph model + core::meta + MLIR transitively

    # JIT pipeline needs the host LLVM target + ORC components linked into
    # the test binary (the dialect lib pulls in MLIRExecutionEngine but not
    # the host-target / ORC libs that ExecutionEngine actually uses at
    # runtime).
    LLVMExecutionEngine
    LLVMOrcJIT
    LLVMRuntimeDyld
    LLVMX86Info
    LLVMX86Desc
    LLVMX86CodeGen
    LLVMX86AsmParser
    LLVMSupport
    LLVMCore
    LLVMipo
    LLVMPasses
)

# ── M-A: pure dataflow + graph variables ──────────────────────────────────────
foreach(_ma_test flowforge_pure_dataflow_test flowforge_variable_test flowforge_multi_func_test flowforge_early_exit_test flowforge_while_cond_test flowforge_field_access_test flowforge_event_dispatch_test flowforge_instance_state_test flowforge_aot_test)
    add_executable(${_ma_test} ${_ma_test}.cpp)
    target_link_libraries(
        ${_ma_test}
        PRIVATE
        toolchain_flowforge
        LLVMExecutionEngine
        LLVMOrcJIT
        LLVMRuntimeDyld
        LLVMX86Info
        LLVMX86Desc
        LLVMX86CodeGen
        LLVMX86AsmParser
        LLVMSupport
        LLVMCore
        LLVMipo
        LLVMPasses
    )
endforeach()

target_link_libraries(
    flowforge_aot_test
    PRIVATE
        lux::engine::function::script_native
)
