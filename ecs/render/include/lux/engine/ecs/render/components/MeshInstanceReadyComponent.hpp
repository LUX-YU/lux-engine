#pragma once
// ============================================================================
//  MeshInstanceReadyComponent.hpp — 「这个实体的 GPU 网格实例已就绪且对当前
//  view 代次可见」(lux::ecs)。
//
//  由 `MeshInstanceSubsystem` 维护的**运行期观察点**(与 MeshGpuCacheComponent
//  同类,故意**不进反射清单**:纯运行时状态,绝不序列化、不进属性面板):
//
//    · emplace —— 实例的 addMeshInstance 回复已落地(slot 建成),且已对
//      **当前** view 代次发过 makeInstanceVisibleForView 之后;
//    · remove  —— 实例被拆(资产换源导致的 remove+重建)或离场
//      (组件被摘 / 实体销毁 / 获得 Exclude 标签)时。
//
//  用途:缩略图 readback 的时序就绪信号 —— ThumbnailService 轮询 job 实体
//  「都长出这个组件」后才发 readbackTargetAsync,否则抓到的是实例还没画进去
//  的空图(不报错,只是黑图)。任何「等这批实体真的在画了」的宿主逻辑都可以
//  轮询它。
//
//  ⚠️ 代次语义:view 代次切换(setView)后,旧的「已可见」不再成立;子系统在
//  下一次 tick 对新代次重发可见性时会重新确认本组件。切换与重发之间存在
//  最多一个 tick 的窗口,期间组件仍在 —— 单 view 终生不变的场景(缩略图
//  预览正是)不受影响;要跨 view 切换消费就绪信号的调用方自己等一帧。
// ============================================================================

namespace lux::ecs
{
    struct MeshInstanceReadyComponent
    {
        // 空标签:存在即事实。EnTT 对空类型有专门的存储优化。
    };

} // namespace lux::ecs
