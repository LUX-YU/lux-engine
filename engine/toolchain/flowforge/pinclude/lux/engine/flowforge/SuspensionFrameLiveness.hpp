#pragma once

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

#include <cstddef>
#include <vector>

namespace lux::flowforge::detail
{
    struct SuspensionFrameLiveness final
    {
        llvm::BitVector persistent;
        llvm::BitVector arguments;
        llvm::BitVector escaped;
        std::vector<llvm::BitVector> conflicts;
    };

    // Run after scalar/phi demotion but before frame materialization. Stores kill only a complete object;
    // unknown aliasing is conservative. Compiler temporaries are not retained merely because an entry awaits.
    inline SuspensionFrameLiveness analyzeSuspensionFrame(
        llvm::Function& function, llvm::ArrayRef<llvm::AllocaInst*> objects,
        llvm::ArrayRef<llvm::CallInst*> awaits, unsigned original_argument_count
    )
    {
        struct Access final
        {
            llvm::SmallVector<unsigned, 2> reads;
            llvm::SmallVector<unsigned, 2> writes;
        };
        struct Block final
        {
            llvm::BasicBlock* block{};
            llvm::BitVector gen;
            llvm::BitVector kill;
            llvm::BitVector incoming;
            llvm::BitVector outgoing;
        };
        const auto object_count = static_cast<unsigned>(objects.size());
        const auto count = object_count + original_argument_count;
        SuspensionFrameLiveness result{
            llvm::BitVector(object_count), llvm::BitVector(original_argument_count), llvm::BitVector(object_count),
            std::vector<llvm::BitVector>(object_count, llvm::BitVector(object_count))
        };
        llvm::SmallPtrSet<llvm::Instruction*, 16> boundaries;
        for (auto* marker : awaits)
            boundaries.insert(marker);
        llvm::DenseMap<llvm::Instruction*, Access> access;
        for (unsigned index{}; index < object_count; ++index)
        {
            auto* object = objects[index];
            llvm::SmallVector<llvm::Value*, 8> pending{object};
            llvm::SmallPtrSet<llvm::Value*, 16> visited;
            while (!pending.empty())
            {
                auto* pointer = pending.pop_back_val();
                if (!visited.insert(pointer).second)
                    continue;
                for (auto* raw_user : pointer->users())
                {
                    auto* user = llvm::dyn_cast<llvm::Instruction>(raw_user);
                    if (user == nullptr)
                    {
                        result.escaped.set(index);
                        continue;
                    }
                    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        access[load].reads.push_back(index);
                        continue;
                    }
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand() == object &&
                            store->getValueOperand()->getType() == object->getAllocatedType())
                            access[store].writes.push_back(index);
                        else
                            access[store].reads.push_back(index);
                        if (store->getValueOperand() == pointer)
                            result.escaped.set(index);
                        continue;
                    }
                    const bool is_derived = llvm::isa<llvm::GetElementPtrInst>(user) ||
                        llvm::isa<llvm::BitCastInst>(user) || llvm::isa<llvm::AddrSpaceCastInst>(user) ||
                        llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::SelectInst>(user);
                    if (is_derived)
                    {
                        pending.push_back(user);
                        continue;
                    }
                    if (auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                    {
                        const auto id = intrinsic->getIntrinsicID();
                        if (id == llvm::Intrinsic::lifetime_start || id == llvm::Intrinsic::lifetime_end ||
                            llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic))
                            continue;
                        if (llvm::isa<llvm::MemIntrinsic>(intrinsic))
                        {
                            access[intrinsic].reads.push_back(index);
                            continue;
                        }
                    }
                    access[user].reads.push_back(index);
                    // Canonical async inputs are consumed during start, not retained by the operation.
                    if (!boundaries.contains(user))
                        result.escaped.set(index);
                }
            }
        }
        for (unsigned index{}; index < original_argument_count; ++index)
            for (auto* user : function.getArg(index)->users())
                if (auto* instruction = llvm::dyn_cast<llvm::Instruction>(user))
                    access[instruction].reads.push_back(object_count + index);

        llvm::DenseMap<llvm::BasicBlock*, std::size_t> indices;
        std::vector<Block> blocks;
        for (auto& block : function)
        {
            indices[&block] = blocks.size();
            blocks.push_back({&block, llvm::BitVector(count), llvm::BitVector(count),
                llvm::BitVector(count), llvm::BitVector(count)});
            auto& entry = blocks.back();
            for (auto& instruction : block)
            {
                const auto found = access.find(&instruction);
                if (found == access.end())
                    continue;
                for (const auto read : found->second.reads)
                    if (!entry.kill.test(read))
                        entry.gen.set(read);
                for (const auto write : found->second.writes)
                    entry.kill.set(write);
            }
        }
        bool changed{};
        do
        {
            changed = false;
            for (auto iterator = blocks.rbegin(); iterator != blocks.rend(); ++iterator)
            {
                auto& block = *iterator;
                llvm::BitVector outgoing(count);
                for (auto* successor : llvm::successors(block.block))
                    outgoing |= blocks[indices.lookup(successor)].incoming;
                auto incoming = outgoing;
                incoming.reset(block.kill);
                incoming |= block.gen;
                changed |= incoming != block.incoming;
                block.incoming = std::move(incoming);
                block.outgoing = std::move(outgoing);
            }
        } while (changed);

        const auto transfer = [&](llvm::Instruction& instruction, llvm::BitVector& live) {
            const auto found = access.find(&instruction);
            if (found == access.end())
                return;
            for (const auto write : found->second.writes)
                live.reset(write);
            for (const auto read : found->second.reads)
                live.set(read);
        };
        for (auto& block : blocks)
        {
            auto live = block.outgoing;
            for (auto iterator = block.block->rbegin(); iterator != block.block->rend(); ++iterator)
            {
                if (boundaries.contains(&*iterator))
                    for (int bit = live.find_first(); bit >= 0; bit = live.find_next(bit))
                        if (static_cast<unsigned>(bit) < object_count)
                            result.persistent.set(bit);
                        else
                            result.arguments.set(static_cast<unsigned>(bit) - object_count);
                transfer(*iterator, live);
            }
        }
        result.persistent |= result.escaped;
        for (auto& block : blocks)
        {
            auto live = block.outgoing;
            const auto record = [&] {
                llvm::BitVector active(object_count);
                for (int bit = result.persistent.find_first(); bit >= 0; bit = result.persistent.find_next(bit))
                    if (live.test(bit))
                        active.set(bit);
                for (int bit = active.find_first(); bit >= 0; bit = active.find_next(bit))
                    result.conflicts[bit] |= active;
            };
            record();
            for (auto iterator = block.block->rbegin(); iterator != block.block->rend(); ++iterator)
            {
                transfer(*iterator, live);
                record();
            }
        }
        for (int escaped = result.escaped.find_first(); escaped >= 0; escaped = result.escaped.find_next(escaped))
            for (int bit = result.persistent.find_first(); bit >= 0; bit = result.persistent.find_next(bit))
            {
                result.conflicts[escaped].set(bit);
                result.conflicts[bit].set(escaped);
            }
        return result;
    }
}
