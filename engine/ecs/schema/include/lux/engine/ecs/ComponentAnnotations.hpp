#pragma once

// ECS schema annotations are parser-only vocabulary. Keeping them in the
// schema package prevents canonical component headers from acquiring the
// reflection adapter's include or binary closure.
#if defined __LUX_PARSE_TIME__
#    define LUX_ECS_SCHEMA_META(...) \
        __attribute__((annotate(#__VA_ARGS__)))
#else
#    define LUX_ECS_SCHEMA_META(...)
#endif

// Internal/non-protocol component: same-build Copy snapshot.
#define LUX_COMPONENT() \
    LUX_ECS_SCHEMA_META( \
        luxref::class, component=true, snapshot=copy \
    )

// Portable Copy component with an explicit stable schema identity.
#define LUX_COMPONENT_SCHEMA(name, version) \
    LUX_ECS_SCHEMA_META( \
        luxref::class, component=true, schema_name=name, \
        schema_version=version, snapshot=copy \
    )

// Derived component rebuilt after restore and omitted from persistence.
#define LUX_REBUILD_COMPONENT_SCHEMA(name, version) \
    LUX_ECS_SCHEMA_META( \
        luxref::class, component=true, schema_name=name, \
        schema_version=version, snapshot=rebuild \
    )
