#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE

#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/compiler/IRImpl.hpp>

#include <llvm/Support/raw_ostream.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>

#include <new>
#include <utility>

namespace lux::flowforge
{
    IR::IR() : impl_(std::make_unique<IRImpl>()) {}
    IR::~IR() = default;

    IRImpl& IR::impl() { return *impl_; }
    const IRImpl& IR::impl() const { return *impl_; }

    FlowForgeResult<std::string> IR::toString() const noexcept
    {
        try
        {
            std::string result;
            llvm::raw_string_ostream output(result);
            if (mlir::ModuleOp module = impl_->top_module.get())
                module.print(output);
            else
                output << "// Empty IR - module not generated yet\n";
            return result;
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

    FlowForgeResult<IRContext> IRContext::create() noexcept
    {
        try
        {
            auto context = std::make_unique<mlir::MLIRContext>();
            context->loadDialect<mlir::flowforge::FlowForgeDialect>();
            context->loadDialect<mlir::LLVM::LLVMDialect>();
            context->loadDialect<mlir::func::FuncDialect>();
            context->loadDialect<mlir::arith::ArithDialect>();
            return IRContext(context.release());
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::CONTEXT_CREATION_FAILED});
        }
    }

    IRContext::IRContext(IRContext&& other) noexcept
        : context_(std::exchange(other.context_, nullptr))
    {}

    IRContext& IRContext::operator=(IRContext&& other) noexcept
    {
        if (this == &other)
            return *this;
        delete static_cast<mlir::MLIRContext*>(context_);
        context_ = std::exchange(other.context_, nullptr);
        return *this;
    }

    IRContext::~IRContext()
    {
        delete static_cast<mlir::MLIRContext*>(context_);
    }
}
