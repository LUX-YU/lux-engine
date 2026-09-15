#include <cassert>
#include <cstdio>
#include <deque>
#include <list>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/simulation/ecs/ComponentDecode.hpp>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace lux::serialization;
constexpr SerializationBudget budget{1024 * 1024, 4096, 32};

template <class T> std::vector<std::byte> encode(const T &value)
{
    std::vector<std::byte> bytes;
    BinaryWriter writer(bytes);
    assert(write(writer, value, budget));
    return bytes;
}

template <class T> void roundtrip(const T &expected)
{
    static_assert(lux::simulation::ecs::detail::directMaterializableValue<T>());
    const auto bytes = encode(expected);
    BinaryReader reader(bytes);
    T actual{};
    assert(read(reader, actual, budget));
    assert(lux::editor::scene::FieldValue<T>::equal(expected, actual));
    assert(reader.offset() == bytes.size());
    assert(lux::editor::scene::FieldValue<T>::valid(actual));
    assert(lux::editor::scene::FieldValue<T>::bytes(actual) >= sizeof(T));
    if (!bytes.empty())
    {
        BinaryReader truncated(std::span(bytes).first(bytes.size() - 1));
        assert(!read(truncated, actual, budget));
        assert(lux::editor::scene::FieldValue<T>::equal(expected, actual));
    }
}

int main()
{
    roundtrip(std::vector<bool>{true, false, true});
    roundtrip(std::deque<std::string>{"Unicode 中文", "second"});
    roundtrip(std::list<std::vector<int>>{{1, 2}, {3, 4}});
    roundtrip(std::map<std::string, std::vector<double>>{{"first", {1.5, 2.5}}, {"next", {3.5}}});
    roundtrip(std::unordered_map<std::string, std::list<int>>{{"first", {1, 2}}, {"next", {3}}});
    roundtrip(std::set<std::string>{"alpha", "beta"});
    roundtrip(std::unordered_set<int>{5, 3, 7});
    roundtrip(std::variant<std::monostate, int, int, std::vector<std::string>>{std::in_place_index<3>,
                                                                               std::vector<std::string>{"one", "two"}});
    roundtrip(std::variant<int, int>{std::in_place_index<1>, 73});

    const auto duplicate = encode(std::vector<std::pair<std::string, int>>{{"duplicate", 1}, {"duplicate", 2}});
    BinaryReader duplicate_reader(duplicate);
    std::map<std::string, int> retained{{"original", 9}};
    const auto rejected = read(duplicate_reader, retained, budget);
    assert(!rejected && rejected.error().code == ESerializationError::INVALID_VALUE);
    assert((retained == std::map<std::string, int>{{"original", 9}}));

    const auto invalid_tag = encode(std::uint32_t{5});
    BinaryReader invalid_reader(invalid_tag);
    std::variant<int, std::string> retained_variant{42};
    const auto tag = read(invalid_reader, retained_variant, budget);
    assert(!tag && tag.error().code == ESerializationError::INVALID_VALUE);
    assert(std::get<int>(retained_variant) == 42);
    std::puts("PASS common-container-codecs: nested values, vector<bool>, repeated variant alternatives, duplicate "
              "keys, truncated input retention");
}
