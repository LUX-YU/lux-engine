// ============================================================================
//  reflection_drain_test — PERMANENT (cpu): the transactional incremental-
//  registration gate.
//
//  Guards ReflectionRegistry::drainPending(), the entry point a plugin host
//  calls after dlopen/LoadLibrary. Two properties, and the SECOND is the one
//  worth a test:
//
//    1. a module chained in AFTER init remains invisible while unpublished;
//    2. committing the draft makes it visible without losing old metadata;
//    3. a bad draft is discarded without contaminating the live registry.
//
//  (2) is the whole reason drainPending exists. The obvious-looking
//  alternative — calling meta_module_init() again — passes (1) and fails (2),
//  because initRegistry() unconditionally builds a NEW registry and drops the
//  old one on the floor. Nothing crashes; the catalogue just quietly shrinks.
//
//  Why that failure mode deserves a permanent gate: an unregistered component
//  type does not throw. Scene::load takes its "skip if type unknown" branch
//  for every component involved — the file loads, the entity count is right,
//  and every entity owns nothing. That is expensive to diagnose and trivial to
//  detect here.
// ============================================================================

#include <lux/engine/meta/Meta.hpp>

#include <cstdio>
#include <memory>

namespace
{
    enum class EByteBacked : std::uint8_t
    {
        VALUE
    };

    enum class ESignedBacked : std::int32_t
    {
        VALUE
    };

    static_assert(lux::meta::deduce_base<EByteBacked>() == lux::meta::EBaseType::Uint8);
    static_assert(lux::meta::deduce_base<ESignedBacked>() == lux::meta::EBaseType::Int32);
    static_assert(
        static_cast<lux::meta::EBaseType>(lux::meta::__builtin_qual_type<EByteBacked>.base) !=
        lux::meta::EBaseType::Unknown);

    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        if (ok)
        {
            std::printf("  ok   %s\n", what);
            return;
        }
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }

    /// Stands in for a plugin's generated `<module>_meta` function: the thing a
    /// MetaModuleRegistrar holds a pointer to.
    void
    pluginModuleRegister(lux::meta::ReflectionRegistry& registry, lux::meta::qual_type_index_fix_list& /*fix_list*/)
    {
        auto info = std::make_unique<lux::meta::RefClass>();
        info->name = "PluginType";
        info->full_name = "lux::test::PluginType";
        registry.registerClass(std::move(info));
    }
} // namespace

int
main()
{
    std::puts("=== reflection_drain_test ===");

    lux::meta::meta_module_init();
    check(lux::meta::ReflectionRegistry::initialized(), "registry initialised");

    // meta_module_init registers std::string as a special support type, so it
    // is a witness for "the pre-existing catalogue" that costs nothing to set
    // up and cannot be confused with the plugin's own type.
    const bool had_string = lux::meta::ReflectionRegistry::instance().findClass("std::string") != nullptr;
    check(had_string, "baseline type present after init");

    check(
        lux::meta::ReflectionRegistry::instance().findClass("lux::test::PluginType") == nullptr,
        "plugin type absent before its module is chained in"
    );

    {
        // Chaining happens in the constructor — this is exactly what a plugin's
        // file-scope static does when the dynamic linker loads it. Kept alive
        // across the drain because the registrar OWNS its list node.
        lux::meta::MetaModuleRegistrar plugin_registrar(&pluginModuleRegister);

        check(
            lux::meta::ReflectionRegistry::instance().findClass("lux::test::PluginType") == nullptr,
            "chaining alone registers nothing (constructor only queues)"
        );

        auto draft = lux::meta::meta_module_drain_draft();

        check(static_cast<bool>(draft), "plugin metadata collected into a valid draft");
        check(
            lux::meta::ReflectionRegistry::instance().findClass("lux::test::PluginType") == nullptr,
            "plugin type remains invisible before draft commit"
        );

        const auto committed = draft.commit();
        check(static_cast<bool>(committed), "plugin metadata draft committed");

        check(
            lux::meta::ReflectionRegistry::instance().findClass("lux::test::PluginType") != nullptr,
            "plugin type visible after draft commit"
        );
        // THE point of the test.
        check(
            lux::meta::ReflectionRegistry::instance().findClass("std::string") != nullptr,
            "baseline type SURVIVED the drain (a second init would have lost it)"
        );
    }

    {
        lux::meta::MetaModuleRegistrar duplicate_registrar(&pluginModuleRegister);
        auto duplicate = lux::meta::meta_module_drain_draft();

        check(!static_cast<bool>(duplicate), "duplicate reflected type rejects the whole draft");
        if (!duplicate)
        {
            check(
                duplicate.error().error == lux::meta::EReflectionRegistrationError::DUPLICATE_CLASS,
                "duplicate draft reports a structured error"
            );
        }
        check(
            lux::meta::ReflectionRegistry::instance().findClass("lux::test::PluginType") != nullptr,
            "discarded duplicate draft leaves the committed type intact"
        );
        check(
            lux::meta::ReflectionRegistry::instance().findClass("std::string") != nullptr,
            "discarded duplicate draft leaves baseline metadata intact"
        );
    }

    // Draining again with nothing pending must be a no-op, not a reset: hosts
    // will call this after every plugin load, including loads that brought no
    // reflected types at all.
    lux::meta::meta_module_drain();
    check(
        lux::meta::ReflectionRegistry::instance().findClass("lux::test::PluginType") != nullptr,
        "empty drain is a no-op (plugin type still there)"
    );
    check(
        lux::meta::ReflectionRegistry::instance().findClass("std::string") != nullptr,
        "empty drain is a no-op (baseline still there)"
    );

    lux::meta::meta_module_deinit();

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
