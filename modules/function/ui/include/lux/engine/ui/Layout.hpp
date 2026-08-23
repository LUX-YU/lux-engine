#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <lux/cxx/compile_time/expected.hpp>

namespace lux::ui
{
	enum class ELayoutError
	{
		INVALID_DATA
	};

	class LayoutSnapshot final
	{
	public:
		LayoutSnapshot() noexcept = default;
		LayoutSnapshot(const LayoutSnapshot &) = delete;
		LayoutSnapshot &operator=(const LayoutSnapshot &) = delete;
		LayoutSnapshot(LayoutSnapshot &&) noexcept = default;
		LayoutSnapshot &operator=(LayoutSnapshot &&) noexcept = default;

		[[nodiscard]] static lux::cxx::expected<LayoutSnapshot, ELayoutError>
		fromBytes(std::span<const std::byte> bytes)
		{
			if (bytes.empty())
				return lux::cxx::unexpected(ELayoutError::INVALID_DATA);
			return LayoutSnapshot{std::vector<std::byte>{bytes.begin(), bytes.end()}};
		}

		[[nodiscard]] std::span<const std::byte> bytes() const noexcept
		{
			return bytes_;
		}

	private:
		friend class UISession;
		explicit LayoutSnapshot(std::vector<std::byte> bytes) noexcept
			: bytes_(std::move(bytes)) {}

		std::vector<std::byte> bytes_;
	};
} // namespace lux::ui
