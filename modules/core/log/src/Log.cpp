#include <lux/engine/log/Log.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace lux::log
{
    namespace
    {
        /// Output iterator that writes at most `cap` chars into `p` but keeps
        /// COUNTING past that, so truncation is detectable.
        ///
        /// It exists because `std::vformat_to` has no bounded variant:
        /// `format_to_n` is a template and cannot take `format_args`, and
        /// `std::vformat` returns a `std::string` — which allocates, on
        /// whatever thread called us. Keeping the stack buffer is the whole
        /// point (see Log.hpp §"Front end").
        class BufSink
        {
        public:
            using difference_type = std::ptrdiff_t;

            BufSink(char* p, std::size_t cap) noexcept : p_(p), cap_(cap) {}

            BufSink& operator=(char c) noexcept
            {
                if (n_ < cap_) p_[n_] = c;
                ++n_;
                return *this;
            }
            BufSink& operator*()     noexcept { return *this; }
            BufSink& operator++()    noexcept { return *this; }
            BufSink  operator++(int) noexcept { return *this; }

            std::size_t written() const noexcept { return n_ < cap_ ? n_ : cap_; }

        private:
            char*       p_;
            std::size_t cap_;
            std::size_t n_{0};
        };

        static_assert(std::output_iterator<BufSink, char>,
                      "BufSink must model output_iterator<char> for std::vformat_to");

        /// 单出口状态。output 的读侧走裸原子指针 RCU(与 events 的泵表同一套
        /// 纪律 —— atomic<shared_ptr> 在主流实现里带锁,会把锁放回每条日志的
        /// 热路径);历史闭包进退休表,活到进程结束(setOutput 每进程 ~2 次)。
        struct State
        {
            std::mutex                             admin;
            std::atomic<const OutputFn*>           output{nullptr};
            std::vector<std::unique_ptr<OutputFn>> retired;
            std::atomic<std::uint8_t>              min_level{
                static_cast<std::uint8_t>(ELevel::Trace)};
            std::atomic<std::uint64_t>             seq{0};
        };

        State& state()
        {
            static State s;   // function-local: alive for any static-dtor logger
            return s;
        }

        std::uint32_t currentTid() noexcept
        {
            // 只求「同线程恒等、异线程几乎不同」—— 显示/分组用,非 OS 句柄。
            return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(
                std::this_thread::get_id()));
        }
    } // namespace

    const char* levelTag(ELevel lv) noexcept
    {
        switch (lv)
        {
        case ELevel::Trace: return "trace";
        case ELevel::Info:  return "info";
        case ELevel::Warn:  return "warn";
        case ELevel::Error: return "error";
        }
        return "?";
    }

    void setOutput(OutputFn fn)
    {
        auto& s = state();
        std::lock_guard lock(s.admin);
        if (fn)
        {
            auto owned = std::make_unique<OutputFn>(std::move(fn));
            const OutputFn* raw = owned.get();
            s.retired.push_back(std::move(owned));
            s.output.store(raw, std::memory_order_release);
        }
        else
        {
            s.output.store(nullptr, std::memory_order_release);
        }
    }

    void setMinLevel(ELevel lv) noexcept
    {
        state().min_level.store(static_cast<std::uint8_t>(lv),
                                std::memory_order_relaxed);
    }

    ELevel minLevel() noexcept
    {
        return static_cast<ELevel>(
            state().min_level.load(std::memory_order_relaxed));
    }

    std::size_t formatRecord(const LogRecord& r, char* out, std::size_t cap) noexcept
    {
        if (r.truncated)
        {
            // 参数区不完整:解码不可靠 —— 打印格式串原文 + 记号,信息降级但
            // 不静默、不崩(232B 之内的正常日志永远到不了这里)。
            const std::string_view fmt(r.format, r.format_len);
            const std::size_t      n = fmt.size() < cap ? fmt.size() : cap;
            std::memcpy(out, fmt.data(), n);
            constexpr std::string_view kNote = " <args truncated>";
            const std::size_t m = (cap - n) < kNote.size() ? (cap - n) : kNote.size();
            std::memcpy(out + n, kNote.data(), m);
            return n + m;
        }

        struct Ctx
        {
            char*       out;
            std::size_t cap;
            std::size_t len;
        } ctx{out, cap, 0};

        r.decode(r,
                 +[](const LogRecord& rec, std::format_args args, void* user)
                 {
                     auto& c = *static_cast<Ctx*>(user);
                     c.len = std::vformat_to(
                         BufSink{c.out, c.cap},
                         std::string_view(rec.format, rec.format_len),
                         args
                     ).written();
                 },
                 &ctx);
        return ctx.len < cap ? ctx.len : cap;
    }

    void writeRecordToStderr(const LogRecord& r) noexcept
    {
        char        buf[1024];
        const auto  len = formatRecord(r, buf, sizeof(buf));
        // One fprintf per record: stdio's internal lock keeps concurrent
        // records line-atomic. Line shape shared by every host terminal
        // outlet — byte-identical to the retired StderrSink.
        std::fprintf(stderr, "[%s][%s] %.*s\n", levelTag(r.level),
                     r.category ? r.category : "?",
                     static_cast<int>(len), buf);
    }

#if defined(__ANDROID__)
    void writeRecordToLogcat(const LogRecord& r, const char* tag) noexcept
    {
        char       buf[1024];
        const auto len = formatRecord(r, buf, sizeof(buf));
        int        prio = ANDROID_LOG_INFO;
        switch (r.level)
        {
        case ELevel::Trace: prio = ANDROID_LOG_VERBOSE; break;
        case ELevel::Info:  prio = ANDROID_LOG_INFO;    break;
        case ELevel::Warn:  prio = ANDROID_LOG_WARN;    break;
        case ELevel::Error: prio = ANDROID_LOG_ERROR;   break;
        }
        __android_log_print(prio, tag ? tag : "lux", "[%s] %.*s",
                            r.category ? r.category : "?",
                            static_cast<int>(len), buf);
    }
#endif

    void emitRecord(LogRecord& r) noexcept
    {
        auto& s = state();
        r.tid   = currentTid();
        r.seq   = s.seq.fetch_add(1, std::memory_order_relaxed) + 1;
        r.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

        if (const OutputFn* out = s.output.load(std::memory_order_acquire))
        {
            (*out)(r);
            return;
        }
        // Host never assembled an output (CLI helper, early startup, tests,
        // post-shutdown tail): stderr fallback — losing diagnostics silently
        // is the exact failure family this service exists to end.
        writeRecordToStderr(r);
    }

    void vlog(ELevel lv, const char* category,
              std::string_view fmt, std::format_args args) noexcept
    {
        if (static_cast<std::uint8_t>(lv) <
            static_cast<std::uint8_t>(minLevel()))
            return;

        // format_args 是栈上实参的类型擦除引用,不能跨线程/延迟(档2 的硬
        // 约束)—— 这里就地格式化,把**文本**作为预格式化记录发出去。
        char        buf[1024];
        std::size_t len = std::vformat_to(
            BufSink{buf, sizeof(buf)},
            fmt,
            args
        ).written();

        using Codec = detail::Codec<std::string_view>;
        // 预格式化文本超过记录容量时截文本本体(留完整前缀)——比置 truncated
        // 走「格式串原文」降级路径丢掉整条消息强。
        constexpr std::size_t kMaxText = kLogArgCapacity - sizeof(std::uint16_t);
        if (len > kMaxText)
            len = kMaxText;
        LogRecord r;
        r.format     = "{}";
        r.format_len = 2;
        r.category   = category ? category : "?";
        r.decode     = &Codec::decode;
        r.level      = lv;
        r.truncated  = false;
        r.arg_bytes  = 0;
        Codec::pack(r, std::string_view(buf, len));
        emitRecord(r);
    }

} // namespace lux::log
