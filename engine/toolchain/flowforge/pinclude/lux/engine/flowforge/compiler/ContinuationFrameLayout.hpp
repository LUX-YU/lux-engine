#pragma once
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <span>
#include <limits>
#include <optional>
#include <vector>

namespace lux::flowforge::detail
{
    // Queries incoming-value liveness on the actual CFG after resume edges and PHI lowering.
    // Worklists preserve CFG order; membership sets never determine layout or cooked hashes.
    class ContinuationFrameLayout final
    {
    public:
        struct Interval final
        {
            std::size_t first{}, last{};
        };

        // A conservative linear envelope of CFG liveness, including every write. Unknown aliases
        // never share storage. Including dead stores prevents them clobbering another live value.
        [[nodiscard]] static std::optional<Interval> storageInterval(llvm::AllocaInst& slot)
        {
            const auto* count = llvm::dyn_cast<llvm::ConstantInt>(slot.getArraySize());
            if (!count || !count->isOne()) return std::nullopt;
            for (auto* user : slot.users())
            {
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (load->isVolatile() || load->isAtomic()) return std::nullopt;
                }
                else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand() != &slot || store->isVolatile() || store->isAtomic())
                        return std::nullopt;
                }
                else return std::nullopt;
            }
            struct Block final
            {
                llvm::BasicBlock* block;
                std::size_t first, last;
                bool uses{}, defines{}, live_in{}, live_out{};
            };
            std::vector<Block> blocks;
            Interval result{(std::numeric_limits<std::size_t>::max)(), 0U};
            std::size_t position{};
            for (auto& block : *slot.getFunction())
            {
                Block item{&block, position, position};
                for (auto& instruction : block)
                {
                    const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                    const bool reads = load && load->getPointerOperand() == &slot;
                    const bool writes = store && store->getPointerOperand() == &slot;
                    if (reads && !item.defines) item.uses = true;
                    if (writes) item.defines = true;
                    if (reads || writes)
                    {
                        result.first = (std::min)(result.first, position);
                        result.last = (std::max)(result.last, position);
                    }
                    item.last = position++;
                }
                blocks.push_back(item);
            }
            bool changed;
            do
            {
                changed = false;
                for (auto& item : blocks)
                {
                    bool live_out{};
                    for (auto* successor : llvm::successors(item.block))
                        for (const auto& candidate : blocks)
                            if (candidate.block == successor) live_out |= candidate.live_in;
                    const bool live_in = item.uses || (live_out && !item.defines);
                    changed |= live_in != item.live_in || live_out != item.live_out;
                    item.live_in = live_in;
                    item.live_out = live_out;
                }
            } while (changed);
            for (const auto& item : blocks)
            {
                if (item.live_in) result.first = (std::min)(result.first, item.first);
                if (item.live_out) result.last = (std::max)(result.last, item.last);
            }
            return result.first <= result.last ? std::optional{result} : std::nullopt;
        }

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
