# N6 implementation boundaries

N5 exact I fae3f9b710842e28d002aff2c91ea4d90a3c573e independently passed R/D/ASan114/114;
V4f90e946f9e2e58c5558b7384f0253010a146577. N6 starts with real Mesh Storage, not a renamed test fixture.

Mesh stores one interleaved vertex/index payload, explicit asset attributes and submesh ranges, qualified
SlotKey/revision and real physical versions. The existing Transfer performs all CPU/staging/copy/acquire
progress. Candidate publication owns only the Mesh transaction; StorageSet owns the Storage, Registry
only borrows it, Context Use borrows the prepared Buffer and contributes to its real LastUse. Physical
versions reserve retirement before admission and survive Storage deletion only via existing Retirement.
No hard-coded business capacity; an optional byte budget counts live and retired versions. A bound Mesh
replacement must preserve its complete layout/submesh contract; structural change requires cold detach/
rebuild so immutable Draw recipes cannot silently consume a different asset layout. Same-shape bytes
replace at the existing owner publication boundary and rebind complete prepared state.

Mesh validation includes actual device vertex formats and limits, byte arithmetic/alignment, submesh
ranges and actual index values. Dynamic request metadata plus complete source backing are budgeted.
Native Commands use the existing typed provider/Reply/defer/GpuWorkSender; no Runtime business switch.

N6.2-5 still pending: existing Material lower/emit/Cook consumption, sparse Instance updates, actual
Camera and BasicMesh/DepthOnly Features plus dual-client integration. The read-only audit is external;
no unimplemented API or placeholder target is introduced to claim those steps. N7 performance and final
qualification remain pending; historical performance PARTIAL is unchanged.
