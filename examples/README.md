# Examples

Each program here is built and run by `make examples` (and by `make test`), so
an example that stops working fails the build. Run them with the same
`PREFIX=` the library was built with. They are C, they use `runtime-heap` and
`runtime-core` (the context a heap attaches to) and nothing else, and none
needs an engine.

| To see how to... | Read |
| --- | --- |
| Attach a heap to a context and allocate described objects | [`cyclic_graph.c`](cyclic_graph.c), `main`, "A collected heap" |
| Describe an object to the collector: where its pointers are and what runs when it dies | [`cyclic_graph.c`](cyclic_graph.c), `node_trace` and `node_destroy` |
| Write a pointer into a heap object (`grheap_store`) | [`cyclic_graph.c`](cyclic_graph.c), `make_cycle` |
| Keep an object alive from a C variable | [`cyclic_graph.c`](cyclic_graph.c), `main`, "Kept by a registered root slot" |
| Keep an object alive when there is no variable to register (a handle) | [`cyclic_graph.c`](cyclic_graph.c), `main`, "Kept by a handle" |
| Keep an object alive, counted, for as long as a call needs it (a pin) | [`cyclic_graph.c`](cyclic_graph.c), `main`, "Kept by a pin" |
| See a cycle collected, and each `destroy` hook run once | [`cyclic_graph.c`](cyclic_graph.c), `main` |
| Read what a heap has done (`grheap_stats`) | [`cyclic_graph.c`](cyclic_graph.c), `main` |
| Allocate in an arena and drop everything at once | [`cyclic_graph.c`](cyclic_graph.c), `main`, "An arena" |
| Describe a weak cell and learn when its target died (`GRHEAP_WeakSpec`, `cleared`) | [`weak_and_ids.c`](weak_and_ids.c), `cache_type`, "A weak cell" |
| Keep a value alive only as long as its key is (an ephemeron) | [`weak_and_ids.c`](weak_and_ids.c), `main`, "An ephemeron" |
| Name an object without its address (`grheap_id_of`, `grheap_object_of_id`, `grheap_identity_hash`) | [`weak_and_ids.c`](weak_and_ids.c), `main`, "Stable IDs" |
| Ask why an object is still alive: the shortest chain from a root, each holder's type and slot offset (`grheap_retention_path`) | [`retention_path.c`](retention_path.c), `main` |
| Tell "not retained" (an answer) from an error | [`retention_path.c`](retention_path.c), `main`, "Fix the leak" |
| Run under the instruments (`GRHEAP_TORTURE=1`, `GRHEAP_VERIFY=1`) | [`weak_and_ids.c`](weak_and_ids.c) runs unchanged under both; `make test-torture` runs the unit tests that way |

The graph is two nodes that point at each other. A reference-counting scheme
never frees such a pair; here nothing special is done for it, because the
collector finds what is reachable from the roots and frees the rest.

A real program also has a root source, so that the collector can find the
pointers a running engine holds on its guest stack and in its registers. That
is `runtime-core`'s `grcore_context_add_root_source`, and it is how a
conservative range of C stack reaches the heap; these examples hold their
pointers in root slots, handles and pins instead.
