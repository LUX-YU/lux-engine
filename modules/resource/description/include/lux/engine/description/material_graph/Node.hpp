#pragma once
// =============================================================================
//  Node.hpp  —  材质图节点基类
// -----------------------------------------------------------------------------
//  多态节点 + 引脚容器（镜像 FlowForge 的 Node，但去掉 exec/token/ENodeOperation）。
//  具体节点见 Nodes.hpp。
// =============================================================================

#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "DataPin.hpp"
#include <lux/engine/resource/visibility.h>

namespace lux::rdesc
{
    /**
     * @brief 材质图节点种类。新增材质节点在 COUNT 前追加。
     */
    enum class EMatNodeKind : uint16_t
    {
        Invalid = 0,
        Constant,       ///< 0 in, 1 out —— 常量
        Input,          ///< 0 in, 1 out —— 一个 MaterialInput（uv0 / world_normal ...）
        SampleTexture,  ///< 1 in(uv), 1 out(vec4) —— 采样 bindless 纹理（set 2）
        Math,           ///< 2 in, 1 out —— 算术（op 由 payload 选）
        Swizzle,        ///< 1 in, 1 out
        Construct,      ///< n in, 1 out —— 组装向量
        DecodeNormal,   ///< 1 in(rgb), 1 out(vec3) —— 法线贴图解码（切线空间）
        TbnTransform,   ///< 1 in(切线空间法线), 1 out —— TBN 变换到世界空间
        Param,          ///< 0 in, 1 out —— 读一个 per-material 参数槽（set 4 Graph 族）
        OutputSurface,  ///< n in（每个表面属性一个）, 0 out —— 终端节点
        COUNT
    };

    LUX_RESOURCE_PUBLIC const char* toString(EMatNodeKind kind) noexcept;

    class LUX_RESOURCE_PUBLIC Node
    {
    public:
        explicit Node(EMatNodeKind kind) : kind_(kind) {}
        virtual ~Node();  // 定义在 Node.cpp（MSVC 导出锚点）

        Node& operator=(const Node&) = delete;

        /// 深拷贝（多态克隆）：每个子类返回一个同类型 + 同 payload + 同引脚（含连接/常量）的副本。
        /// 编辑器据此从资产持有的图复制出一份可编辑的工作副本（图本身 move-only）。
        [[nodiscard]] virtual std::unique_ptr<Node> clone() const = 0;

        node_id      id() const noexcept { return id_; }
        void         setId(node_id id) noexcept { id_ = id; }
        EMatNodeKind kind() const noexcept { return kind_; }

        /// Safe downcast to a concrete node type — no RTTI. Keys kind() against
        /// the target's `kKind` constant (every concrete node declares one).
        /// Returns nullptr on a kind mismatch, exactly like the dynamic_cast it
        /// replaces.
        template<class T> [[nodiscard]] T* as() noexcept
        { return kind_ == T::kKind ? static_cast<T*>(this) : nullptr; }

        template<class T> [[nodiscard]] const T* as() const noexcept
        { return kind_ == T::kKind ? static_cast<const T*>(this) : nullptr; }

        const std::string& name() const noexcept { return name_; }
        void               setName(std::string n) { name_ = std::move(n); }

        std::vector<DataPin>&       inputs()  noexcept { return in_pins_; }
        const std::vector<DataPin>& inputs()  const noexcept { return in_pins_; }
        std::vector<DataPin>&       outputs() noexcept { return out_pins_; }
        const std::vector<DataPin>& outputs() const noexcept { return out_pins_; }

        // ---- 编辑器元数据（lowering 不读；LMGR v2 起随图持久化）---------------
        // 画布位置由 GraphKit 经 IGraphView::nodePos/setNodePos 读写。v1 资产
        // 解码后 ui_placed=false → GraphLayout 网格摆放，下次保存即升级 v2。
        // clone() 经受保护拷贝构造自动携带。
        float ui_pos[2] = { 0.0f, 0.0f };
        bool  ui_placed = false;

    protected:
        // 受保护的拷贝构造：只供子类 clone() 用（`make_unique<Derived>(*this)`），
        // 对外仍不可拷贝（防切片）。
        Node(const Node&) = default;

        node_id              id_ = invalid_node;
        EMatNodeKind         kind_;
        std::string          name_;
        std::vector<DataPin> in_pins_;
        std::vector<DataPin> out_pins_;
    };

} // namespace lux::rdesc
