// ============================================================================
//  submission_merge_test.cpp
//
//  GPU-free unit test for SubmissionMerger (FrameSubmitMerge.hpp), the per-view
//  multi-queue submission merge extracted from FrameDriver::endFrame (batch 7).
//
//  Pins the behavior that used to be buried in endFrame's hot path:
//    - submissions with the SAME (queue_type, cmd) collapse into ONE entry, with
//      their wait/signal semaphore lists concatenated in input order;
//    - distinct (queue_type, cmd) pairs stay separate, in first-seen order;
//    - null infos, non-multi-queue infos, and null command buffers are skipped;
//    - the reused scratch is cleared between merge() calls (no accumulation).
//
//  Uses opaque sentinel handles — merge() never dereferences them — so no Vulkan
//  device is required. Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/renderer/FrameSubmitMerge.hpp>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdint>
#include <utility>
#include <vector>

using namespace lux::render;

static int g_fail = 0;
static void check(bool cond, const char* name)
{
    std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name);
    if (!cond) ++g_fail;
}

static VkCommandBuffer cb(uintptr_t v)  { return reinterpret_cast<VkCommandBuffer>(v); }
static VkSemaphore     sem(uintptr_t v) { return reinterpret_cast<VkSemaphore>(v); }

static VkSemaphoreSubmitInfo semInfo(uintptr_t s)
{
    VkSemaphoreSubmitInfo i{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    i.semaphore = sem(s);
    return i;
}

static RGQueueSubmission sub(ERGQueueType q, uintptr_t cmd,
                             std::vector<VkSemaphoreSubmitInfo> waits,
                             std::vector<VkSemaphoreSubmitInfo> signals)
{
    RGQueueSubmission s;
    s.queue_type       = q;
    s.cmd              = cb(cmd);
    s.wait_semaphores  = std::move(waits);
    s.signal_semaphores = std::move(signals);
    return s;
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== submission_merge_test ===\n");

    SubmissionMerger merger;

    // ── Case 1: same (queue,cmd) merges + concatenates; distinct stays separate ──
    {
        RGMultiQueueSubmitInfo a;
        a.is_multi_queue = true;
        a.submissions.push_back(sub(ERGQueueType::GRAPHICS, 0x100, {semInfo(0x1)}, {semInfo(0x9)}));
        a.submissions.push_back(sub(ERGQueueType::COMPUTE,  0x200, {semInfo(0x2)}, {}));

        RGMultiQueueSubmitInfo b;
        b.is_multi_queue = true;
        // same (GRAPHICS, 0x100) as a.submissions[0] -> merges into entry 0
        b.submissions.push_back(sub(ERGQueueType::GRAPHICS, 0x100, {semInfo(0x3)}, {semInfo(0xA)}));

        std::vector<const RGMultiQueueSubmitInfo*> in{&a, &b};
        const std::vector<RGQueueSubmission>& merged = merger.merge(in);

        check(merged.size() == 2, "same (queue,cmd) merged -> 2 unique submissions");
        check(merged[0].queue_type == ERGQueueType::GRAPHICS && merged[0].cmd == cb(0x100),
              "entry 0 keeps the first-seen (queue,cmd) = GRAPHICS/0x100");
        check(merged[0].wait_semaphores.size() == 2 && merged[0].signal_semaphores.size() == 2,
              "merged entry concatenates wait + signal semaphores");
        check(merged[0].wait_semaphores[0].semaphore == sem(0x1)
           && merged[0].wait_semaphores[1].semaphore == sem(0x3),
              "wait semaphores concatenated in input order");
        check(merged[1].queue_type == ERGQueueType::COMPUTE && merged[1].cmd == cb(0x200),
              "entry 1 = distinct (COMPUTE,0x200) kept separate, in order");
    }

    // ── Case 2: skip rules (null info, non-multi-queue info, null cmd) ──────────
    {
        RGMultiQueueSubmitInfo good;
        good.is_multi_queue = true;
        good.submissions.push_back(sub(ERGQueueType::GRAPHICS, 0x100, {}, {}));
        good.submissions.push_back(sub(ERGQueueType::GRAPHICS, 0,     {}, {})); // null cmd -> skipped

        RGMultiQueueSubmitInfo not_mq;
        not_mq.is_multi_queue = false; // whole info skipped
        not_mq.submissions.push_back(sub(ERGQueueType::COMPUTE, 0x300, {}, {}));

        std::vector<const RGMultiQueueSubmitInfo*> in{nullptr, &not_mq, &good};
        const std::vector<RGQueueSubmission>& merged = merger.merge(in);

        check(merged.size() == 1, "null info + non-multi-queue info + null-cmd sub all skipped -> 1");
        check(merged[0].cmd == cb(0x100), "only the valid submission survives");
    }

    // ── Case 3: scratch reuse — a second merge() clears prior results ──────────
    {
        RGMultiQueueSubmitInfo a; a.is_multi_queue = true;
        a.submissions.push_back(sub(ERGQueueType::GRAPHICS, 0x111, {}, {}));
        std::vector<const RGMultiQueueSubmitInfo*> in1{&a};
        check(merger.merge(in1).size() == 1, "reuse: first merge -> 1");

        RGMultiQueueSubmitInfo b; b.is_multi_queue = true;
        b.submissions.push_back(sub(ERGQueueType::COMPUTE,  0x222, {}, {}));
        b.submissions.push_back(sub(ERGQueueType::TRANSFER, 0x333, {}, {}));
        std::vector<const RGMultiQueueSubmitInfo*> in2{&b};
        const std::vector<RGQueueSubmission>& m2 = merger.merge(in2);
        check(m2.size() == 2, "reuse: second merge clears prior -> 2 (not 3)");
        check(m2[0].cmd == cb(0x222) && m2[1].cmd == cb(0x333),
              "reuse: second merge holds only the new submissions");
    }

    // ── Case 4: empty input -> empty result ────────────────────────────────────
    {
        std::vector<const RGMultiQueueSubmitInfo*> in;
        check(merger.merge(in).empty(), "empty input -> empty merged");
    }

    std::printf("=== submission_merge_test %s (fails=%d) ===\n",
                g_fail == 0 ? "PASSED" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
