#include <lux/engine/render/resources/mesh/StableRecordPages.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

int main()
{
    using lux::render::StableRecordPages;

    constexpr std::size_t kPageSize = 4096u;
    constexpr std::size_t kRecordCount = 100'000u;
    StableRecordPages<std::uint64_t, kPageSize> records;
    records.reserve(kRecordCount);

    records.push_back(0x1234u);
    const auto* first_record = &records[0u];
    for (std::size_t i = 1u; i < kRecordCount; ++i)
        records.push_back(static_cast<std::uint64_t>(i));

    assert(records.size() == kRecordCount);
    assert(records.pageCount() ==
        (kRecordCount + kPageSize - 1u) / kPageSize);
    assert(records.capacity() >= kRecordCount);
    assert(&records[0u] == first_record);
    assert(records[0u] == 0x1234u);
    assert(records[kPageSize] == kPageSize);
    assert(records[kRecordCount - 1u] == kRecordCount - 1u);
    return 0;
}
