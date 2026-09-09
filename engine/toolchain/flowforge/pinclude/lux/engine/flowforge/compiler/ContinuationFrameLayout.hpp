#pragma once
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <span>

namespace lux::flowforge::detail
{
    // Queries incoming-value liveness on the actual CFG after resume edges and PHI lowering.
    // Worklists preserve CFG order; membership sets never determine layout or cooked hashes.
    class ContinuationFrameLayout final
    {
    public:
        [[nodiscard]] static bool readsIncoming(llvm::AllocaInst& slot, llvm::BasicBlock* entry)
        {
            llvm::SmallPtrSet<llvm::Value*, 16> addresses;
            llvm::SmallVector<llvm::Value*, 16> aliases{&slot};
            addresses.insert(&slot);
            while (!aliases.empty())
            {
                auto* address = aliases.pop_back_val();
                for (auto* user : address->users())
                {
                    if (llvm::isa<llvm::GetElementPtrInst, llvm::BitCastInst>(user))
                    {
                        if (addresses.insert(user).second) aliases.push_back(user);
                    }
                    else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getValueOperand() == address) return true; // Address escapes.
                    }
                    else if (!llvm::isa<llvm::LoadInst, llvm::DbgInfoIntrinsic, llvm::LifetimeIntrinsic>(user))
                        return true; // Unknown aliasing/call contracts remain conservative.
                }
            }
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> visited;
            llvm::SmallVector<llvm::BasicBlock*, 16> work{entry};
            while (!work.empty())
            {
                auto* block = work.pop_back_val();
                if (!visited.insert(block).second) continue;
                bool killed{};
                for (auto& instruction : *block)
                {
                    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                        if (addresses.contains(load->getPointerOperand())) return true;
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        const bool full_store = store->getPointerOperand() == &slot &&
                            store->getValueOperand()->getType() == slot.getAllocatedType() &&
                            llvm::cast<llvm::ConstantInt>(slot.getArraySize())->isOne();
                        if (full_store)
                        {
                            killed = true;
                            break;
                        }
                    }
                }
                if (!killed)
                    for (auto* successor : llvm::successors(block)) work.push_back(successor);
            }
            return false;
        }
        [[nodiscard]] static bool persists(llvm::AllocaInst& slot, std::span<llvm::BasicBlock* const> resumes)
        {
            for (auto* entry : resumes) if (readsIncoming(slot, entry)) return true;
            return false;
        }
        [[nodiscard]] static bool argumentPersists(llvm::Argument& argument,
            std::span<llvm::BasicBlock* const> resumes)
        {
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> visited;
            llvm::SmallVector<llvm::BasicBlock*, 16> work(resumes.begin(), resumes.end());
            while (!work.empty())
            {
                auto* block = work.pop_back_val();
                if (!visited.insert(block).second) continue;
                for (auto* user : argument.users())
                    if (auto* instruction = llvm::dyn_cast<llvm::Instruction>(user))
                        if (instruction->getParent() == block) return true;
                for (auto* successor : llvm::successors(block)) work.push_back(successor);
            }
            return false;
        }
    };
}
