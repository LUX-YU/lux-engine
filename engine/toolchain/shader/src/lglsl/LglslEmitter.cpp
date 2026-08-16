#include <lux/engine/toolchain/shader/lglsl/LglslEmitter.hpp>
#include <lux/engine/description/LayoutContract.hpp>

#include <array>
#include <lux/engine/platform/FormatCompat.h>
#include <optional>

namespace lux::shadergen::lglsl
{
namespace
{
    // ── small lexical helpers (convention: whitespace = space/tab) ─────

    [[nodiscard]] std::string_view trim(std::string_view s) noexcept
    {
        const size_t b = s.find_first_not_of(" \t\r");
        if (b == std::string_view::npos) return {};
        const size_t e = s.find_last_not_of(" \t\r");
        return s.substr(b, e - b + 1);
    }

    /// Strips a trailing "//" line comment, then trims — used for
    /// classification only (detecting decl starts / single-line completeness /
    /// name extraction); the output side always uses the original text, so
    /// comments are preserved verbatim.
    [[nodiscard]] std::string_view stripLineComment(std::string_view s) noexcept
    {
        if (const size_t c = s.find("//"); c != std::string_view::npos)
            s = s.substr(0, c);
        return trim(s);
    }

    /// Takes the next whitespace-delimited token and advances the cursor;
    /// returns empty if there is no token.
    [[nodiscard]] std::string_view nextToken(std::string_view& cursor) noexcept
    {
        size_t b = cursor.find_first_not_of(" \t");
        if (b == std::string_view::npos) { cursor = {}; return {}; }
        size_t e = cursor.find_first_of(" \t", b);
        std::string_view tok = (e == std::string_view::npos)
                             ? cursor.substr(b)
                             : cursor.substr(b, e - b);
        cursor = (e == std::string_view::npos) ? std::string_view{} : cursor.substr(e);
        return tok;
    }

    /// Strips the trailing array suffix and semicolon off a declaration name:
    /// "uTex[];" -> "uTex", "xs[16]" -> "xs".
    [[nodiscard]] std::string_view stripNameSuffix(std::string_view name) noexcept
    {
        if (size_t p = name.find_first_of("[;"); p != std::string_view::npos)
            name = name.substr(0, p);
        return name;
    }

    // ── pragma parsing ──────────────────────────────────────────────────

    [[nodiscard]] bool parseStage(std::string_view v, rdesc::EShaderType& out) noexcept
    {
        if (v == "vertex")   { out = rdesc::EShaderType::VERTEX;   return true; }
        if (v == "fragment") { out = rdesc::EShaderType::FRAGMENT; return true; }
        if (v == "compute")  { out = rdesc::EShaderType::COMPUTE;  return true; }
        return false;
    }

    /// Parses a "//!" pragma line (prefix already stripped). Recognizes
    /// lux-shader / lux-variant; any other lux-* directive is an error (so a
    /// typo fails loudly instead of silently doing nothing).
    [[nodiscard]] std::optional<std::string> parsePragma(std::string_view body, ShaderMeta& meta)
    {
        std::string_view cursor = body;
        const std::string_view directive = nextToken(cursor);

        if (directive == "lux-shader")
        {
            for (std::string_view kv = nextToken(cursor); !kv.empty(); kv = nextToken(cursor))
            {
                const size_t eq = kv.find('=');
                if (eq == std::string_view::npos)
                    return lux::format("lux-shader 需要 key=value 形式,得到 '{}'", kv);
                const std::string_view key = kv.substr(0, eq);
                const std::string_view val = kv.substr(eq + 1);
                if (key == "stage")
                {
                    if (!parseStage(val, meta.stage))
                        return lux::format("未知 stage '{}'(可用:vertex/fragment/compute)", val);
                }
                else if (key == "entry")
                {
                    meta.entry.assign(val);
                }
                else
                {
                    return lux::format("lux-shader 不认识的键 '{}'", key);
                }
            }
            return std::nullopt;
        }
        if (directive == "lux-variant")
        {
            const std::string_view name = nextToken(cursor);
            if (name.empty())
                return std::string("lux-variant 需要一个宏名");
            meta.variants.push_back({std::string(name)});
            return std::nullopt;
        }
        if (directive.starts_with("lux-"))
            return lux::format("不认识的 pragma 指令 '{}'", directive);
        return std::nullopt;   // a plain "//!" comment, unrelated to this emitter
    }

    // ── resource declaration recognition ────────────────────────────────

    constexpr std::array kQualifiers = {
        std::string_view{"readonly"}, std::string_view{"writeonly"},
        std::string_view{"coherent"}, std::string_view{"restrict"},
        std::string_view{"volatile"},
    };

    /// Whitelist of layout qualifiers that can be merged with slot injection
    /// (pure memory/matrix layout). A layout containing set / push_constant /
    /// location / constant_id etc. is an "explicit declaration" and passes
    /// through unchanged.
    constexpr std::array kMergeableLayoutQualifiers = {
        std::string_view{"std140"},    std::string_view{"std430"},
        std::string_view{"scalar"},
        std::string_view{"row_major"}, std::string_view{"column_major"},
    };

    /// If trimmed starts with layout(...) and everything inside the
    /// parentheses is on the whitelist, returns the parenthesized content
    /// (to be merged with the slot); otherwise returns nullopt (an explicit
    /// declaration, or a non-layout line).
    [[nodiscard]] std::optional<std::string_view> mergeableLayoutContent(std::string_view trimmed) noexcept
    {
        if (!trimmed.starts_with("layout"))
            return std::nullopt;
        const size_t open = trimmed.find('(');
        const size_t close = trimmed.find(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open)
            return std::nullopt;
        const std::string_view content = trimmed.substr(open + 1, close - open - 1);

        std::string_view cursor = content;
        size_t items = 0;
        for (std::string_view tok = nextToken(cursor); !tok.empty(); tok = nextToken(cursor))
        {
            // Comma-separated: strip the trailing comma, then check each item
            // against the whitelist.
            while (!tok.empty() && tok.back() == ',') tok.remove_suffix(1);
            if (tok.empty()) continue;
            ++items;
            bool ok = false;
            for (const auto q : kMergeableLayoutQualifiers)
                if (tok == q) { ok = true; break; }
            if (!ok)
                return std::nullopt;
        }
        return items > 0 ? std::optional{content} : std::nullopt;
    }

    /// Detects a declaration start: an [optionally mergeable layout(...)]
    /// followed by qualifier* then uniform|buffer. A line with an explicit
    /// layout (containing set= / push_constant / location etc.) is not
    /// treated as a declaration pending injection.
    struct DeclHead
    {
        bool             is_decl{false};
        std::string_view after_keyword;    ///< remainder after the keyword (used to extract the name)
        std::string_view merge_layout;     ///< original layout() content pending merge (may be empty)
    };

    [[nodiscard]] DeclHead matchDeclStart(std::string_view trimmed) noexcept
    {
        std::string_view merge{};
        std::string_view cursor = trimmed;
        if (trimmed.starts_with("layout"))
        {
            const auto mergeable = mergeableLayoutContent(trimmed);
            if (!mergeable)
                return {};   // explicit layout(set=… / push_constant / location …) -> passed through unchanged
            merge = *mergeable;
            cursor = trimmed.substr(trimmed.find(')') + 1);
        }
        for (std::string_view tok = nextToken(cursor); !tok.empty(); tok = nextToken(cursor))
        {
            if (tok == "uniform" || tok == "buffer")
                return {true, cursor, merge};
            bool is_qualifier = false;
            for (const auto q : kQualifiers)
                if (tok == q) { is_qualifier = true; break; }
            if (!is_qualifier)
                return {};
        }
        return {};
    }

    /// R2-1:契约外局部资源的固定落点 —— 管线私有集的既有惯例(实证自迁出前的
    /// 5 个私有条目,全部落在 set 1)。运行期由 LayoutPlan 的 PipelinePrivate
    /// 衔接:形状=反射,不同管线的同号私有集互不相干。
    inline constexpr uint32_t kLocalResourceSet = 1;

    /// Assembles the injected layout: the slot comes first, followed by any
    /// memory qualifiers the author wrote.
    [[nodiscard]] std::string formatInjection(uint32_t set, uint32_t binding,
                                              std::string_view merge_layout)
    {
        if (merge_layout.empty())
            return lux::format("layout(set = {}, binding = {}) ", set, binding);
        return lux::format("layout(set = {}, binding = {}, {}) ",
                           set, binding, merge_layout);
    }

    // ── emitter state machine ───────────────────────────────────────────

    class Emitter
    {
    public:
        Emitter(std::string_view source, EEmitMode mode) : source_(source), mode_(mode) {}

        lux::cxx::expected<EmitOutput, std::string> run()
        {
            std::string_view rest = source_;
            uint32_t line_no = 0;
            while (!rest.empty())
            {
                ++line_no;
                const size_t nl = rest.find('\n');
                const std::string_view line = (nl == std::string_view::npos)
                                            ? rest : rest.substr(0, nl);
                rest = (nl == std::string_view::npos) ? std::string_view{} : rest.substr(nl + 1);

                if (auto err = consumeLine(line, line_no))
                    return lux::cxx::unexpected<std::string>(
                        lux::format("line {}: {}", line_no, *err));
            }
            if (in_block_)
                return lux::cxx::unexpected<std::string>(
                    lux::format("line {}: 资源块声明未闭合(缺 '}};')", block_start_line_));
            if (mode_ == EEmitMode::Shader &&
                out_.meta.stage == rdesc::EShaderType::UNDEFINED)
                return lux::cxx::unexpected<std::string>(
                    "缺少 '//! lux-shader stage=...' pragma(.lglsl 必须自描述)");
            if (mode_ == EEmitMode::Header &&
                out_.meta.stage != rdesc::EShaderType::UNDEFINED)
                return lux::cxx::unexpected<std::string>(
                    "共享头(.lglslh)不应声明 lux-shader —— stage 属于包含它的着色器");
            return std::move(out_);
        }

    private:
        /// Consumes one line at a time; returns an error message (without a
        /// line-number prefix). Classification (decl start / single-line
        /// completeness / block end) is always based on the text with
        /// trailing comments stripped; the output text and the name-lookup
        /// text are each picked separately as needed — so a trailing comment
        /// can no longer confuse the state machine.
        std::optional<std::string> consumeLine(std::string_view line, uint32_t line_no)
        {
            const std::string_view judged = stripLineComment(line);

            if (in_block_)
                return consumeBlockLine(line, judged);

            const std::string_view trimmed = trim(line);
            if (trimmed.starts_with("//!"))   // a pragma is itself a comment, so check it before stripping comments
            {
                if (auto err = parsePragma(trimmed.substr(3), out_.meta))
                    return err;
                appendLine(line);   // keep the pragma as a comment, for easier debugging/diffing
                return std::nullopt;
            }

            const DeclHead head = matchDeclStart(judged);
            if (!head.is_decl)
            {
                appendLine(line);
                return std::nullopt;
            }

            // Single-line opaque / single-line block: if it ends with ';',
            // this line is already a complete declaration.
            if (judged.ends_with(";"))
                return emitDeclaration(line, judged, line_no);

            // Multi-line block declaration: enter collection mode; where to
            // inject on the block's first line is decided when it's flushed.
            in_block_ = true;
            block_start_line_ = line_no;
            block_lines_.assign(1, std::string(line));
            block_judged_.assign(judged);
            block_brace_depth_ = braceDelta(judged);
            return std::nullopt;
        }

        std::optional<std::string> consumeBlockLine(std::string_view line, std::string_view judged)
        {
            block_lines_.emplace_back(line);
            block_judged_ += '\n';
            block_judged_ += judged;
            block_brace_depth_ += braceDelta(judged);
            if (block_brace_depth_ <= 0 && judged.ends_with(";"))
            {
                in_block_ = false;
                std::string whole;
                for (const auto& l : block_lines_)
                {
                    whole += l;
                    whole += '\n';
                }
                whole.pop_back();
                return emitDeclaration(whole, block_judged_, block_start_line_);
            }
            return std::nullopt;
        }

        /// A complete declaration (single-line, or an assembled block) ->
        /// look up the contract -> inject the slot and emit.
        /// A mergeable layout the author wrote (std430 etc.) is folded into
        /// the injected layout (replacing the original parentheses).
        /// `judged` is the classification text with comments stripped;
        /// `full_text` is the original text (comments intact).
        std::optional<std::string> emitDeclaration(std::string_view full_text,
                                                   std::string_view judged,
                                                   uint32_t decl_line)
        {
            const std::string_view name = declarationName(judged);
            if (name.empty())
                return std::string("无法从资源声明中提取名字(约定式解析;若为特殊形态请显式写 layout)");

            const auto* entry = rdesc::findLogicalResource(name);

            // R2-1:契约不再是词汇表的边界。契约条目 = 冻结域与引擎共享资源
            // (它们的位置由契约定,一行未动);**契约外 = 自由域局部资源**,
            // 自动分配:set 1,按本文件声明序编 binding —— 单资源文件得 b0,
            // highlight_composite 的 uBlur/uMask 得 b0/b1,与迁出前的契约
            // 钉位逐字节一致(.spv md5 为证)。
            //
            // 两个已记档的边界(endgame checklist R2-1):
            //  · 跨 stage 共享的局部资源:按文件独立计数会分裂 —— 显式写
            //    layout(set=..,binding=..)(matchDeclStart 直通)即可;
            //  · .lglslh 里的局部资源按**头文件**独立计数:多个带局部资源的头
            //    被同一着色器包含会撞 b0 —— 今日树内无此形状,出现时同上处理。
            const uint32_t inj_set     = entry ? entry->canonical_set     : kLocalResourceSet;
            const uint32_t inj_binding = entry ? entry->canonical_binding : next_local_binding_++;

            const DeclHead head = matchDeclStart(judged);

            // Declaration body = the original text minus its leading
            // indentation and (if present) the original layout(...) segment
            // that is about to be merged.
            const size_t indent_end = full_text.find_first_not_of(" \t");
            const std::string_view indent =
                full_text.substr(0, indent_end == std::string_view::npos ? 0 : indent_end);
            std::string_view body =
                full_text.substr(indent_end == std::string_view::npos ? 0 : indent_end);
            if (!head.merge_layout.empty())
            {
                body.remove_prefix(body.find(')') + 1);
                if (const size_t b = body.find_first_not_of(" \t"); b != std::string_view::npos)
                    body.remove_prefix(b);
            }

            out_.glsl.append(indent);
            out_.glsl.append(formatInjection(inj_set, inj_binding, head.merge_layout));
            out_.glsl.append(body);
            out_.glsl.push_back('\n');
            out_.injected.push_back({std::string(name), inj_set, inj_binding});
            (void)decl_line;
            return std::nullopt;
        }

        /// Reconciliation key: for a block declaration, use the instance name
        /// (the token after '}'), or the block name if there is no instance
        /// name; for an opaque declaration, use the last token. Either way,
        /// the array suffix and semicolon are stripped.
        [[nodiscard]] static std::string_view declarationName(std::string_view trimmed) noexcept
        {
            const size_t close = trimmed.rfind('}');
            if (close != std::string_view::npos)
            {
                std::string_view tail = trimmed.substr(close + 1);
                std::string_view cursor = tail;
                const std::string_view instance = nextToken(cursor);
                if (!instance.empty() && instance != ";")
                    return stripNameSuffix(instance);
                // No instance name -> use the block name: the first token
                // after the uniform|buffer keyword
                const DeclHead head = matchDeclStart(trimmed);
                std::string_view c2 = head.after_keyword;
                return stripNameSuffix(nextToken(c2));
            }
            // opaque: the last token
            std::string_view cursor = trimmed;
            std::string_view last{};
            for (std::string_view tok = nextToken(cursor); !tok.empty(); tok = nextToken(cursor))
                last = tok;
            return stripNameSuffix(last);
        }

        [[nodiscard]] static int braceDelta(std::string_view s) noexcept
        {
            int d = 0;
            for (char c : s)
            {
                if (c == '{') ++d;
                if (c == '}') --d;
            }
            return d;
        }

        void appendLine(std::string_view line)
        {
            out_.glsl.append(line);
            out_.glsl.push_back('\n');
        }

        std::string_view source_;
        EEmitMode        mode_{EEmitMode::Shader};
        EmitOutput       out_{};

        bool                     in_block_{false};
        uint32_t                 next_local_binding_{0};   ///< R2-1:契约外局部资源的声明序计数(每文件一个 Emitter 实例,天然按文件归零)
        int                      block_brace_depth_{0};
        uint32_t                 block_start_line_{0};
        std::vector<std::string> block_lines_{};   ///< verbatim text (for output)
        std::string              block_judged_{};  ///< comment-stripped, concatenated (for classification/name lookup)
    };

} // namespace

lux::cxx::expected<EmitOutput, std::string> emitCanonicalGlsl(std::string_view source, EEmitMode mode)
{
    return Emitter(source, mode).run();
}

} // namespace lux::shadergen::lglsl
