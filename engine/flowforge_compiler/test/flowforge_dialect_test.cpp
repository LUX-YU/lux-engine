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
#include <lux/engine/flowforge/FlowGraph.hpp>
#include <lux/engine/flowforge/ControlNode.hpp>
#include <lux/engine/flowforge/mlir/IR.hpp>


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
	
	lux::flowforge::LastLink last_link;
	start_node->execOutPin().linkTo(&branch_node->execInPin(), last_link);
	branch_node->execOutPin().linkTo(&forloop_node->execInPin(), last_link);
	forloop_node->execOutPin().linkTo(&return_node->execInPin(), last_link);
	
	auto graph = lux::flowforge::FlowGraph();
	graph.addNodes(std::move(start_node));
	graph.addNodes(std::move(forloop_node));
	graph.addNodes(std::move(branch_node));
	graph.addNodes(std::move(return_node));

	lux::flowforge::MLIRBuilder builder(&ir_context);
	auto ir = builder.generateIR(graph);

	std::cout << ir->toString() << std::endl;

    return 0;
}
