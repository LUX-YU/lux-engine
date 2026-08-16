//=============================================================================
// mesh_asset_test.cpp
// -----------------------------------------------------------------------------
// Acceptance test for MeshSerDeser persistence (mesh .luxasset).
//
// Always-on, no external assets needed — exercises the MeshDescriptionCodec
// directly (the SerDeser wrapper is a thin header+blob copy of the proven
// SkeletonSerDeser):
//   * round-trip with skinned vertices + indices + bounds (bit-for-bit)
//   * round-trip with no bounds (std::optional stays empty)
//   * round-trip of an empty mesh (header+trailer only)
//   * rubbish input is rejected (bad magic)
//=============================================================================

#include <lux/engine/resource/asset/MeshAsset.hpp>            // lux::asset::{Mesh,Vertex,Bone}
#include <lux/engine/resource/asset/MeshDescriptionCodec.hpp> // detail::{encode,decode}MeshDescription
#include <lux/engine/resource/asset/MeshSerDeser.hpp>

#include <uuid.h>

#include <cstring>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace lux::asset;
using lux::rdesc::Mesh;
using lux::rdesc::Vertex;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& desc)
{
    if (cond) { std::cout << "  PASS  " << desc << "\n"; ++g_pass; }
    else      { std::cout << "  FAIL  " << desc << "\n"; ++g_fail; }
}

static void banner(const char* title)
{
    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  " << title << "\n"
              << std::string(60, '=') << "\n";
}

//-----------------------------------------------------------------------------
static Mesh makeTestMesh(bool with_bounds)
{
    Mesh m;
    for (int i = 0; i < 3; ++i)
    {
        Vertex v{};                              // value-init: zero all bytes first
        v.position  = Eigen::Vector3f(0.1f * i, 0.2f * i, 0.3f * i);
        v.normal    = Eigen::Vector3f(0.f, 1.f, 0.f);
        v.tangent   = Eigen::Vector3f(1.f, 0.f, 0.f);
        v.uv        = Eigen::Vector2f(0.25f * i, 0.5f * i);
        v.bitangent = Eigen::Vector3f(0.f, 0.f, 1.f);
        v.bone.bone_ids[0] = i;   v.bone.bone_ids[1] = i + 1;
        v.bone.bone_ids[2] = -1;  v.bone.bone_ids[3] = -1;
        v.bone.weights[0]  = 0.6f; v.bone.weights[1] = 0.4f;
        v.bone.weights[2]  = 0.0f; v.bone.weights[3] = 0.0f;
        m.vertices.push_back(v);
    }
    m.indices = { 0u, 1u, 2u };
    if (with_bounds)
        m.bounds = lux::math::AABB(Eigen::Vector3f(-1.f, -2.f, -3.f),
                                   Eigen::Vector3f( 4.f,  5.f,  6.f));
    return m;
}

//-----------------------------------------------------------------------------
static void test_mesh_roundtrip()
{
    banner("Mesh codec round-trip (skinned verts + indices + bounds)");
    Mesh in = makeTestMesh(/*with_bounds=*/true);

    auto blob = detail::encodeMeshDescription(in);
    check(!blob.empty(), "encoded blob non-empty");

    Mesh out;
    std::string err;
    bool ok = detail::decodeMeshDescription(std::span<const std::byte>(blob), out, &err);
    check(ok, "decode succeeded (" + err + ")");
    check(out.vertices.size() == in.vertices.size(), "vertex count preserved");
    check(out.indices.size()  == in.indices.size(),  "index count preserved");

    if (out.vertices.size() == in.vertices.size() && !in.vertices.empty())
        check(std::memcmp(out.vertices.data(), in.vertices.data(),
                          in.vertices.size() * sizeof(Vertex)) == 0,
              "vertices bit-for-bit equal (incl. bone ids/weights)");
    if (out.indices.size() == in.indices.size() && !in.indices.empty())
        check(std::memcmp(out.indices.data(), in.indices.data(),
                          in.indices.size() * sizeof(std::uint32_t)) == 0,
              "indices equal");

    check(out.bounds.has_value(), "bounds present");
    if (out.bounds.has_value() && in.bounds.has_value())
        check(out.bounds->min == in.bounds->min && out.bounds->max == in.bounds->max,
              "bounds min/max equal");
}

static void test_mesh_no_bounds()
{
    banner("Mesh codec round-trip (no bounds)");
    Mesh in = makeTestMesh(/*with_bounds=*/false);
    in.bounds.reset();

    auto blob = detail::encodeMeshDescription(in);
    Mesh out;
    std::string err;
    bool ok = detail::decodeMeshDescription(std::span<const std::byte>(blob), out, &err);
    check(ok, "decode succeeded (" + err + ")");
    check(!out.bounds.has_value(), "bounds remains nullopt");
    check(out.vertices.size() == in.vertices.size(), "vertex count preserved");
}

static void test_empty_mesh()
{
    banner("Mesh codec round-trip (empty mesh)");
    Mesh in;  // no verts, no indices, no bounds

    auto blob = detail::encodeMeshDescription(in);
    check(!blob.empty(), "empty mesh still encodes header+trailer");

    Mesh out;
    std::string err;
    bool ok = detail::decodeMeshDescription(std::span<const std::byte>(blob), out, &err);
    check(ok, "decode succeeded (" + err + ")");
    check(out.vertices.empty() && out.indices.empty() && !out.bounds.has_value(),
          "empty mesh round-trips empty");
}

static void test_bad_blob()
{
    banner("Mesh codec rejects rubbish");
    std::vector<std::byte> garbage(64, std::byte{ 0xAB });
    Mesh out;
    std::string err;
    bool fails = !detail::decodeMeshDescription(std::span<const std::byte>(garbage), out, &err);
    check(fails, "decode rejects rubbish input (bad magic)");
}

static void test_mesh_lods()
{
    banner("Mesh codec round-trip (v2 LOD chain)");
    Mesh in = makeTestMesh(/*with_bounds=*/true);
    in.indices = { 0u, 1u, 2u, 0u, 2u, 1u };          // LOD0 (6 indices)
    in.lods.push_back({ { 0u, 1u, 2u }, 0.10f });     // LOD1 (3)
    in.lods.push_back({ { 0u, 2u },     0.30f });     // LOD2 (2)

    auto blob = detail::encodeMeshDescription(in);
    Mesh out;
    std::string err;
    bool ok = detail::decodeMeshDescription(std::span<const std::byte>(blob), out, &err);
    check(ok, "decode succeeded (" + err + ")");
    check(out.indices == in.indices, "LOD0 indices preserved");
    check(out.lods.size() == in.lods.size(), "LOD count preserved");
    if (out.lods.size() == in.lods.size())
    {
        bool all = true;
        for (size_t i = 0; i < in.lods.size(); ++i)
            all = all && (out.lods[i].indices == in.lods[i].indices)
                      && (out.lods[i].error   == in.lods[i].error);
        check(all, "each LOD's indices + error round-trip exactly");
    }
}

static void test_v1_tolerance()
{
    banner("Mesh codec tolerates legacy v1 blobs (no re-bake)");
    // Hand-craft a minimal v1 empty-mesh blob: the pre-LOD wire format
    // (magic, endian, version=1, vcount, icount, has_bounds, trailer).
    auto u32 = [](std::vector<std::byte>& b, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(std::byte((v >> (i * 8)) & 0xFFu));
    };
    std::vector<std::byte> blob;
    u32(blob, 0x48534D4Cu); // magic 'LMSH'
    u32(blob, 0x01020304u); // endian tag
    u32(blob, 1u);          // schema version 1 (legacy, no LOD block)
    u32(blob, 0u);          // vertex_count
    u32(blob, 0u);          // index_count
    u32(blob, 0u);          // has_bounds
    u32(blob, 0x45534D4Cu); // trailer 'LMSE'

    Mesh out;
    std::string err;
    bool ok = detail::decodeMeshDescription(std::span<const std::byte>(blob), out, &err);
    check(ok, "v1 blob decodes without error (" + err + ")");
    check(out.lods.empty(), "v1 mesh decodes as single-LOD (no lods)");
}

static void test_standard_generated_asset()
{
    banner("Standard generated Mesh asset is deterministic");
    const auto id = uuids::uuid::from_string(
        "76500000-0000-4000-8000-000000000001").value();
    const Mesh input = makeTestMesh(/*with_bounds=*/true);
    const auto first = MeshSerDeser::encodeData(id, input);
    const auto second = MeshSerDeser::encodeData(id, input);
    check(static_cast<bool>(first), "standard Mesh image encodes");
    check(first && second && *first == *second,
          "same id and geometry produce identical bytes");
    if (!first)
        return;
    const auto decoded = MeshSerDeser::decodeData(
        first->data(), first->size());
    check(static_cast<bool>(decoded), "standard Mesh image decodes");
    if (decoded)
    {
        check((*decoded)->vertices.size() == input.vertices.size(),
              "standard image vertex count preserved");
        check((*decoded)->indices == input.indices,
              "standard image indices preserved");
    }
}

//-----------------------------------------------------------------------------
int main()
{
    test_mesh_roundtrip();
    test_mesh_no_bounds();
    test_empty_mesh();
    test_bad_blob();
    test_mesh_lods();
    test_v1_tolerance();
    test_standard_generated_asset();

    std::cout << "\nmesh_asset_test: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
