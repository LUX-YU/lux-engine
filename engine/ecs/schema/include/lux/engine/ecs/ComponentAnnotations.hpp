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

// A stable Component declaration is one atomic schema contract. The generator
// validates identity and Snapshot policy before publishing any projection.
#define LUX_COMPONENT(...) \
    LUX_ECS_SCHEMA_META( \
        luxref::class, type_info=static, component=true, __VA_ARGS__ \
    )
