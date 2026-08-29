# Phase 4 Qualification Evidence

- Reviewed implementation commit: `43d2f4dc`
- RelWithDebInfo full target `all -j 4 -k 0`: passed.
- Immediate second build: `ninja: no work to do`.
- CTest: 98/98 passed.
- Installed `component-decode-emplace` consumer: configured, built and executed successfully.
- Installed `large-world-transform` consumer: configured, built and executed successfully.

## Precision

- `Transform2D/3D` and `WorldTransform2D/3D` scalar type: double.
- Transform hierarchy test includes ±1e12 coordinates plus sub-meter offsets.
- No Scene-wide floating-origin state or second Transform component family was introduced.

## Generated decode/emplace

- Transform3D generated schema has a non-null direct thunk.
- Parent<Entity> and REBUILD WorldTransform schemas have null thunks.
- Tests cover valid roundtrip, unsupported version, truncated/trailing payload, invalid Entity and no half component.
- Decode uses stable field serialization and does not use RefClass/raw C++ layout.

## 1M Transform benchmark

`ecs_l1_benchmark --group ecs-snapshot --mode diagnostic --size 1000000`:

| Sample | Time | Hot allocations |
|---:|---:|---:|
| 0 | 46,905,200 ns | 0 |
| 1 | 44,715,900 ns | 0 |
| 2 | 44,342,000 ns | 0 |
