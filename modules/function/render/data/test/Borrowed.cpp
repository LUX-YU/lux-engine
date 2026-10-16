#include "Fixtures.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace lux::render;
static void check(bool x) { if (!x) std::abort(); }
int main()
{
    const EncodedDataLimits limits{1024, 1024};
    std::array<std::byte, 128> storage;
    fixture::Record input{42, {1, 2, 3}, .25f, true, 3};
    auto size = encodeDataInto(input, storage, {}, limits);
    check(bool(size) && *size == 16);
    auto owned = encodeData(input, {}, limits);
    check(bool(owned) && std::ranges::equal(owned->view(), std::span(storage).first(*size)));
    check(!encodeDataInto(input, std::span(storage).first(15), {}, limits));
    for (auto fault : {fixture::ECodecFault::SIZE, fixture::ECodecFault::SHORT_WRITE,
        fixture::ECodecFault::LONG_WRITE, fixture::ECodecFault::ENCODE_ERROR})
    {
        fixture::codec_fault = fault;
        check(!encodeDataInto(input, storage, {}, limits));
    }
    fixture::codec_fault = fixture::ECodecFault::NONE;
    const std::array<std::byte, 8> blob{};
    const std::array owners{lux::cxx::SharedBytes<>::copyOf(blob)};
    const std::array views{owners[0].view()};
    fixture::Payload value{owners[0], {0, 0, 8}, owners[0]};
    auto encoded = encodeData(value, owners, limits);
    check(bool(encoded));
    const auto& schema = DataSchema<fixture::Payload>::describe();
    std::vector<std::byte> changed(encoded->view().begin(), encoded->view().end());
    for (std::size_t position = 0; position < changed.size(); ++position)
    {
        const auto original = changed[position];
        changed[position] = std::byte{0xff};
        auto pinned = lux::cxx::SharedBytes<>::copyOf(changed);
        auto old_result = validateEncodedData(RetainableEncodedDataView{schema, pinned, owners}, limits);
        auto borrowed = validateEncodedData(EncodedDataView{schema, changed, views}, limits);
        check(bool(old_result) == bool(borrowed));
        if (!old_result) check(old_result.error().type == borrowed.error().type);
        changed[position] = original;
    }
    check(!validateEncodedData(EncodedDataView{schema, encoded->view(), {}}, limits));
    check(!validateEncodedData(EncodedDataView{schema, encoded->view(), views}, {1024, 4}));
    check(!validateEncodedData(EncodedDataView{schema, encoded->view().first(35), views}, limits));
    changed.push_back(std::byte{});
    check(!validateEncodedData(EncodedDataView{schema, changed, views}, limits));
    auto bad_schema = schema;
    bad_schema.alignment = 3;
    check(!validateEncodedData(EncodedDataView{bad_schema, encoded->view(), views}, limits));
    std::printf("borrowed/owned validation parity; bounded span encoder failures; no fabricated Owner\n");
}
