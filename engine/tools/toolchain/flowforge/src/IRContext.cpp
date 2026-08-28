#include "FlowForgeDialect.h"
#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE

#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/compiler/IRImpl.hpp>

#include <llvm/Support/raw_ostream.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>

#include <utility>

namespace lux::flowforge
{
    IR::IR() : impl_(std::make_unique<IRImpl>()) {}
    IR::~IR() = default;

    IRImpl& IR::impl() { return *impl_; }
    const IRImpl& IR::impl() const { return *impl_; }

    std::string IR::toString() const
    {
        std::string result;
        llvm::raw_string_ostream output(result);
        if (mlir::ModuleOp module = impl_->top_module.get())
            module.print(output);
        else
            output << "// Empty IR - module not generated yet\n";
        return result;
    }

    IRContext::IRContext()
        : context_(new mlir::MLIRContext)
    {
        auto* context = static_cast<mlir::MLIRContext*>(context_);
        context->loadDialect<mlir::flowforge::FlowForgeDialect>();
        context->loadDialect<mlir::LLVM::LLVMDialect>();
        context->loadDialect<mlir::func::FuncDialect>();
        context->loadDialect<mlir::arith::ArithDialect>();
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
