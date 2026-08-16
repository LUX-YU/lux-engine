#pragma once
// ============================================================================
//  FixedStepConfig.hpp — the shared fixed-step accumulator's parameters
//  (lux::ecs).
//
//  Extracted from D2ScenePlan for the SAME reason Physics2DConfig was
//  (见 Physics2DConfig.hpp):**拥有这套词汇的是消费它的那一层,plan 只是携带**。
//  消费者是 `Simulation2DSystem`(ecs/physics),所以配置跟着它走 ——
//  `ecs/scene → ecs/physics` 是单向依赖,反向永远不成立。
//
//  每一个固定步能力(像素仿真、物理)共用**同一个**累加器(2D pack 设计 §2.4),
//  所以这是场景级配置而非每系统配置。
// ============================================================================

namespace lux::ecs
{
    struct FixedStepConfig
    {
        float fixed_dt        = 1.0f / 60.0f;  ///< the fixed timestep
        int   max_substeps    = 4;             ///< spiral-of-death guard (cap substeps/frame)
        float max_accumulated = 0.25f;         ///< clamp on banked time (long-stall catch-up guard)
        /// On hitting max_substeps with time still banked, DISCARD the backlog
        /// (fmod) rather than carrying it — the only MVP drop policy. Kept as a
        /// field so a future "catch-up" variant is a config change, not a rewrite.
        bool  drop_excess_time = true;
    };

} // namespace lux::ecs
