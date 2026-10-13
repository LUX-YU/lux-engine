# Render Core — Draft4 G1

This is the new Core after the user-cleared V2 baseline, not an RD3 compatibility layer.

The only object identities are `SceneId`, `ViewId`, `TargetId`, and `EffectId`, direct
`lux::cxx::SlotKey<Tag>` aliases. Their future owners allocate and validate them with
lux-cxx containers. Core owns no objects. `isValid()` checks the null sentinel, not
membership. A View key is interpreted with its Scene key; keys from different
containers can have identical bits. Recreating a Scene invalidates the enclosing key.
Endpoint admission and reconnect invalidation belong to later stages, not this module.

`TargetDesc` is a final color-output specification with width, height, existing
`lux::rdesc::ETextureFormat`, sample count and `ETargetKind`. Validation rejects zero
extent, non-color/compressed/undefined formats, sample counts other than powers of two
from 1 to 64, and unknown kinds. These are description rules, not device support claims.
Suspended zero-size surfaces, device limits, format support, WSI and GPU ownership are
not implemented here. A future runtime Target's `uint64_t target_epoch` changes on
in-place configuration updates; SlotKey generation changes when its object is recycled.
Neither is a GPU completion token. Core deliberately has no Target owner/epoch allocator.

`RenderResult<T>` reuses `cxx::expected<T, lux::error::Error>`. Four static error descriptors
can be registered by the host. Validation allocates no memory, throws no semantic
exceptions, and performs no logging or runtime registry operations.

## Build and installation

`render_core` is a static component, exported as `lux::engine::render::render_core` by
`find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_core)`. It links only
the container, compile_time and algorithm components of lux-cxx.

The package installs three Core public headers and three unmodified canonical value
headers: `error/Error.hpp`, `error/ErrorDescriptor.hpp`, and `description/Image.hpp`.
These are installed directly from their owning source directories; there is no second
implementation or wholesale link to the Error registry or resource description library.
The latter would pull unrelated script/asset dependencies into this stage.

Use the existing `cmake/render-v2-bootstrap` entry with the normal toolset and lux-cxx
package locations. Build `all -j 4 -- -k 0`, run CTest, and repeat the build after CMake
changes. `render.core.installed` installs into a fresh prefix, relocates it, builds all
three public headers independently, and runs a separate Core-only installed consumer.
The six behavior tests use explicit checks that remain active under NDEBUG. They cover
all four IDs, local scopes, real SlotMap generation exhaustion, target validation,
revision/identity separation, and structured errors. Test-local values are not production
Scene/View/Target implementations. GPU tests are N/A; G2 and all higher modules are absent.

Root product/TOOLCHAIN profiles still reference missing old consumers. This stage does
not add dummy targets, restore the deleted tests, disable profiles, or claim whole-engine
qualification. Historical bootstrap test scripts remain untouched reference files.
