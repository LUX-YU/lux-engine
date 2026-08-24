#pragma once
// ============================================================================
//  PixelDirtyLedger.hpp — 「这块面自上次以来哪里变了」的台账：
//  合并、按预算取批（**延后而不丢弃**）、以及 revision 确认簿记。
//
//  一个台账管一张面（像素场的一个 chunk）。**权威像素归生产者**（CellStorage）：
//  它一边改一边 markDirty，每帧一次：
//
//      auto plan = ledger.takeBatch(texel_bytes, budget);
//      if (!plan.empty()) {
//          <按 plan.rects 的顺序、在各自 data_offset 处紧密打包像素>
//          <交给消费者（今天是渲染子系统，它转成纹理区域上传）>
//          <回复到达时 ledger.onAck(plan.content_revision, ok)>
//      }
//
//  ── 为什么在 ecs/pixel 而不在渲染层 ────────────────────────────────────────
//  它此前叫 `lux::render::RegionUploadPlanner`，住在
//  `modules/function/render/.../resources/ops/`。逐个核实用户之后发现：
//  **唯一的用户就是像素场**，`modules/function/render` 自己一次都没用过。
//  它是纯 CPU 的矩形记账 —— 没有 GPU 类型、没有线协议语义 —— 而「我哪里脏了」
//  本来就是模拟的知识，不是渲染的。放错地方的代价是实打实的：整个 `ecs/pixel`
//  为它链着渲染客户端 SDK，于是「只想跑像素模拟」的上层也得拖上渲染。
//
//  名字也跟着改正：它记的是脏区，不是 "upload" —— 谁来消费、要不要上传、
//  上传到哪张纹理，都是消费者的事。
//
//  ── 契约（逐条保留，行为与改名前逐位相同）──────────────────────────────
//   - 合并：相邻/重叠的矩形，当外接盒相对覆盖面积的浪费 ≤ 25% 时合并 —— 于是
//     散落的小点不会滚成一次不合理的全图更新，而整行相邻的瓦片会塌成几个矩形。
//   - 预算：每次 takeBatch 的字节/矩形上限；装不下的**留着脏**，由后续
//     takeBatch 发出 —— 延后，绝不丢失。单个超大矩形按行切分，于是再小的预算
//     也能推进。
//   - REVISION：每个非空批带一个新的 content_revision，其覆盖范围记为在途；
//     `onAck(revision, true)` 推进 `uploadedRevision()` —— **只有它能推进** ——
//     其余任何情况都把覆盖范围重新置脏，于是一次失败/被拒的入队不可能永久丢数据。
//
//  纯 CPU 逻辑，刻意 header-only、无设备依赖。
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lux::ecs
{
    /// 一块脏区。**只有消费者关心的几何 + 打包偏移** —— mip / array_layer /
    /// row_pitch 是纹理的概念，不在这里（消费者上传时自己补）。
    struct PixelDirtyRect
    {
        std::uint32_t x{0}, y{0};
        std::uint32_t width{0}, height{0};
        /// 本矩形的像素在批的紧密像素块里的字节偏移（由 takeBatch 按发出顺序分配）。
        std::uint32_t data_offset{0};
    };

    /// 每次 takeBatch 的上限。任一字段为 0 = 该维度不限。
    struct PixelExportBudget
    {
        std::uint64_t max_bytes_per_frame{0};
        std::uint32_t max_regions_per_frame{0};
    };

    class PixelDirtyLedger
    {
    public:
        PixelDirtyLedger(std::uint32_t surface_width, std::uint32_t surface_height) noexcept
            : width_(surface_width), height_(surface_height) {}

        /// 标记一块改动过的矩形（面坐标）。会被裁到面内；裁完为空则忽略。
        /// 便宜 —— 合并发生在 takeBatch。
        void markDirty(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h)
        {
            if (x >= width_ || y >= height_) return;
            const std::uint32_t cw = static_cast<std::uint32_t>(std::min<std::uint64_t>(w, width_  - x));
            const std::uint32_t ch = static_cast<std::uint32_t>(std::min<std::uint64_t>(h, height_ - y));
            if (cw == 0 || ch == 0) return;
            dirty_.push_back(Rect{x, y, cw, ch});
        }

        [[nodiscard]] bool hasDirty() const noexcept { return !dirty_.empty(); }

        /// 一帧的批：已合并、已按预算裁剪、偏移按发出顺序紧密分配（按同样的顺序
        /// 打包像素）。空批（没有脏区，或预算小到连一行都切不出）**不**推进
        /// revision，也不留任何在途记录。
        struct Plan
        {
            std::vector<PixelDirtyRect> rects;   ///< data_offset 已分配
            std::uint64_t content_revision{0};
            std::uint64_t pixel_bytes{0};        ///< 要打包的紧密总字节
            [[nodiscard]] bool empty() const noexcept { return rects.empty(); }
        };

        [[nodiscard]] Plan takeBatch(std::uint32_t texel_bytes, const PixelExportBudget& budget)
        {
            Plan plan;
            if (dirty_.empty() || texel_bytes == 0)
                return plan;

            coalesce();

            // 确定的发出顺序（左上优先）—— 也是打包顺序。
            std::sort(dirty_.begin(), dirty_.end(), [](const Rect& a, const Rect& b) {
                return a.y != b.y ? a.y < b.y : (a.x != b.x ? a.x < b.x : a.w < b.w);
            });

            std::vector<Rect> taken;
            std::uint64_t     bytes = 0;
            std::size_t       i     = 0;
            for (; i < dirty_.size(); ++i)
            {
                const Rect&        r       = dirty_[i];
                const std::uint64_t r_bytes = std::uint64_t{r.w} * r.h * texel_bytes;
                const bool byte_room   = budget.max_bytes_per_frame == 0
                                      || bytes + r_bytes <= budget.max_bytes_per_frame;
                const bool region_room = budget.max_regions_per_frame == 0
                                      || taken.size() < budget.max_regions_per_frame;
                if (!region_room)
                    break;
                if (byte_room)
                {
                    taken.push_back(r);
                    bytes += r_bytes;
                    continue;
                }
                // 字节预算触顶。若**一个都还没拿**，把这个矩形按行切开，于是再小的
                // 预算也能推进（延后，绝不饿死）。
                if (taken.empty() && budget.max_bytes_per_frame > 0)
                {
                    const std::uint64_t row_bytes = std::uint64_t{r.w} * texel_bytes;
                    const std::uint32_t rows =
                        row_bytes ? static_cast<std::uint32_t>(
                                        std::min<std::uint64_t>(budget.max_bytes_per_frame / row_bytes, r.h))
                                  : 0u;
                    if (rows > 0)
                    {
                        taken.push_back(Rect{r.x, r.y, r.w, rows});
                        bytes += rows * row_bytes;
                        dirty_[i] = Rect{r.x, r.y + rows, r.w, r.h - rows};   // 余下的留着脏
                    }
                }
                break;
            }
            // 从 i 起的全部留给以后的帧（延后，不是丢弃）；按行切剩的那块就在 i 上。
            dirty_.erase(dirty_.begin(), dirty_.begin() + static_cast<std::ptrdiff_t>(i));

            if (taken.empty())
                return plan;

            plan.content_revision = ++next_revision_;
            plan.rects.reserve(taken.size());
            std::uint64_t offset = 0;
            for (const Rect& r : taken)
            {
                PixelDirtyRect d{};
                d.x = r.x; d.y = r.y; d.width = r.w; d.height = r.h;
                d.data_offset = static_cast<std::uint32_t>(offset);
                plan.rects.push_back(d);
                offset += std::uint64_t{r.w} * r.h * texel_bytes;
            }
            plan.pixel_bytes = offset;

            in_flight_.emplace(plan.content_revision, std::move(taken));
            return plan;
        }

        /// 每个回复都要喂回来。`uploaded == true` 是**唯一**能推进
        /// `uploadedRevision()` 的事；任何拒绝都把这一批的覆盖范围重新置脏，
        /// 于是下一次 takeBatch 会重试它。
        void onAck(std::uint64_t revision, bool uploaded)
        {
            const auto it = in_flight_.find(revision);
            if (it == in_flight_.end())
                return;                                   // 未知/重复的 ack
            if (uploaded)
                uploaded_revision_ = std::max(uploaded_revision_, revision);
            else
                dirty_.insert(dirty_.end(), it->second.begin(), it->second.end());
            in_flight_.erase(it);
        }

        [[nodiscard]] std::uint64_t uploadedRevision() const noexcept { return uploaded_revision_; }

    private:
        struct Rect { std::uint32_t x{0}, y{0}, w{0}, h{0}; };

        static bool touchOrOverlap(const Rect& a, const Rect& b) noexcept
        {
            // 相接也算（共边合成一个拷贝区域）。
            return a.x <= b.x + b.w && b.x <= a.x + a.w &&
                   a.y <= b.y + b.h && b.y <= a.y + a.h;
        }
        static Rect unionOf(const Rect& a, const Rect& b) noexcept
        {
            const std::uint32_t x0 = std::min(a.x, b.x);
            const std::uint32_t y0 = std::min(a.y, b.y);
            const std::uint32_t x1 = std::max(a.x + a.w, b.x + b.w);
            const std::uint32_t y1 = std::max(a.y + a.h, b.y + b.h);
            return Rect{x0, y0, x1 - x0, y1 - y0};
        }
        static std::uint64_t area(const Rect& r) noexcept { return std::uint64_t{r.w} * r.h; }
        static std::uint64_t overlapArea(const Rect& a, const Rect& b) noexcept
        {
            const std::uint64_t x0 = std::max(a.x, b.x), x1 = std::min(a.x + a.w, b.x + b.w);
            const std::uint64_t y0 = std::max(a.y, b.y), y1 = std::min(a.y + a.h, b.y + b.h);
            return (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) : 0;
        }

        /// 两两的定点合并，带 25% 浪费上界：相接/重叠、且外接盒面积不超过覆盖面积
        /// 5/4 的那些塌成一个。**绝不**合并相距很远的两块（它们的外接盒会爆掉上界），
        /// 于是对角两个小脏点不可能变成一次全图更新。
        void coalesce()
        {
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (std::size_t a = 0; a < dirty_.size() && !merged; ++a)
                    for (std::size_t b = a + 1; b < dirty_.size() && !merged; ++b)
                    {
                        if (!touchOrOverlap(dirty_[a], dirty_[b]))
                            continue;
                        const Rect u = unionOf(dirty_[a], dirty_[b]);
                        const std::uint64_t covered =
                            area(dirty_[a]) + area(dirty_[b]) - overlapArea(dirty_[a], dirty_[b]);
                        if (area(u) * 4 <= covered * 5)   // 浪费 ≤ 25%
                        {
                            dirty_[a] = u;
                            dirty_[b] = dirty_.back();
                            dirty_.pop_back();
                            merged = true;
                        }
                    }
            }
        }

        std::uint32_t width_{0};
        std::uint32_t height_{0};
        std::vector<Rect> dirty_;
        std::unordered_map<std::uint64_t, std::vector<Rect>> in_flight_;   ///< revision → 覆盖范围
        std::uint64_t next_revision_{0};
        std::uint64_t uploaded_revision_{0};
    };

} // namespace lux::ecs
