# Asset Lifetime / Barrier A Evidence

- Lifecycle probe revision: `855cf23ed763dd4d78064583a1866286cfb4f671`.
- Full-render build and test configuration: see `spatial3d-preloaded-qualification.md`.

Exact GPU qualification result:

```text
io_submits=6,accounting=1,entities=2,partition=0,jolt_contact=1,physics_x=1000000000000.205078125,render_relative_x=0.205078125,texture=1,material=1,mesh=1,instance=1,duplicate_instance=1,alive_after_first_release=1,survived_first_release=1,final_release_empty=1,texture_slot=1,texture_generation=1,recreated_texture_slot=1,recreated_texture_generation=2,texture_generation_changed=1,lit_pixels=579,validation_errors=0
```

Observed facts:

- two World objects carried the same concrete Mesh/Material/Texture references;
- CPU procedural payloads were uploaded once and both Render instances used the same GPU handles;
- releasing one instance retained one live instance and visible output;
- releasing the final instance reduced live instances to zero;
- explicit final resource release allowed the same texture slot to be reused with generation 2 instead of generation 1;
- the real GPU path remained validation-clean.

Missing evidence:

- AssetId/cooked provider load and CPU decode ownership;
- duplicate interests arriving concurrently through async completion;
- load/decode/upload failure and retry/cooldown behavior;
- cancellation and Scene destruction during Asset work;
- World bundle-generation replacement;
- cross-scene sharing and an independent product domain.

Barrier A result: `needs more evidence`. No production demand/residency type is authorized and Full 3D Streaming remains
gated.
