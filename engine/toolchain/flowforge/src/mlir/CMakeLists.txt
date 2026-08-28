find_package(MLIR REQUIRED CONFIG)

message(STATUS "Using MLIRConfig.cmake in: ${MLIR_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Detect MLIR version for the trait/include compatibility shims below.
set(MLIR_VERSION_MAJOR ${LLVM_VERSION_MAJOR})
set(MLIR_VERSION_MINOR ${LLVM_VERSION_MINOR})

# Fall back to parsing the version from llvm-config.h if find_package didn't
# surface it directly.
if(NOT MLIR_VERSION_MAJOR)
    message(STATUS "Trying to parse version from header file...")
    if(EXISTS "${MLIR_INCLUDE_DIRS}/llvm/Config/llvm-config.h")
        file(READ "${MLIR_INCLUDE_DIRS}/llvm/Config/llvm-config.h" LLVM_CONFIG_H_CONTENT)
        string(REGEX MATCH "#define LLVM_VERSION_MAJOR ([0-9]+)" _ "${LLVM_CONFIG_H_CONTENT}")
        if(CMAKE_MATCH_1)
            set(MLIR_VERSION_MAJOR ${CMAKE_MATCH_1})
        endif()
        string(REGEX MATCH "#define LLVM_VERSION_MINOR ([0-9]+)" _ "${LLVM_CONFIG_H_CONTENT}")
        if(CMAKE_MATCH_1)
            set(MLIR_VERSION_MINOR ${CMAKE_MATCH_1})
        endif()
    endif()
endif()

message(STATUS "Detected MLIR version: ${MLIR_VERSION_MAJOR}.${MLIR_VERSION_MINOR}")

# Generate the version compatibility header.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/FlowForgeVersionCompat.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/FlowForgeVersionCompat.h
)

# Select the right TableGen compatibility traits for the detected MLIR.
if(MLIR_VERSION_MAJOR GREATER_EQUAL 16)
    # MLIR 16+ exposes the Pure trait directly.
    set(FLOWFORGE_PURE_TRAIT_DEF "// Pure trait is available directly in MLIR 16+")
    set(FLOWFORGE_PURE_TRAIT_NAME "Pure")
    set(FLOWFORGE_ARITH_DIALECT_INCLUDE "mlir/Dialect/Arith/IR/Arith.h")
    set(FLOWFORGE_REGION_BRANCH_INTERFACE "RegionBranchPoint")
    message(STATUS "Using MLIR 16+ compatibility mode")
elseif(MLIR_VERSION_MAJOR EQUAL 15)
    # MLIR 15 uses MemoryEffects<[]>.
    set(FLOWFORGE_PURE_TRAIT_DEF "def FlowForgePure : MemoryEffects<[]>;")
    set(FLOWFORGE_PURE_TRAIT_NAME "FlowForgePure")
    set(FLOWFORGE_ARITH_DIALECT_INCLUDE "mlir/Dialect/Arithmetic/IR/Arithmetic.h")
    set(FLOWFORGE_REGION_BRANCH_INTERFACE "Optional<unsigned>")
    message(STATUS "Using MLIR 15 compatibility mode")
else()
    # MLIR 14 and earlier.
    set(FLOWFORGE_PURE_TRAIT_DEF "def FlowForgePure : NoSideEffect;")
    set(FLOWFORGE_PURE_TRAIT_NAME "FlowForgePure")
    set(FLOWFORGE_ARITH_DIALECT_INCLUDE "mlir/Dialect/StandardOps/IR/Ops.h")
    set(FLOWFORGE_REGION_BRANCH_INTERFACE "Optional<unsigned>")
    message(STATUS "Using MLIR 14- compatibility mode")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/FlowForgeCompatTraits.td.in
    ${CMAKE_CURRENT_BINARY_DIR}/FlowForgeCompatTraits.td
)

# Generate the version-compatible FlowForgeOps.td.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/FlowForgeOps.td.in
    ${CMAKE_CURRENT_BINARY_DIR}/FlowForgeOps.td
)

set(MLIR_BINARY_DIR ${CMAKE_BINARY_DIR})

include(AddLLVM)
include(TableGen)
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
include(AddMLIR)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})
message(STATUS "MLIR_INCLUDE_DIRS: ${MLIR_INCLUDE_DIRS}")
message(STATUS "LLVM_INCLUDE_DIRS: ${LLVM_INCLUDE_DIRS}")
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Generate the dialect via mlir_tablegen.
set(LLVM_TARGET_DEFINITIONS FlowForge.td)
mlir_tablegen(FlowForgeDialect.h.inc    --gen-dialect-decls  --dialect=flowforge)
mlir_tablegen(FlowForgeDialect.cpp.inc  --gen-dialect-defs   --dialect=flowforge)
mlir_tablegen(FlowForgeOps.h.inc        --gen-op-decls       --dialect=flowforge)
mlir_tablegen(FlowForgeOps.cpp.inc      --gen-op-defs        --dialect=flowforge)
mlir_tablegen(FlowForgeTypes.h.inc      --gen-typedef-decls  --typedefs-dialect=flowforge --dialect=flowforge)
mlir_tablegen(FlowForgeTypes.cpp.inc    --gen-typedef-defs   --typedefs-dialect=flowforge --dialect=flowforge)
add_public_tablegen_target(FlowForgeDialectIncGen)

# Create the FlowForge dialect library
add_library(_flowforge_dialect STATIC
    FlowForgeDialect.cpp
)

lux_classify_target(
    TARGET _flowforge_dialect
    LAYER TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE DOMAIN
)

target_include_directories(_flowforge_dialect PRIVATE
    ${FLOWFORGE_MODULE_DIR}/include
    ${FLOWFORGE_MODULE_DIR}/pinclude
    ${CMAKE_CURRENT_BINARY_DIR}
    ${MLIR_INCLUDE_DIRS}
)

add_dependencies(_flowforge_dialect FlowForgeDialectIncGen)

set(MLIR_PUBLIC_LIBS
    MLIRIR
    MLIRSupport
    MLIRParser
    MLIRPass
    MLIRTransforms
    MLIRLLVMDialect
    MLIRFuncDialect
    MLIRArithDialect
    MLIRTensorDialect
    MLIRSCFDialect                  # target dialect of FlowForge lowering
    MLIRControlFlowDialect          # scf -> cf intermediate
    MLIRSCFToControlFlow            # scf -> cf pass
    MLIRArithToLLVM                 # arith -> llvm pass
    MLIRControlFlowToLLVM           # cf -> llvm pass
    MLIRFuncToLLVM                  # func -> llvm pass
    MLIRReconcileUnrealizedCasts    # sweep builtin.unrealized_conversion_cast
    MLIRExecutionEngine             # JIT
    MLIRBuiltinToLLVMIRTranslation  # ExecutionEngine MLIR -> LLVM IR
    MLIRLLVMToLLVMIRTranslation     # ExecutionEngine MLIR -> LLVM IR
)

target_link_libraries(_flowforge_dialect PRIVATE
    ${MLIR_PUBLIC_LIBS}
    MLIRSideEffectInterfaces
    MLIRControlFlowInterfaces
    MLIRSupport
    MLIRParser
    # MLIR Standard Dialects used in flowforge
    MLIRLLVMDialect
    MLIRFuncDialect
    MLIRArithDialect
    MLIRTensorDialect
    # MLIR Transforms and Utilities
    MLIRTransforms
    MLIRTransformUtils
)

# Ensure the dialect library uses position-independent code
set_target_properties(_flowforge_dialect PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(
    _flowforge_dialect
    PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${MLIR_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
