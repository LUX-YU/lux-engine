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

// Component identity, snapshot behavior, and cooked-section capability are
// separate declarations. Stable components must state both policies.
#define LUX_COMPONENT() \
    LUX_ECS_SCHEMA_META( \
        luxref::class, component=true \
    )

#define LUX_COMPONENT_SCHEMA(name, version) \
    LUX_ECS_SCHEMA_META( \
        luxref::class, component=true, schema_name=name, \
        schema_version=version \
    )

#define LUX_COMPONENT_SNAPSHOT(policy) \
    LUX_ECS_SCHEMA_META(snapshot=policy)

#define LUX_COMPONENT_WORLD_SECTION(policy) \
    LUX_ECS_SCHEMA_META(world_section=policy)
