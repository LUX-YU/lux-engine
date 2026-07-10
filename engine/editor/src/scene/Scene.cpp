#include <lux/engine/editor/scene/Scene.hpp>

#include <lux/engine/serialize/Archive.hpp>
#include <lux/engine/serialize/NameTable.hpp>
#include <lux/engine/serialize/TaggedPropertyArchive.hpp>

#include <lux/engine/ecs/ComponentTypeRegistry.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>  // repairHierarchyCycles (load-time hierarchy guard)
#include <lux/engine/meta/LuxObject.hpp>  // EntityRegistry
#include <lux/engine/meta/Meta.hpp>       // RefClass

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        /// Mint a random v4 UUID for fresh scenes. Mirrors the helper in
        /// ProjectManifest.cpp; small duplication beats a circular dep.
        uuids::uuid makeRandomUuid()
        {
            std::random_device rd;
            std::array<int, std::mt19937::state_size> seed_data{};
            std::generate(seed_data.begin(), seed_data.end(), std::ref(rd));
            std::seed_seq seq(seed_data.begin(), seed_data.end());
            std::mt19937 gen(seq);
            uuids::uuid_random_generator g{gen};
            return g();
        }

        /// Read an entire file into a byte buffer. Used by the loader
        /// instead of streaming because scene files are small (kB-MB) and
        /// random access into NameTable + entity data is simpler with the
        /// whole file in memory.
        lux::cxx::expected<std::vector<std::byte>, std::string>
            readFileToBytes(const std::filesystem::path& path)
        {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f.is_open())
                return lux::cxx::unexpected(
                    std::string{"failed to open '"} + path.string() + "'");

            const auto sz = static_cast<std::size_t>(f.tellg());
            f.seekg(0, std::ios::beg);

            std::vector<std::byte> buf(sz);
            if (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()),
                                  static_cast<std::streamsize>(sz)))
                return lux::cxx::unexpected(
                    std::string{"failed to read '"} + path.string() + "'");
            return buf;
        }

        /// Write bytes to a temp file then atomically rename onto the
        /// target path. Mirrors the pattern ProjectManifest::saveToFile
        /// uses — avoids leaving a corrupted scene on partial writes.
        lux::cxx::expected<void, std::string>
            atomicWrite(const std::filesystem::path& path,
                        const std::vector<std::byte>& bytes)
        {
            std::error_code ec;
            if (auto parent = path.parent_path();
                !parent.empty() && !std::filesystem::exists(parent))
            {
                std::filesystem::create_directories(parent, ec);
                if (ec)
                    return lux::cxx::unexpected(
                        std::string{"failed to create parent of '"} +
                        path.string() + "': " + ec.message());
            }

            auto tmp = path;
            tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f.is_open())
                    return lux::cxx::unexpected(
                        std::string{"failed to open '"} + tmp.string() +
                        "' for writing");
                if (!bytes.empty() &&
                    !f.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size())))
                    return lux::cxx::unexpected(
                        std::string{"failed to write '"} + tmp.string() + "'");
            }

            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                std::filesystem::remove(tmp, ec);
                return lux::cxx::unexpected(
                    std::string{"failed to replace '"} + path.string() +
                    "': " + ec.message());
            }
            return {};
        }

        constexpr std::uint32_t kNoActiveCamera = 0xFFFFFFFFu;
    } // namespace

    // ─────────────────────────────────────────────────────────────────
    //  Scene::save
    // ─────────────────────────────────────────────────────────────────

    lux::cxx::expected<void, std::string>
    Scene::save(const std::filesystem::path& path,
                lux::meta::EntityRegistry&   registry,
                const SceneSaveOptions&      opts)
    {
        using namespace lux::serialize;

        // ── Phase 1: serialise entity data into a temp buffer, while
        //    interning component-type / field names into the shared
        //    NameTable. We can't write the NameTable to disk yet because
        //    we don't know all its strings until phase 1 is done. ──
        std::vector<std::byte> entity_bytes;
        NameTable              names;
        std::uint32_t          active_camera_index = kNoActiveCamera;

        const auto& catalogue = lux::ecs::ComponentTypeRegistry::instance().all();

        // First, snapshot the entity list. entt's `registry.view<entity>()`
        // returns all live entities; iterating it is stable for the
        // duration of this function.
        std::vector<entt::entity> entities;
        for (auto e : registry.view<entt::entity>())
            entities.push_back(e);

        {
            ArchiveWriter aw(entity_bytes);
            aw.writePod(static_cast<std::uint32_t>(entities.size()));

            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const entt::entity e = entities[i];
                if (e == opts.active_camera)
                    active_camera_index = static_cast<std::uint32_t>(i);

                // Serialised handle: opaque opaque entt::entity bit-cast.
                // The loader builds a mapping `serialized → live` for any
                // future intra-scene-reference support; for now we just
                // round-trip the bits.
                aw.writePod(static_cast<std::uint64_t>(e));

                // Gather components present on this entity and remember
                // pointers to their ComponentTypeEntry so we don't have
                // to find them twice.
                std::vector<const lux::ecs::ComponentTypeEntry*> present;
                for (const auto& entry : catalogue)
                {
                    if (entry.has && entry.has(registry, e))
                        present.push_back(&entry);
                }
                aw.writePod(static_cast<std::uint32_t>(present.size()));

                for (const auto* entry : present)
                {
                    const auto type_name_idx = names.intern(entry->full_name);
                    aw.writePod(type_name_idx);
                    const std::size_t size_off    = aw.reserveU32();
                    const std::size_t body_start  = aw.tell();

                    if (entry->ref_class && entry->get)
                    {
                        void* comp = entry->get(registry, e);
                        if (comp)
                        {
                            TaggedPropertyWriter w(aw, names);
                            w.writeObject(*entry->ref_class, comp);
                        }
                    }

                    const std::size_t body_end = aw.tell();
                    aw.patchU32At(size_off,
                        static_cast<std::uint32_t>(body_end - body_start));
                }
            }

            aw.writePod(active_camera_index);
        }

        // ── Phase 2: serialise NameTable into its own buffer. ──
        std::vector<std::byte> name_bytes;
        {
            ArchiveWriter nw(name_bytes);
            names.serialize(nw);
        }

        // ── Phase 3: assemble [header][name_table][entity_data]. ──
        SceneFileHeader header{};
        header.magic              = kSceneMagic;
        header.format_version     = kSceneFormatVersion;
        header.scene_guid         = opts.scene_guid.is_nil() ? makeRandomUuid()
                                                              : opts.scene_guid;
        header.name_table_offset  = sizeof(SceneFileHeader);
        header.name_table_size    = name_bytes.size();
        header.entity_data_offset = header.name_table_offset + header.name_table_size;
        header.entity_data_size   = entity_bytes.size();

        std::vector<std::byte> file;
        file.reserve(sizeof(SceneFileHeader) + name_bytes.size() + entity_bytes.size());
        file.resize(sizeof(SceneFileHeader));
        std::memcpy(file.data(), &header, sizeof(SceneFileHeader));
        file.insert(file.end(), name_bytes.begin(), name_bytes.end());
        file.insert(file.end(), entity_bytes.begin(), entity_bytes.end());

        return atomicWrite(path, file);
    }

    // ─────────────────────────────────────────────────────────────────
    //  Scene::load
    // ─────────────────────────────────────────────────────────────────

    lux::cxx::expected<void, std::string>
    Scene::load(const std::filesystem::path& path,
                lux::meta::EntityRegistry&   registry,
                SceneLoadResult&             out)
    {
        using namespace lux::serialize;

        auto file = readFileToBytes(path);
        if (!file)
            return lux::cxx::unexpected(std::move(file.error()));

        if (file->size() < sizeof(SceneFileHeader))
            return lux::cxx::unexpected(
                std::string{"scene file too small: '"} + path.string() + "'");

        SceneFileHeader header{};
        std::memcpy(&header, file->data(), sizeof(SceneFileHeader));
        if (header.magic != kSceneMagic)
            return lux::cxx::unexpected(
                std::string{"bad scene magic in '"} + path.string() + "'");
        if (header.format_version != kSceneFormatVersion)
        {
            std::ostringstream ss;
            ss << "unsupported scene format_version=" << header.format_version
               << " in '" << path.string() << "' (want " << kSceneFormatVersion
               << ")";
            return lux::cxx::unexpected(ss.str());
        }
        if (header.name_table_offset + header.name_table_size > file->size() ||
            header.entity_data_offset + header.entity_data_size > file->size())
            return lux::cxx::unexpected(
                std::string{"scene file truncated: '"} + path.string() + "'");

        out.scene_guid = header.scene_guid;
        out.active_camera = entt::null;
        out.created_entities.clear();

        // ── Phase 1: read NameTable. ──
        NameTable names;
        {
            ArchiveReader nr(file->data() + header.name_table_offset,
                             header.name_table_size);
            names = NameTable::deserialize(nr);
        }

        // ── Phase 2: build full_name → ComponentTypeEntry lookup for
        //    constant-time dispatch in the entity loop. ──
        const auto& catalogue = lux::ecs::ComponentTypeRegistry::instance().all();
        std::unordered_map<std::string_view, const lux::ecs::ComponentTypeEntry*> by_name;
        by_name.reserve(catalogue.size());
        for (const auto& e : catalogue)
            by_name.emplace(e.full_name, &e);

        // Alarm signal: an empty catalogue means the reflection sidecar
        // never registered any component. Loading will silently drop every
        // component payload below, leaving entities that look valid but own
        // nothing — and the editor will crash the moment it dereferences an
        // expected component (e.g. the camera's TransformComponent). The
        // usual root cause is a stale `install/.../MetaAnnotations.hpp`
        // shadowing a freshly added LUX_ macro at meta_generator parse time.
        if (by_name.empty())
        {
            std::fprintf(stderr,
                "[Scene::load] WARNING component-type catalogue is EMPTY — "
                "ComponentTypeRegistry has no registered components. "
                "Every component in '%s' will be skipped. "
                "(Did the gameplay_meta sidecar load? Are the LUX_ macros "
                "visible to meta_generator — i.e. is install/.../MetaAnnotations.hpp "
                "up to date?)\n",
                path.string().c_str());
        }

        // ── Phase 3: read entity data. ──
        ArchiveReader ar(file->data() + header.entity_data_offset,
                         header.entity_data_size);

        const auto entity_count = ar.readPod<std::uint32_t>();
        out.created_entities.reserve(entity_count);

        for (std::uint32_t i = 0; i < entity_count; ++i)
        {
            // Round-trip the serialized handle; presently unused (no
            // intra-scene refs yet) but reserved for later — emit a void
            // cast so compilers don't warn unused.
            const auto serialized_handle = ar.readPod<std::uint64_t>();
            (void)serialized_handle;

            const auto live = registry.create();
            out.created_entities.push_back(live);

            const auto component_count = ar.readPod<std::uint32_t>();
            for (std::uint32_t c = 0; c < component_count; ++c)
            {
                const auto type_name_idx = ar.readPod<std::uint32_t>();
                const auto payload_size  = ar.readPod<std::uint32_t>();

                const std::string_view type_name = names.at(type_name_idx);

                auto it = by_name.find(type_name);
                if (it == by_name.end() || !it->second->ref_class ||
                    !it->second->emplace)
                {
                    // Component unknown to this build (removed type, plugin
                    // not loaded, missing reflection sidecar, etc.). Warn
                    // once per type rather than per-occurrence so a stripped
                    // build does not flood stderr.
                    static std::unordered_set<std::string> warned;
                    std::string key{type_name};
                    if (warned.insert(key).second)
                    {
                        std::fprintf(stderr,
                            "[Scene::load] WARNING component type '%s' is not "
                            "in the runtime catalogue — payload will be skipped "
                            "for every entity that has it. "
                            "(found=%d ref_class=%p emplace=%p)\n",
                            key.c_str(),
                            (int)(it != by_name.end()),
                            (const void*)(it == by_name.end() ? nullptr : it->second->ref_class),
                            (const void*)(it == by_name.end() ? nullptr : (const void*)it->second->emplace));
                    }
                    ar.skip(payload_size);
                    continue;
                }

                void* comp = it->second->emplace(registry, live);
                if (!comp)
                {
                    std::fprintf(stderr,
                        "[Scene::load] WARNING entity_idx=%u emplace('%.*s') "
                        "returned null — payload skipped.\n",
                        i, (int)type_name.size(), type_name.data());
                    ar.skip(payload_size);
                    continue;
                }

                // Bound the tagged-property read to the declared payload
                // size: if the writer's schema had extra fields the reader
                // doesn't know, we still want to consume exactly
                // payload_size bytes so the next component header lines up.
                const std::size_t before = ar.tell();
                TaggedPropertyReader r(ar, names);
                r.readObject(*it->second->ref_class, comp);
                const std::size_t after = ar.tell();
                if (after - before < payload_size)
                    ar.skip(payload_size - (after - before));
            }
        }

        const auto active_idx = ar.readPod<std::uint32_t>();
        if (active_idx != kNoActiveCamera &&
            active_idx < out.created_entities.size())
            out.active_camera = out.created_entities[active_idx];

        // Entry-point hierarchy guard (2026-07-06 ruling: entries validate, the
        // per-frame system does not pay for malformed data): a file whose parent
        // links form a cycle — hand-edit, merge conflict, version skew — is
        // repaired here (cycle members become roots, still visible/selectable)
        // with a loud warning, instead of surfacing as excluded entities later.
        if (const auto repaired = lux::ecs::repairHierarchyCycles(registry); repaired != 0)
            std::fprintf(stderr,
                "[Scene::load] WARNING %zu entity(ies) sat on a parent CYCLE — their "
                "parent links were removed (now roots). Re-parent them and re-save '%s'.\n",
                repaired, path.string().c_str());

        return {};
    }

} // namespace lux::editor
