# F4-C fixed Binding investigation

This records development evidence; only the independent verification receipt can close P1.
Original F3 I: `0fbbbd6db3aa4ad53283522e4a54d96556b5b478`.
Original F4 I: `bd5399063907087e147ee159f24e1da6a87ab30f`.
The historical +5.0420168067% p50 and all unsuccessful runs remain untouched.

## Controlled reproduction

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4-C/`.
`performance-pre/` contains nine independent-process pairs each for AA, BB and AB, alternating AB/BA,
affinity mask 4, ABOVE_NORMAL priority, unchanged inputs and original executables. High Performance power plan,
Intel i7-13700KF, RTX 4070 Ti; every raw intra-process sample and command is retained. No sample exclusions.
Paired percentages use each process's p50/p95; 10,000 bootstrap medians, fixed seed 20261011, 95% intervals.

| Fixed Binding | Paired p50 median | p50 95% interval | Paired p95 median |
| --- | ---: | --- | ---: |
| AA | +0.8333% | [-1.6393%, +0.8403%] | 0% |
| BB | 0% | [0%, 0%] | 0% |
| AB original links | +5.6911% | [+4.8387%, +6.7227%] | +3.6232% |
| AB finalized diagnostic links | -0.8000% | [-0.8333%, 0%] | +2.2727% |

Original link AB still fails the 5% p50 gate. It is not explained away by AA/BB noise.
Large p95/max scheduling outliers remain in the JSON; a favorable mean is not substituted for them.

## Machine-code, layout and causal intervention

`binding-code/` records actual .obj hashes, compiler commands, disassembly, MSVC class-layout diagnostics,
PDB publics and CDB inspection of the original, unmodified executable at the measured call site.
F3 BoundDescriptorSets is 144 bytes; F4 is 176 bytes. F4 keeps the direct bind fields at offsets 0–95,
with DescriptorPool/device/rewrite data after them. The immutable benchmark does not allocate rewrite recipes.
No new F4-C Binding algorithm, object fields or descriptor policy is introduced.

CDB shows each original hot call targets an incremental-link ILT jump stub. Both original Ninja link
commands contain `/debug /INCREMENTAL`. Caller stack offsets and link placement differ; source equivalence
alone therefore did not establish an equivalent native instruction path/layout.

`relink-control.py` relinks the **same original objects and libraries**, changes only `/INCREMENTAL:NO`, and
places diagnostic executables/PDB/map files outside both original build trees. It records every input hash
and exact before/after command. The unchanged million-bind workload is run again, nine pairs, no exclusions.
The AB fixed-binding difference falls below the gate. Separate AA/BB finalized-link controls are retained;
some unrelated cold p95 cases also fluctuate by one 100 ns timing quantum in BB and are not hidden.

This intervention supports a link-layout contribution to the original Binding regression. It does not prove
a particular cache-miss or branch-predictor mechanism. WPR enumerates hardware counters, but starting the
strict PMU profile is denied with `0x80070005`; no trace session remains active. PMU counters are NOT_RUN,
and no fabricated instructions/cycles/cache figures are reported.

The minimal bootstrap change finalizes only the shader performance executable with `/INCREMENTAL:NO`.
It adds no native owner, changes no workload, removes no validation and does not affect product CMake.
The final independent qualification must compare the locked candidate both with the **original F3 binary**
and the matched finalized F3 link. A new >5% unexplained result remains blocking; this document is no waiver.

## Evidence retention and scope

`baseline-reproduction/` retains the pre-fix real GPU executable/libraries and failed C1/C2 output.
`performance-pre/`, `relink-control/`, `binding-code/` and all runner logs are cumulative evidence, not
replacement files for historical F4 qualification. Memory.hpp's new allocation-facts query does not change
Buffer storage layout. Actual dependency closure and Normal/ASan/GPU obligations are qualified on the final I.
