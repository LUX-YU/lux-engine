# R2 — Transport Foundation work order

The user's R1 review explicitly accepts baseline
`14319331d616dbfe0318352857cf34676df6b564` and authorizes R2 only on
`codex/render-v2`. This work order records that authorization; it does not amend
the immutable architecture chapters or derive permission from their older R0 text.

Production scope: `modules/function/render/transport/**`.
Supporting scope: the independent bootstrap entry and `docs/render-v2/R2_*`.
Render Core, all frozen Legacy, architecture chapters, root/product CMake,
Engine/Editor/Toolchain consumers and existing qualification reports are read-only.
No Vulkan, Graph, Runtime, Scene/ECS, concrete Feature or domain schema is allowed.

Deliver typed stable-to-local registration/binding, owner-thread PROGRAM/CONTROL
SPSC, bounded UPLOAD MPMC, independently owned packets/blob/attachments,
request/reply, observable backpressure, stop and wake. Reuse existing generic
queue/admission and luxop generation mechanisms. The component README records
algorithm sources and any decision requiring review.

Required negative cases include two concurrent owners with identical local slot
and generation, stale routes, wrong producer/consumer threads, wrong lane/kind/
payload, foreign packets and reply tickets. Explicit reply readiness must cross
lanes; no global lane order may be assumed. Accepted packet lifetime must not
depend on the producer's source memory.

Validation includes standalone public-header compilation, nonempty CPU CTest,
actual CMake source/include/link and compiler dependency checks, generated trait
completeness, bounded concurrency and stop races, one-million-operation transport
microbenchmarks and preservation of 719 frozen blobs and original user changes.
No frame/GPU performance, V1 matrix or installed SDK result is claimed.

Follow implementation commit I -> tracked snapshot validation -> independent
clean clone qualification -> report-only verification commit V -> push -> STOP.
Implementation fixes require a new I and qualification bound to that I. Keep
commands, exit codes and hashes outside the source tree. Synchronize new public
headers to Debug, RelWithDebInfo and Android include prefixes without treating
these prefixes as bootstrap inputs. R3 requires separate user approval.
