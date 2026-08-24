#include "FlowForgeDialect.h"
#include <mlir/IR/DialectRegistry.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/BuiltinDialect.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Target/LLVMIR/LLVMTranslationInterface.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <iostream>
#include <FlowForgeDialect.h>
#include <lux/engine/authoring/flowforge/FlowGraph.hpp>
#include <lux/engine/authoring/flowforge/ControlNode.hpp>
#include <lux/engine/toolchain/flowforge/mlir/IR.hpp>
#include "FlowForgeTestResult.hpp"


int main(int argc, char* argv[])
{
    // mlir::DialectRegistry registry;
	// registry.insert<mlir::BuiltinDialect>();
	// registry.insert<mlir::func::FuncDialect>();
	// registry.insert<mlir::flowforge::FlowForgeDialect>();
	// LLVM IR dialect
	// registry.insert<mlir::LLVM::LLVMDialect>();

	lux::flowforge::IRContext ir_context;

	auto start_node		= std::make_unique<lux::flowforge::StartNode>();
	auto forloop_node	= std::make_unique<lux::flowforge::ForLoopNode>();
	auto branch_node    = std::make_unique<lux::flowforge::BranchNode>();
	auto return_node    = std::make_unique<lux::flowforge::ReturnNode>();
	
	// Start -> Branch -> (true: ForLoop -> completed | false: direct) ->
	// Return. Both legs converge on Return so it is the branch's
	// post-dominator and lowers at the OUTER scope — a Return wired inside a
	// loop body / branch leg is rejected by the builder (early return is not
	// expressible in structured control flow yet).
	lux::flowforge::LastLink last_link;
	start_node->execOutPin().linkTo(&branch_node->execInPin(), last_link);
	branch_node->execOutPin().linkTo(&forloop_node->execInPin(), last_link);
	const_cast<lux::flowforge::ExecOutPin&>(forloop_node->completed())
		.linkTo(&return_node->execInPin(), last_link);
	const_cast<lux::flowforge::ExecOutPin&>(branch_node->execOutPinDown())
		.linkTo(&return_node->execInPin(), last_link);

	auto graph = lux::flowforge::FlowGraph();
	graph.addNodes(std::move(start_node));
	graph.addNodes(std::move(forloop_node));
	graph.addNodes(std::move(branch_node));
	graph.addNodes(std::move(return_node));

	lux::flowforge::MLIRBuilder builder(&ir_context);
	auto built = builder.generateIR(graph);
	if (!built) {
		std::cerr << "generateIR failed: " << built.error().message << "\n";
		return 1;
	}
	auto ir = std::move(built.value());
	std::cout << ir->toString() << std::endl;

	auto text = ir->toString();
	for (const char* needle :
	     { "flowforge.start", "flowforge.branch", "flowforge.for_loop",
	       "flowforge.token_merge", "flowforge.return" }) {
		if (text.find(needle) == std::string::npos) {
			std::cerr << "MISSING op in IR: " << needle << "\n";
			return 1;
		}
	}

	std::cout << "flowforge_dialect_test: all checks passed\n";
	return 0;
}
