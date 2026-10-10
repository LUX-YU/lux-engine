#pragma once
/**
 * @file PreparedPipelineStages.hpp
 * @brief 一条管线全部 stage 的模块与反射,顺序与请求一致。
 *
 * 它存在的理由是**让「句柄先行」从一条纪律变成唯一可能的调用形状**。
 *
 * 域合并切换可能新增着色器记录并让内部存储扩容,于是任何提前取出的反射指针都会悬垂。
 * 这条规则一度由十几份注释分散维护,而一次不完整的清扫就让若干管线拿到了悬垂指针拷出
 * 的空反射,三套互不兼容的布局落进同一个 pass。
 *
 * 现在所有切换都在 preparePipelineStages 内部一次做完,调用方拿到的是**反射的拷贝** ——
 * 之后无论谁再注册着色器,这些数据都不会失效。物理上写不出「先取指针再切换」。
 */

#include <lux/engine/function/render/client/core/ResourceHandle.hpp> // ShaderHandle
#include <lux/engine/render/core/vk_fwd.hpp>                         // VkShaderModule
#include <lux/engine/description/ShaderInfo.hpp>                     // 按值持有,需要完整类型

#include <cstddef>
#include <span>
#include <vector>

namespace lux::render
{
    class PreparedPipelineStages
    {
    public:
        [[nodiscard]] std::size_t size() const noexcept
        {
            return stages_.size();
        }
        [[nodiscard]] bool empty() const noexcept
        {
            return stages_.empty();
        }

        [[nodiscard]] ShaderHandle handle(std::size_t stage) const noexcept
        {
            return stages_[stage].handle;
        }
        [[nodiscard]] VkShaderModule module(std::size_t stage) const noexcept
        {
            return stages_[stage].module;
        }
        [[nodiscard]] const lux::rdesc::ShaderInfo& info(std::size_t stage) const noexcept
        {
            return stages_[stage].info;
        }

        /// 供管线注册使用的反射指针数组,与请求同序。指向本对象内的拷贝,所以只需保证
        /// 本对象活到注册调用之后。
        [[nodiscard]] std::span<const lux::rdesc::ShaderInfo* const> infos() const noexcept
        {
            return info_pointers_;
        }

        /// 装入一个已经切换完成的 stage。只有着色器资源在完成全部注册后才调用它。
        void appendStage(ShaderHandle handle, VkShaderModule module, const lux::rdesc::ShaderInfo& info)
        {
            stages_.push_back(Stage{handle, module, info});
        }

        void reserveStages(std::size_t count)
        {
            stages_.reserve(count);
        }

        /// 拷贝全部完成后重建指针表 —— vector 增长会让旧指针失效,所以只在填完之后建
        /// 一次。装载与建表分开,是为了让「指针在什么时刻才稳定」这件事显式可见。
        void bindInfoPointers()
        {
            info_pointers_.clear();
            info_pointers_.reserve(stages_.size());
            for (const Stage& s : stages_)
                info_pointers_.push_back(&s.info);
        }

    private:
        struct Stage
        {
            ShaderHandle handle{};
            VkShaderModule module{};
            lux::rdesc::ShaderInfo info{};
        };

        std::vector<Stage> stages_;
        std::vector<const lux::rdesc::ShaderInfo*> info_pointers_;
    };

} // namespace lux::render
