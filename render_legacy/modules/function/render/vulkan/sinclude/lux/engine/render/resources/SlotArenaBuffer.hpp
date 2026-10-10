#pragma once
/**
 * @file SlotArenaBuffer.hpp
 * @brief 一条大 GPU 缓冲 + 变长槽位的 freelist 竞技场,按元素类型参数化。
 *
 * 收敛自 TrajectoryGlobalBuffer 与 PointCloudGlobalBuffer —— 两者曾是同一份实现
 * 的两次改名抄写(约 69% 的行逐字相同,私有实现签名一字不差),
 * TrajectoryGlobalBuffer 的文件头注释自己就写着 "analogous to
 * PointCloudGlobalBuffer"。
 *
 * 为什么值得收编,不只是省行数:**freelist 与 growBuffer 是 RAW 危险区**。
 * 槽位扩容时若既换缓冲又搬数据,搬运拷贝会与"整缓冲 old→new"拷贝同优先级竞争
 * (两条 priority=-1 之间无屏障),读到还没写入的字节。轨迹侧为此有一个
 * pre_grow_buffer 修复(#26);点云侧因为扩容即丢弃数据而不需要它。两份实现并存
 * 时,点云一旦改成"扩容保留数据"就得把那个修复重新发现一遍。归一之后,
 * 该修复住在**派生类各自的 ensureSlotCapacity 里**(那才是策略分歧所在),
 * 而竞技场机制只有一份。
 *
 * 分工:
 *   - 本模板:缓冲创建/销毁、freelist 分配与相邻合并、槽位表、按槽上传、
 *     整缓冲扩容(allocOrGrow / growBuffer)。
 *   - 派生类:ensureSlotCapacity 的**策略**(见下)、领域专属操作、领域命名访问器。
 *
 * 为什么 ensureSlotCapacity 不进基类:两个消费方的语义**真的不同** ——
 *   轨迹:扩容保留旧数据(append 语义),初始容量有 64 下限,按 2 倍增长;
 *   点云:扩容丢弃旧数据(整块 replace 语义),容量精确等于请求值。
 * 把它塞进基类只能靠布尔/策略参数区分"要不要搬数据",而那正是抽象开始说谎的地方。
 *
 * 线程:全部方法仅渲染线程。
 */

#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/render/core/FrameRetireScheduler.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace lux::render
{
    /// @tparam ElemT 槽位里的元素类型(顶点 / 点),只用于 sizeof 与 span 类型。
    template <typename ElemT> class TSlotArenaBuffer
    {
    public:
        /// 一个槽位在竞技场里的位置。字段用中性名:两个领域曾各自叫
        /// first_vertex/vertex_count 与 first_point/point_count,同一个概念两套名字。
        struct Slot
        {
            uint32_t first{0};    ///< 起始元素下标
            uint32_t capacity{0}; ///< 已分配容量(>= count)
            uint32_t count{0};    ///< 实际存活元素数(供绘制用)
        };

        /// "无此 id" / "分配失败" 的哨兵。
        static constexpr uint32_t kInvalidId = ~uint32_t{0};

        ~TSlotArenaBuffer()
        {
            // Each nonmovable arena cancels only its own freelist callbacks.
            // Other resources in the same scene retain their retirement responsibilities.
            retire_scheduler_.purge(ownerToken());
        }

        TSlotArenaBuffer(const TSlotArenaBuffer&) = delete;
        TSlotArenaBuffer& operator=(const TSlotArenaBuffer&) = delete;
        TSlotArenaBuffer(TSlotArenaBuffer&&) = delete;
        TSlotArenaBuffer& operator=(TSlotArenaBuffer&&) = delete;

        // ── 槽位管理(仅渲染线程) ───────────────────────────────────────

        /// 为 @p id 预留 @p capacity 个元素。不触发整缓冲扩容。
        /// @return 成功 true;缓冲已满返回 false;id 已存在直接 true。
        bool reserveSlot(uint32_t id, uint32_t capacity)
        {
            if (capacity == 0)
            {
                return false;
            }
            if (slots_.contains(id))
            {
                return true;
            }

            const uint32_t first = tryAllocFromFreeList(capacity);
            if (first == kInvalidId)
            {
                return false;
            }

            slots_.insert(id, Slot{first, capacity, 0});
            return true;
        }

        /// 释放一个槽位。区间的归还**延迟**到 GPU 越过当前 serial ——
        /// 缓冲是 GPU_ONLY 持久缓冲,在飞帧可能还在读这段偏移;立即归还会让
        /// 同一轮 free-then-upload 复用该区间并 vkCmdCopyBuffer 覆盖在读数据。
        void freeSlot(uint32_t id)
        {
            if (!slots_.contains(id))
            {
                return;
            }
            const auto slot = slots_.at(id);
            (void)slots_.erase(id);
            deferReturn(slot.first, slot.capacity);
        }

        /// 释放全部槽位(逐个走与 freeSlot 相同的延迟归还口径)。
        void freeAllSlots()
        {
            const auto& vals = slots_.values();
            const std::size_t n = vals.size();
            const auto serial = buffer_owned_.queue().currentSerial();
            for (std::size_t i = 0; i < n; ++i)
            {
                retire_scheduler_.defer(
                    serial,
                    ownerToken(),
                    [this, first = vals[i].first, cap = vals[i].capacity] { returnToFreeList(first, cap); }
                );
            }
            slots_.clear();
        }

        /// 把存活计数清零,保留槽位分配(无需 GPU 工作)。
        void resetSlot(uint32_t id) noexcept
        {
            if (!slots_.contains(id))
            {
                return;
            }
            slots_.at(id).count = 0;
        }

        /// 直接改存活计数(不上传数据)。钳到容量上限:曾有一侧钳、一侧不钳,
        /// 不钳的那侧能把 count 设到容量之外,绘制时就是越界读。
        void setCount(uint32_t id, uint32_t count) noexcept
        {
            if (!slots_.contains(id))
            {
                return;
            }
            auto& slot = slots_.at(id);
            slot.count = std::min(count, slot.capacity);
        }

        // ── 查询 ────────────────────────────────────────────────────────

        [[nodiscard]] std::optional<Slot> getSlot(uint32_t id) const noexcept
        {
            if (!slots_.contains(id))
            {
                return std::nullopt;
            }
            return slots_.at(id);
        }

        [[nodiscard]] bool hasSlot(uint32_t id) const noexcept
        {
            return slots_.contains(id);
        }

        [[nodiscard]] VkBuffer buffer() const noexcept
        {
            return buffer_owned_.get();
        }

        [[nodiscard]] uint32_t maxElements() const noexcept
        {
            return max_elements_;
        }

        [[nodiscard]] uint32_t usedElements() const noexcept
        {
            uint32_t free = 0;
            for (const auto& r : free_list_)
            {
                free += r.capacity;
            }
            return max_elements_ - free;
        }

        /// 遍历全部活跃槽位。回调:void fn(uint32_t id, const Slot&)
        template <typename Fn> void forEachSlot(Fn&& fn) const
        {
            const auto& keys = slots_.keys();
            const auto& values = slots_.values();
            const std::size_t n = std::min(keys.size(), values.size());
            for (std::size_t i = 0; i < n; ++i)
            {
                fn(keys[i], values[i]);
            }
        }

        // ── 上传 ────────────────────────────────────────────────────────

        /// 覆盖写入一个已存在的槽位,并把 count 设为 data.size()。
        bool upload(uint32_t id, std::span<const ElemT> data, TransferScheduler& scheduler)
        {
            if (data.empty())
            {
                return false;
            }
            if (!slots_.contains(id))
            {
                return false;
            }

            Slot& slot = slots_.at(id);
            const uint32_t count = static_cast<uint32_t>(data.size());
            assert(count <= slot.capacity && "Upload exceeds allocated slot capacity");
            // Release 兜底:扩容失败过(VMA OOM 或 uint32 天花板)时上面的 assert 被
            // 编译掉,这段拷贝就会溢出本槽、写进邻居槽 —— 尾槽甚至越出整条缓冲
            // (验证层报错 / 设备丢失)。宁可丢掉这次上传。(C-2)
            if (count > slot.capacity)
            {
                return false;
            }

            const VkDeviceSize byte_offset = elemBytes(slot.first);
            const VkDeviceSize byte_size = elemBytes(count);

            StagingAlloc stg = scheduler.allocateStaging(byte_size);
            if (!stg.mapped)
            {
                return false;
            }

            std::memcpy(stg.mapped, data.data(), byte_size);
            scheduler.submitBufferCopy({
                .src = stg.buffer,
                .src_offset = stg.srcOffset,
                .dst = buffer(),
                .dst_offset = byte_offset,
                .size = byte_size,
                .domain = EBufferDomain::VERTEX_INPUT_CS,
                .priority = 0,
            });

            slot.count = count;
            return true;
        }

    protected:
        struct FreeRegion
        {
            uint32_t first;
            uint32_t capacity;
        };

        [[nodiscard]] static constexpr VkDeviceSize elemBytes(uint32_t n) noexcept
        {
            return static_cast<VkDeviceSize>(n) * sizeof(ElemT);
        }

        /// 首次适配:走一遍 free_list_ 找够大的区间。
        uint32_t tryAllocFromFreeList(uint32_t capacity)
        {
            for (auto it = free_list_.begin(); it != free_list_.end(); ++it)
            {
                if (it->capacity >= capacity)
                {
                    const uint32_t first = it->first;
                    if (it->capacity == capacity)
                    {
                        free_list_.erase(it);
                    }
                    else
                    {
                        it->first += capacity;
                        it->capacity -= capacity;
                    }
                    return first;
                }
            }
            return kInvalidId;
        }

        /// 按起始偏移有序插入,并与前后相邻区间合并。
        void returnToFreeList(uint32_t first, uint32_t capacity)
        {
            auto pos = std::lower_bound(
                free_list_.begin(),
                free_list_.end(),
                first,
                [](const FreeRegion& r, uint32_t v) { return r.first < v; }
            );
            auto it = free_list_.insert(pos, {first, capacity});

            auto next = std::next(it);
            if (next != free_list_.end() && it->first + it->capacity == next->first)
            {
                it->capacity += next->capacity;
                free_list_.erase(next);
            }
            if (it != free_list_.begin())
            {
                auto prev = std::prev(it);
                if (prev->first + prev->capacity == it->first)
                {
                    prev->capacity += it->capacity;
                    free_list_.erase(it);
                }
            }
        }

        /// 延迟归还一个区间(见 freeSlot 的注释)。
        void deferReturn(uint32_t first, uint32_t capacity)
        {
            retire_scheduler_.defer(
                buffer_owned_.queue().currentSerial(),
                ownerToken(),
                [this, first, capacity] { returnToFreeList(first, capacity); }
            );
        }

        /// 先试 freelist;不够就整缓冲扩容再试一次。
        uint32_t allocOrGrow(uint32_t capacity, TransferScheduler& scheduler)
        {
            const uint32_t first = tryAllocFromFreeList(capacity);
            if (first != kInvalidId)
            {
                return first;
            }
            if (!growBuffer(capacity, scheduler))
            {
                return kInvalidId;
            }
            return tryAllocFromFreeList(capacity);
        }

        /// 整缓冲扩容:建新缓冲、提交 old→new 整体拷贝(priority=-1,调度器的
        /// 中段屏障保证它先于 priority=0 的数据拷贝完成)、退休旧缓冲、把新增
        /// 尾部并入 freelist。
        bool growBuffer(uint32_t required_capacity, TransferScheduler& scheduler)
        {
            if (required_capacity == 0)
            {
                return false;
            }

            constexpr uint64_t kU32Max = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
            uint64_t new_capacity = static_cast<uint64_t>(max_elements_);
            const uint64_t min_required = static_cast<uint64_t>(required_capacity);

            while (new_capacity < min_required)
            {
                const uint64_t doubled = new_capacity * 2ull;
                if (doubled <= new_capacity)
                {
                    new_capacity = kU32Max;
                    break;
                }
                new_capacity = doubled;
            }

            if (new_capacity == static_cast<uint64_t>(max_elements_))
            {
                new_capacity = std::min<uint64_t>(
                    kU32Max,
                    std::max<uint64_t>(
                        static_cast<uint64_t>(max_elements_) * 2ull,
                        static_cast<uint64_t>(max_elements_) + min_required
                    )
                );
            }

            if (new_capacity > kU32Max)
            {
                new_capacity = kU32Max;
            }
            if (new_capacity <= static_cast<uint64_t>(max_elements_))
            {
                return false;
            }

            auto candidate = prepareBacking(allocator_, static_cast<uint32_t>(new_capacity));
            if (!candidate)
            {
                return false;
            }
            scheduler.submitBufferCopy({
                .src = buffer(),
                .src_offset = 0,
                .dst = candidate->buffer(),
                .dst_offset = 0,
                .size = elemBytes(max_elements_),
                .domain = EBufferDomain::TRANSFER_DST,
                .priority = -1,
            });

            const auto allocation = candidate->release();
            buffer_owned_ =
                TFifOwnedAllocated<VkBuffer>{buffer_owned_.queue(), allocation.buffer, allocation.allocation};
            returnToFreeList(max_elements_, static_cast<uint32_t>(new_capacity) - max_elements_);
            max_elements_ = static_cast<uint32_t>(new_capacity);
            return true;
        }

        /// VERTEX(简单绘制)| STORAGE(GPU-driven / LOD 的 SSBO 路径)
        /// | TRANSFER_DST(暂存目标)| TRANSFER_SRC(扩容拷贝源)
        static Expected<VmaBuffer> prepareBacking(VmaAllocator allocator, uint32_t elements) noexcept
        {
            const bool is_invalid_configuration = !allocator || elements == 0;
            if (is_invalid_configuration)
            {
                return renderFailure<err::memory::InvalidBufferConfiguration>();
            }
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = elemBytes(elements);
            bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            return VmaBuffer::create(allocator, bci, aci);
        }

        TSlotArenaBuffer(
            DeferredDestroyQueue& retirement,
            FrameRetireScheduler& callbacks,
            uint32_t capacity,
            VmaBuffer::Allocation allocation
        ) noexcept
            : allocator_(allocation.allocator), buffer_owned_(retirement, allocation.buffer, allocation.allocation),
              max_elements_(capacity), free_list_{{0, capacity}}, retire_scheduler_(callbacks)
        {
        }

        [[nodiscard]] FrameRetireScheduler::OwnerToken ownerToken() const noexcept
        {
            return static_cast<FrameRetireScheduler::OwnerToken>(reinterpret_cast<std::uintptr_t>(this));
        }

        VmaAllocator allocator_;
        TFifOwnedAllocated<VkBuffer> buffer_owned_;
        uint32_t max_elements_;

        lux::cxx::OffsetSparseSet<uint32_t, Slot> slots_;
        std::vector<FreeRegion> free_list_; ///< 按 first 有序
        FrameRetireScheduler& retire_scheduler_;
    };

} // namespace lux::render
