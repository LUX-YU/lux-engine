#pragma once
/**
 * @file VisualSoak.hpp
 * @brief Shared unattended-run protocol for windowed visual demos.
 *
 * The demos remain interactive by default. `--soak <seconds>` gives local
 * automation a deterministic, graceful exit without a timeout or process
 * kill. Parsing happens before a window or Vulkan object is created, so a
 * malformed invocation fails fast instead of accidentally entering the
 * unbounded interactive loop.
 */

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <system_error>

namespace lux::rendertest
{
    class VisualSoak final
    {
    public:
        using Clock = std::chrono::steady_clock;

        /**
         * Parse the complete visual-demo argument surface.
         *
         * No arguments selects interactive mode. The only accepted option is
         * `--soak <finite-positive-seconds>`. Diagnostics go to stderr and a
         * false result maps to process exit code 2 at each executable boundary.
         */
        [[nodiscard]] static bool parse(int argc, char* const argv[], VisualSoak& out) noexcept
        {
            out = {};
            bool seen_soak = false;

            for (int i = 1; i < argc; ++i)
            {
                const std::string_view argument = argv[i] ? argv[i] : "";
                if (argument != "--soak")
                {
                    std::fprintf(
                        stderr,
                        "unexpected argument '%.*s'; usage: %s "
                        "[--soak <seconds>]\n",
                        static_cast<int>(argument.size()),
                        argument.data(),
                        programName(argc, argv)
                    );
                    return false;
                }
                if (seen_soak)
                {
                    std::fprintf(stderr, "--soak may be specified only once\n");
                    return false;
                }
                if (++i >= argc || argv[i] == nullptr)
                {
                    std::fprintf(stderr, "--soak requires a positive number of seconds\n");
                    return false;
                }

                const std::string_view value_text{argv[i]};
                double seconds = 0.0;
                const auto parsed = std::from_chars(
                    value_text.data(),
                    value_text.data() + value_text.size(),
                    seconds,
                    std::chars_format::general
                );
                const bool is_parse_error = parsed.ec != std::errc{};
                const bool has_trailing_characters = parsed.ptr != value_text.data() + value_text.size();
                const bool is_non_finite = !std::isfinite(seconds);
                const bool is_non_positive = seconds <= 0.0;
                const bool is_invalid_value = is_parse_error || has_trailing_characters || is_non_finite ||
                    is_non_positive;
                if (is_invalid_value)
                {
                    std::fprintf(
                        stderr,
                        "invalid --soak value '%.*s'; expected finite seconds "
                        "greater than zero\n",
                        static_cast<int>(value_text.size()),
                        value_text.data()
                    );
                    return false;
                }

                out.seconds_ = seconds;
                seen_soak = true;
            }
            return true;
        }

        [[nodiscard]] bool enabled() const noexcept
        {
            return seconds_ > 0.0;
        }

        [[nodiscard]] double seconds() const noexcept
        {
            return seconds_;
        }

        [[nodiscard]] bool reached(Clock::time_point start, Clock::time_point now) const noexcept
        {
            return enabled() && std::chrono::duration<double>(now - start).count() >= seconds_;
        }

        void
        reportGracefulTeardown(Clock::time_point start, Clock::time_point now, std::uint64_t frame_count) const noexcept
        {
            const double elapsed = std::chrono::duration<double>(now - start).count();
            std::printf(
                "soak duration reached: %.2f s, %llu frames; "
                "beginning graceful teardown\n",
                elapsed,
                static_cast<unsigned long long>(frame_count)
            );
        }

    private:
        [[nodiscard]] static const char* programName(int argc, char* const argv[]) noexcept
        {
            return argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "visual-demo";
        }

        double seconds_{0.0};
    };
} // namespace lux::rendertest
