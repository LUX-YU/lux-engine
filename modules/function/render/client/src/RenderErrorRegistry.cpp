#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/cxx/core/Format.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/function/render/client/core/EngineSetSlot.hpp>          // DescriptorSlot 实参槽
#include <lux/engine/function/render/client/resources/EBuiltinShader.hpp>    // BuiltinShader 实参槽
#include <lux/engine/description/LayoutContract.hpp>         // LogicalResource / BindFrequency 实参槽

#include <string>

namespace lux::render
{
namespace
{
    /// 常见 VkResult 的稳定 wire 值→名字。这一层只解码已经结构化
    /// 的错误，不应为了打印文字而要求 Vulkan SDK。数值是 Vulkan ABI 的
    /// 一部分；未列出的扩展结果保持数值形式。
    const char* vkResultName(std::int32_t result) noexcept
    {
        switch (result)
        {
        case 0:           return "VK_SUCCESS";
        case 1:           return "VK_NOT_READY";
        case 2:           return "VK_TIMEOUT";
        case 1000001003:  return "VK_SUBOPTIMAL_KHR";
        case -1:          return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case -2:          return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case -3:          return "VK_ERROR_INITIALIZATION_FAILED";
        case -4:          return "VK_ERROR_DEVICE_LOST";
        case -5:          return "VK_ERROR_MEMORY_MAP_FAILED";
        case -7:          return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case -8:          return "VK_ERROR_FEATURE_NOT_PRESENT";
        case -9:          return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case -10:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case -11:         return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case -12:         return "VK_ERROR_FRAGMENTED_POOL";
        case -1000069000: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case -1000001004: return "VK_ERROR_OUT_OF_DATE_KHR";
        case -1000000000: return "VK_ERROR_SURFACE_LOST_KHR";
        case -1000000001: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case -1000011001: return "VK_ERROR_VALIDATION_FAILED_EXT";
        default:                                return nullptr;
        }
    }

    /// 内置着色器枚举值 → 名字。与 EBuiltinShader 本身同源(同一张 X-macro 列表),
    /// 所以两边不会分叉。
    const char* builtinShaderName(std::uint32_t value) noexcept
    {
        switch (static_cast<EBuiltinShader>(value))
        {
#define LUX_BUILTIN_SHADER_NAME_CASE(enum_name, prefix) \
        case EBuiltinShader::enum_name: return #enum_name;
            LUX_BUILTIN_SHADER_LIST(LUX_BUILTIN_SHADER_NAME_CASE)
#undef LUX_BUILTIN_SHADER_NAME_CASE
        default: return nullptr;
        }
    }

    const char* descriptorSlotName(std::uint32_t value) noexcept
    {
        switch (static_cast<EDescriptorSetSlot>(value))
        {
        case EDescriptorSetSlot::Scene:      return "Scene";
        case EDescriptorSetSlot::Instance:   return "Instance";
        case EDescriptorSetSlot::Texture:    return "Texture";
        case EDescriptorSetSlot::Light:      return "Light";
        case EDescriptorSetSlot::Material:   return "Material";
        case EDescriptorSetSlot::Particle:   return "Particle";
        case EDescriptorSetSlot::Compute:    return "Compute";
        case EDescriptorSetSlot::VertexPool: return "VertexPool";
        default:                             return nullptr;
        }
    }

    const char* bindFrequencyName(std::uint32_t value) noexcept
    {
        switch (static_cast<rdesc::EBindFrequency>(value))
        {
        case rdesc::EBindFrequency::GLOBAL:     return "GLOBAL";
        case rdesc::EBindFrequency::BINDLESS:   return "BINDLESS";
        case rdesc::EBindFrequency::FEATURE:    return "FEATURE";
        case rdesc::EBindFrequency::PASS_LOCAL: return "PASS_LOCAL";
        default:                                return nullptr;
        }
    }

    void appendArg(std::string& out, EErrorArg kind, std::uint32_t value)
    {
        switch (kind)
        {
        case EErrorArg::None:
            out += "<未使用>";
            return;

        case EErrorArg::Uint:
            out += std::to_string(value);
            return;

        case EErrorArg::Hex:
        {
            out += lux::format("0x{:X}", value);
            return;
        }

        case EErrorArg::VkResult:
        {
            const auto signed_value = static_cast<std::int32_t>(value);
            if (const char* named = vkResultName(signed_value))
                out += named;
            else
                out += std::to_string(signed_value);
            return;
        }

        case EErrorArg::VkFormat:
            out += "VkFormat(";
            out += std::to_string(value);
            out += ')';
            return;

        case EErrorArg::BuiltinShader:
            if (const char* named = builtinShaderName(value))
                out += named;
            else
            {
                out += "EBuiltinShader(";
                out += std::to_string(value);
                out += ')';
            }
            return;

        case EErrorArg::LogicalResource:
            // 名字连同它在契约里的规范位置一起展开 —— 消费侧据此可自行算出「本该在
            // 哪个域槽的哪个 binding」,所以期望值不必随错误过线。
            if (const rdesc::LogicalResourceDesc* res = rdesc::logicalResourceAt(value))
            {
                out += '\'';
                out += res->name;
                out += "'(契约 set ";
                out += std::to_string(res->canonical_set);
                out += ", binding ";
                out += std::to_string(res->canonical_binding);
                out += ')';
            }
            else
            {
                out += "<不在契约里>";
            }
            return;

        case EErrorArg::DescriptorSlot:
            if (const char* named = descriptorSlotName(value))
                out += named;
            else
            {
                out += "set ";
                out += std::to_string(value);
            }
            return;

        case EErrorArg::BindFrequency:
            if (const char* named = bindFrequencyName(value))
                out += named;
            else
                out += std::to_string(value);
            return;

        case EErrorArg::GraphResource:
            out += "resource#";
            out += std::to_string(value);
            return;

        case EErrorArg::GraphPass:
            out += "pass#";
            out += std::to_string(value);
            return;

        case EErrorArg::FeatureType:
        {
            // 低 32 位而已(见 EErrorArg::FeatureType),渲染层没有 id→名字 的反查表
            // ——注册表在 Renderer 那一侧。原样以十六进制给出,消费侧自行比对。
            out += lux::format("feature:0x{:08X}", value);
            return;
        }
        }
        out += std::to_string(value);
    }

    /// 展开 message 里的 {n},其余字符原样拷贝。未声明或越界的 {n} 原样保留 ——
    /// concept 已在编译期拦住这种情况,这里只保证格式化本身不会崩。
    std::string expandMessage(const ErrorTypeDesc& desc, const RenderError& error)
    {
        std::string_view tmpl = desc.message ? desc.message : "";
        std::string      out;
        out.reserve(tmpl.size() + 32);

        for (std::size_t i = 0; i < tmpl.size(); ++i)
        {
            const bool is_placeholder =
                tmpl[i] == '{' && i + 2 < tmpl.size() && tmpl[i + 2] == '}'
                && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9';
            if (!is_placeholder)
            {
                out += tmpl[i];
                continue;
            }

            const auto slot = static_cast<std::size_t>(tmpl[i + 1] - '0');
            if (slot < kErrorArgCount)
                appendArg(out, desc.args[slot], error.args[slot]);
            else
                out.append(tmpl.substr(i, 3));
            i += 2;
        }
        return out;
    }
} // namespace

RenderErrorRegistry& renderErrorRegistry() noexcept
{
    static RenderErrorRegistry instance;
    return instance;
}

RenderErrorRegistry::RenderErrorRegistry()
{
#define LUX_RENDER_ERROR_REGISTER(T) (void)errorType<T>();
    LUX_RENDER_ERROR_LIST(LUX_RENDER_ERROR_REGISTER)
#undef LUX_RENDER_ERROR_REGISTER
}

ErrorTypeId RenderErrorRegistry::acquire(TypeKey key, const ErrorTypeDesc& desc)
{
    {
        const std::shared_lock read{mutex_};
        if (const auto it = by_type_.find(key); it != by_type_.end())
            return it->second;
    }

    const std::unique_lock write{mutex_};

    // 双检:两个线程同时首次请求同一个类型时,后到的那个在这里命中。
    if (const auto it = by_type_.find(key); it != by_type_.end())
        return it->second;

    const std::string_view name{desc.name ? desc.name : ""};
    if (const auto clash = by_name_.find(name); clash != by_name_.end())
    {
        std::string what = "RenderErrorRegistry: 两个不同的错误类型声明了同一个 name \"";
        what += name;
        what += '"';
        renderFatal(what);
    }

    const ErrorTypeId id = types_.insert(desc);
    by_type_.emplace(key, id);
    by_name_.emplace(name, id);
    return id;
}

void RenderErrorRegistry::release(TypeKey key) noexcept
{
    const std::unique_lock write{mutex_};

    const auto it = by_type_.find(key);
    if (it == by_type_.end())
        return;

    const ErrorTypeId id = it->second;
    if (const ErrorTypeDesc* desc = types_.tryGet(id))
        by_name_.erase(std::string_view{desc->name ? desc->name : ""});

    types_.erase(id);
    by_type_.erase(it);
}

std::optional<ErrorTypeDesc> RenderErrorRegistry::find(ErrorTypeId id) const
{
    const std::shared_lock read{mutex_};
    if (const ErrorTypeDesc* desc = types_.tryGet(id))
        return *desc;
    return std::nullopt;
}

ErrorTypeId RenderErrorRegistry::findByName(std::string_view name) const
{
    const std::shared_lock read{mutex_};
    const auto it = by_name_.find(name);
    return it != by_name_.end() ? it->second : ErrorTypeId{};
}

std::vector<std::pair<ErrorTypeId, ErrorTypeDesc>> RenderErrorRegistry::snapshot() const
{
    const std::shared_lock read{mutex_};

    const auto& keys   = types_.keys();
    const auto& values = types_.values();

    std::vector<std::pair<ErrorTypeId, ErrorTypeDesc>> out;
    out.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i)
        out.emplace_back(keys[i], values[i]);
    return out;
}

std::size_t RenderErrorRegistry::size() const
{
    const std::shared_lock read{mutex_};
    return types_.size();
}

std::string formatRenderError(const RenderErrorRegistry& registry, const RenderError& error)
{
    if (error.ok())
        return "成功";

    const std::optional<ErrorTypeDesc> desc = registry.find(error.type);
    if (!desc)
    {
        std::string out = "未知错误类型(id ";
        out += std::to_string(error.type.index);
        out += '/';
        out += std::to_string(error.type.gen);
        out += ",可能来自已卸载的 feature)";
        return out;
    }

    std::string out{desc->name ? desc->name : "?"};
    out += ": ";
    out += expandMessage(*desc, error);
    return out;
}

} // namespace lux::render
