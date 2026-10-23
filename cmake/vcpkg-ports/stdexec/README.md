# Pinned stdexec package

This overlay keeps NVIDIA stdexec at `7d704575019a1405b2067c5e47265245c55ca66e` (0.10.0).
The vcpkg build/feature patches are unchanged. `task-std-stop-token.patch` fixes one callback type selection:
`exec::task` must use the library's existing `stop_callback_for_t` for a parent `std::stop_token`,
which has no nested `callback_type`. This does not change the scheduler, scope or cancellation algorithm.

`task-variant-lookup.patch` qualifies the two accesses to the task's result variant. Otherwise including
`exec/variant_sender.hpp` first introduces `experimental::execution::__var`, which shadows
`stdexec::__var`; task result extraction incorrectly finds the tuple getter. Header order must not be
a consumer-side workaround. The repair changes name lookup only, not storage, allocation or execution.

Install with the repository's `cmake/vcpkg-ports` as an overlay. Classic vcpkg installations that already
contain the old package need to reinstall `stdexec:<triplet>`; an ordinary `install` may retain it.
Do not patch the installed header by hand. The source hash, patch and package ABI remain reproducible.

`world.partition_decoder_parity` exercises a coroutine inside TaskScope with ambient cancellation during
an accepted read. It also verifies file/memory decode parity without Main-thread completion dispatch.
