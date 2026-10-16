# v11 S1/S2-R implementation

Baseline: `ae77e17fcfe5549615b9bc01ffa5822246ab3314` (accepted S1/S2 verification).

This bounded revision changes two public headers and their direct tests. No new production type,
Owner, Registry, queue, scheduler, factory, or retirement path is introduced.

- `RenderCompletion::take<C>()` validates the returned original Request independently of the outcome.
  FAILED/CANCELLED can retain the existing `IpcReply.original_request`; `header()` preserves the error.
  `tryResponse()` returns null for non-success. `response()` requires SUCCESS and terminates on misuse,
  instead of interpreting a RenderError as a response. No returned original means recovery fails without
  consuming the completion. OUTCOME_UNKNOWN remains unknown, including when an ABORTED delivery does
  return an original value: recovering bytes is not proof that business work was cancelled.
- No budget owner changes: moving a value into application ownership does not immediately refund its
  still-live transport completion. Destroying it releases the original leases. A closed Link's queued
  input remains charged until the final participant releases it.
- `IpcMessage::bindType<T>()` also compares local/canonical inline policy at cold binding. The negative
  DLL fixture deliberately varies only the move noexcept contract and never transfers that incompatible
  object. SPSC, Channel/Link, publication, reply credits and reclamation source are unchanged.

Development all build passed; three targeted tests passed after correcting a test's incorrect assumption
that a closed Link releases its queued owner before the Requester exits. Failed attempt logs are retained.
The real Server Batch Handler rejects an invalid batch without partial writes; its vector backing and
capacity are recovered, corrected in place and resubmitted. Separate Client tests cover FAILED, CANCELLED,
unknown closure with no original, budget preservation and exact return to the prior retained value.

Independent validation must bind this implementation commit, preserve all 98 existing test names, and
run Release/Debug/ASan, installation and protection gates. Performance remains PARTIAL pending controlled
same-work comparison and attribution. VTune profiling and benchmark-only harness changes are external
evidence, not production optimization. Factory installation and O(N) Storage collection are deferred.
No S3 implementation is authorized by this revision.
