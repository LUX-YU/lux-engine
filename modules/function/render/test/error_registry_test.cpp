// ============================================================================
//  error_registry_test.cpp — PERMANENT (cpu tier): 锁住错误类型注册表的三条不变量。
//
//      1. 同一个错误类型注册两次拿到同一个 id(幂等,由 type_hash 做键保证)
//      2. 槽位回收后 generation 推进,旧 id 从此查不到(不会命中落进该槽的新类型)
//      3. 消息模板的 {n} 与声明的实参槽逐一对应,格式化后没有残留占位符
//
//  为什么需要它:这三条都是「结构上不可能出错」型的保证,一旦某次重构把 by_type_
//  的键换成名字、或者把 SlotKeyAutoSparseSet 换成不带 generation 的容器,失效是
//  静默的 —— 重复注册会多出一个 id,陈旧 id 会读到别人的描述,而两者都不产生任何
//  编译错误或崩溃,只是让错误报错成另一件事。
//
//  不需要 Vulkan,不需要设备:注册表是纯 CPU 代码。
// ============================================================================

#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/function/render/client/resources/EBuiltinShader.hpp>
#include <lux/engine/description/LayoutContract.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using namespace lux::render;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

namespace
{
    /// 测试专用的两个「动态 feature 错误类型」,模拟 register_ops_fn 里的注册。
    struct ProbeAlpha
    {
        static constexpr const char* name = "test.probe_alpha";
        static constexpr const char* message = "alpha 拿到 {0} 与 {1}";
        static constexpr ERecovery recovery = ERecovery::Retryable;
        static constexpr ErrorArgs args{EErrorArg::Uint, EErrorArg::Hex};
    };

    struct ProbeBeta
    {
        static constexpr const char* name = "test.probe_beta";
        static constexpr const char* message = "beta 无实参";
        static constexpr ERecovery recovery = ERecovery::Bug;
        static constexpr ErrorArgs args{};
    };
} // namespace

int
main()
{
    RenderErrorRegistry registry;

    // ── 引擎自带的那批在构造时已经登记完 ──────────────────────────────────
    {
        const std::size_t engine_count = registry.size();
        CHECK(engine_count > 0);

        const ErrorTypeId by_name = registry.findByName("memory.out_of_memory");
        CHECK(by_name.isValid());
        CHECK(by_name == registry.errorType<err::memory::OutOfMemory>());

        CHECK(!registry.findByName("nonexistent.error").isValid());
        CHECK(registry.size() == engine_count); // 查询不会顺手登记
        std::puts("  [ok] 构造后引擎错误类型表已完整,按名字可解析");
    }

    // ── 不变量 1:注册幂等 ────────────────────────────────────────────────
    {
        const std::size_t before = registry.size();

        const ErrorTypeId first = registry.errorType<ProbeAlpha>();
        const ErrorTypeId second = registry.errorType<ProbeAlpha>();
        CHECK(first.isValid());
        CHECK(first == second);
        CHECK(registry.size() == before + 1);

        // 名字不同的第二个类型是另一个 id。
        const ErrorTypeId other = registry.errorType<ProbeBeta>();
        CHECK(other.isValid());
        CHECK(other != first);
        CHECK(registry.size() == before + 2);
        std::puts("  [ok] 同一类型重复注册返回同一个 id");
    }

    // ── 不变量 2:归还后 generation 推进,旧 id 失效 ───────────────────────
    {
        const ErrorTypeId stale = registry.errorType<ProbeAlpha>();
        CHECK(registry.find(stale).has_value());

        registry.forget<ProbeAlpha>();
        CHECK(!registry.find(stale).has_value());
        CHECK(!registry.findByName("test.probe_alpha").isValid());

        // 重新注册拿到的是新 id;旧 id 不会突然又能解析。
        const ErrorTypeId fresh = registry.errorType<ProbeAlpha>();
        CHECK(fresh.isValid());
        CHECK(fresh != stale);
        CHECK(!registry.find(stale).has_value());
        std::puts("  [ok] 归还后旧 id 永久失效,不会命中回收槽位上的新类型");
    }

    // ── 不变量 3:格式化后没有残留占位符 ──────────────────────────────────
    {
        const ErrorTypeId id = registry.errorType<ProbeAlpha>();
        const std::string out = formatRenderError(registry, makeError(id, 42u, 0xC0DEu));

        CHECK(out.find("test.probe_alpha") != std::string::npos);
        CHECK(out.find("42") != std::string::npos);
        CHECK(out.find("0xC0DE") != std::string::npos);
        CHECK(out.find('{') == std::string::npos); // 占位符全部被替换
        std::puts("  [ok] 实参按声明的语义展开,模板无残留");
    }

    // ── 成功值与失效 id 都能安全格式化 ───────────────────────────────────
    {
        CHECK(RenderError{}.ok());
        CHECK(!formatRenderError(registry, RenderError{}).empty());

        registry.forget<ProbeBeta>();
        const std::string unknown = formatRenderError(registry, makeError(registry.findByName("test.probe_beta")));
        CHECK(!unknown.empty());
        std::puts("  [ok] 成功值与已失效 id 的格式化都不崩");
    }

    // ── 契约索引类实参展开成名字,而不是数字 ──────────────────────────────
    //
    // 这是「错误里不搬字符串,只搬共享表索引」那条机制的验收点:产生侧只放一个
    // 数,消费侧照 EErrorArg 查表拿名字。
    {
        const std::string vk =
            formatRenderError(registry, renderError<err::device::VulkanCallFailed>(encodeVkResult(-2)));
        CHECK(vk.find("VK_ERROR_OUT_OF_DEVICE_MEMORY") != std::string::npos);

        const std::string builtin = formatRenderError(
            registry,
            renderError<err::shader::BuiltinUnavailable>(static_cast<std::uint32_t>(EBuiltinShader::TONEMAP_VERT))
        );
        CHECK(builtin.find("TONEMAP_VERT") != std::string::npos);

        // 契约资源槽:产生侧只放一个下标,消费侧查同一份引擎契约表拿名字,并顺带带出
        // 它的规范位置 —— 于是「本该在哪」不必随错误过线。
        const std::uint32_t uviews = lux::rdesc::logicalResourceIndex("uViews");
        CHECK(uviews != lux::rdesc::kInvalidLogicalResourceIndex);
        const std::string contract =
            formatRenderError(registry, renderError<err::pipeline::BindingTypeMismatch>(uviews));
        CHECK(contract.find("uViews") != std::string::npos);
        CHECK(contract.find("契约 set") != std::string::npos);

        // 描述符槽与绑定域也走各自的枚举名。
        const std::string domain = formatRenderError(
            registry,
            renderError<err::pipeline::DomainLayoutNotInitialised>(
                static_cast<std::uint32_t>(lux::rdesc::EBindFrequency::FEATURE))
        );
        CHECK(domain.find("FEATURE") != std::string::npos);

        std::puts("  [ok] VkResult / EBuiltinShader / 契约资源 / 绑定域 实参展开成名字");
    }

    // ── 不在契约里的索引自证,而不是伪装成一个真资源 ──────────────────────
    {
        const std::string out = formatRenderError(
            registry,
            renderError<err::pipeline::BindingTypeMismatch>(lux::rdesc::kInvalidLogicalResourceIndex)
        );
        CHECK(out.find("不在契约里") != std::string::npos);
        std::puts("  [ok] 越界的契约索引自证为「不在契约里」");
    }

    // ── 无实参的错误类型不会凭空报出一个假事实 ────────────────────────────
    //
    // VulkanObjectCreationFailed 声明了零实参(它的失败点只知道拿到了空句柄)。
    // 即使调用方误填了一个数,消息里也不会出现任何被当作 VkResult 解释的内容 ——
    // 「声明了才展开」是这套机制不撒谎的根据。
    {
        const ErrorTypeId id = registry.errorType<err::device::VulkanObjectCreationFailed>();
        const std::string out = formatRenderError(registry, makeError(id, encodeVkResult(-2)));
        CHECK(out.find("VK_") == std::string::npos);
        std::puts("  [ok] 未声明的实参槽不参与展开");
    }

    std::puts("error_registry_test PASSED");
    return 0;
}
