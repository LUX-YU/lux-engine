# Active Mesh queries

`MeshQuerySystem` is an optional Scene system. It queries loaded ECS entities and
their current derived transforms. It has no Renderer, camera, Editor selection,
model import, partition traversal or implicit IO dependency.

## Ownership and progression

The Scene owner installs the system against its Registry. Asset loading prepares
immutable local `MeshQueryGeometry` values on Process; the owner adopts a complete
asset version with `setGeometry`. Instances and separate author/Run Scenes may
share the same geometry. Asset replacement removes the old association; outstanding
shared owners determine when the old geometry is destroyed.

`Mesh3D` and `WorldTransform3D` changes invalidate the object index. At the normal
stable point, after transform derivation, structural changes rebuild the flat AABB
tree; transform changes refit only affected leaves and their ancestors. A query
never advances the Scene, loads assets or builds geometry. A dirty or incomplete
index returns `NOT_READY`, rather than reporting an inaccurate miss.

## Query contract

`raycastNearest` uses a normalized double-precision world ray and a finite positive
maximum distance. A successful `true` result writes the nearest full Entity,
world-space position, inverse-transpose normal, distance and local triangle index.
A successful `false` leaves the supplied hit untouched. Invalid input, singular
transforms and unavailable/failed geometry are distinct structured failures.

WorldObjectId and partition indexes do not participate. Entity generation is part
of the result; callers retaining a result across Scene lifetimes additionally bind
it to that Scene instance. Unloaded partitions are outside the query domain.

Large coordinates are subtracted in double precision before local triangle work.
Local geometry retains its asset float precision. The first implementation queries
static Mesh triangles; skin deformation and material opacity are not pixel-picking
guarantees.

## Cost and extension boundaries

Static queries do not allocate, copy components or rebuild either index. They walk
the object tree and then candidate local BVHs. Structural rebuild is O(N log N);
refit cost follows the changed leaves and their ancestors. The index accounting
reports vector capacity and map value payloads, excluding allocator/map-node
overhead. Shared geometry storage is reported separately and must not be multiplied
by instance count.

Camera projection belongs to optional render integration; screen coordinates and
Editor work-plane fallback belong to the spatial viewport. Future scripting may
expose this same query capability through the existing Ability mechanism.
