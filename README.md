# Ghoti.io Runtime-heap

The collector of the Ghoti.io language runtime stack, in C: a precise,
non-moving, stop-the-world mark and sweep heap that attaches to a
[`runtime-core`](../runtime-core) context. Objects are *described*, never owned
layouts: a type names an object, says how big it is, and optionally supplies a
`trace` function that reports its pointer slots and a `destroy` function that
releases what it owns outside the heap. The heap knows no engine and no value
encoding, so it works for a C program that wants a collected graph with no
engine at all, and for a language engine on the shared runtime.

Nothing is released. So far there is the collector (allocation by size class,
the mark and sweep, the value codec, registered root slots, handles, pins and
`gc_store`), an arena mode, weak cells and ephemerons, stable IDs and an identity
hash, the two verification instruments (GC torture and barrier-verify), the
barrier contract (written in [documentation/design.md](documentation/design.md),
unshared variant implemented, shared variant designed), CI, and the build, gates
and benchmark harness.

## Example

```c
#include <ghoti.io/runtime-heap/runtime-heap.h>

typedef struct { void * peer; } Node;

static void node_trace(GRHEAP_Tracer * tracer, void * payload) {
  grheap_trace_slot(tracer, &((Node *)payload)->peer);
}

static const GRHEAP_Type node_type = {"node", sizeof(Node), node_trace, NULL, NULL};

int main(void) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  grcore_group_create(NULL, NULL, &group);
  grcore_context_create(group, NULL, &context);
  grheap_heap_create(context, NULL, &heap);   /* dies with the context */

  void * a, * b;
  grheap_alloc(heap, &node_type, &a);
  grheap_alloc(heap, &node_type, &b);
  grheap_store(heap, a, &((Node *)a)->peer, b);   /* a cycle: a <-> b */
  grheap_store(heap, b, &((Node *)b)->peer, a);

  grheap_collect(heap);          /* neither is reachable: both are freed */

  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return 0;
}
```

Compile it against an installed copy with
`cc example.c $(pkg-config --cflags --libs ghoti.io-runtime-heap-0)`.
[examples/cyclic_graph.c](examples/cyclic_graph.c) is a complete version that
keeps the cycle alive three ways and then drops it.

## Building

`runtime-heap` depends on `cutil` and `runtime-core`, found through pkg-config
only. Build the suite first from the workspace root (`./bootstrap.sh`), then:

```bash
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/runtime-heap test PREFIX="$PWD/.local"
```

Pass `PREFIX=` to every `make`, including a throwaway one: the rpath is added
only when it is set. `make help` lists the targets. The ones particular to
this library:

| Target | Does |
| --- | --- |
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the examples, the unit tests, and one smoke run of the benchmark |
| `examples` | build each program under `examples/` and run it; a failing example fails `test` |
| `check-labels` | fail if a public header has no `@stability stable` label (every header here is `stable`) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil`, `runtime-core` and this one, and on any include of `runtime-core`'s A headers or its umbrella |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
| `check-dynamic-gates` | build two small programs twice (a control and a planted defect: a missing root, a missing store) and require that GC torture and barrier-verify fail the planted one, pass the control, and that the defect passes unseen when the instrument is off |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `test-torture` | the ASan+UBSan build, every unit test with `GRHEAP_TORTURE=1 GRHEAP_VERIFY=1` (a collection at every allocation, freed cells poisoned, every store checked) |
| `coverage` | instrumented run and line report; `COVERAGE_MIN=80` fails below the floor |

## The API

All headers are in `include/ghoti.io/runtime-heap/`, all labelled `stable`:
this is the C embedding API of the collector (AD-14). `runtime-heap.h` includes
them all and includes nothing from `runtime-core`'s A.

| Header | Holds |
| --- | --- |
| `options.h` | `GRHEAP_Options`: opaque, set through setters. The mode (collected or arena), the value codec, the allocation threshold, whether allocation may collect, and the two instruments (`set_torture`, `set_verify`) |
| `image.h` | `GRHEAP_Image`: an immutable, reference-counted, address-free image of the objects a context's roots reach (`grheap_image_take`, `grheap_image_instantiate`), with the writer and reader a type's hooks use; the heap's key carries one inside a context snapshot |
| `type.h` | `GRHEAP_Type` (name, default size, `trace`, `destroy`, an optional `GRHEAP_WeakSpec` for a weak cell or ephemeron, and optional `snapshot` and `restore` hooks for a payload that holds a host pointer) and the two functions a `trace` calls: `grheap_trace_slot` for a raw pointer, `grheap_trace_word` for an encoded value |
| `heap.h` | `GRHEAP_Heap`: `grheap_heap_create` (attaches to a context), `grheap_alloc`, `grheap_alloc_sized`, `grheap_collect`, `grheap_arena_reset`, `grheap_contains`, `grheap_type_of`, `grheap_size_of`, `grheap_stats` |
| `store.h` | `grheap_store`, `grheap_store_range`, `grheap_store_word`: the only sanctioned pointer stores into heap objects; and `grheap_verify`, which compares every traced slot with what the stores wrote |
| `id.h` | `grheap_id_of`, `grheap_object_of_id`, `grheap_identity_hash`: stable IDs (monotonic from 1, never reused, held weakly) and a hash that is a function of the ID |
| `roots.h` | `grheap_root_add`/`remove` (a C variable as a root), `GRHEAP_Handle`, and counted `grheap_pin`/`unpin` |

**Who owns what.** The heap owns every object it returns and a caller never
frees one. The pointer `grheap_alloc` returns is borrowed: it is valid until a
collection finds the object unreachable. A pointer is kept by a registered
root slot, a handle, a pin, a `grheap_store` into a reachable object, or a
root source of the context (the precise slots and conservative ranges a root
source reports, which is how a running engine's stack reaches the heap). Every
allocation is a GC point.

**Objects.** An object is a 16-byte header, the payload (16-byte aligned,
zero-filled), and at least one pad byte, so a pointer one past the end still
lies inside the object. Small objects come from blocks of `max(64 KiB, page
size)` in 31 size classes; anything over 8 KiB of cell gets a mapping of its
own. Nothing moves.

**Roots.** The collector marks, in order, the registered root slots, the
handles, the pinned objects, and then whatever the context's root sources
report. A precise slot must name the exact start of a live payload or it is
counted in `invalid_pointers` and skipped; a conservative range may hold
interior and one-past-the-end pointers, which keep their object alive.

**Weak cells and ephemerons.** A type with a `GRHEAP_WeakSpec` has a slot the
collector does not trace. After marking, the collector marks the value of every
ephemeron whose key is marked (to a fixpoint), sets the weak slot of every cell
whose target is unmarked to NULL, and after the sweep calls each affected
cell's `cleared(payload)` once. [examples/weak_and_ids.c](examples/weak_and_ids.c)
shows it.

**Instruments.** `GRHEAP_TORTURE=1` makes torture the default of every new
options object: a collection at the start of every allocation and in every ACT
poll, with every freed cell poisoned `0xDD`, so an object held only in a C
variable across an allocation shows up at once. `GRHEAP_VERIFY=1` makes
barrier-verify (abort; `count` counts) the default: the heap keeps a shadow of
what `grheap_store*` wrote and finds a slot written some other way. Both are off
by default and change no result the program computes (torture does change when
`destroy` and `cleared` run, and verify's tables are charged to the context's
memory budget; torture collects whatever `set_auto_collect` says, and poisons
size-class cells only, not a large object's mapping), and are shown to find a planted defect by
`make check-dynamic-gates`.

**Budgets.** Pages and metadata are charged to the context. A collection runs
when the bytes allocated since the last reach the threshold, once more when a
block cannot be obtained, and when the poll asks for memory to be reclaimed.
An allocation the context's memory budget refused is `GRHEAP_ERR_LIMIT`; one
the allocator or provider failed is `GRHEAP_ERR_OOM`.

**Threads.** The heap belongs to its context's owning thread; every function
that acts on the heap refuses another thread with `GRHEAP_ERR_INVALID` (the
queries answer NULL or false instead). The heap and its handles
migrate with the context.

[examples/README.md](examples/README.md) indexes the runnable examples by what
they show, and `make examples` builds and runs each.

## Status

Version 0.0.0, unreleased. The design and the alternatives it rejected are in
[documentation/design.md](documentation/design.md). Patches are not being
accepted at this time; see [CONTRIBUTING.md](CONTRIBUTING.md).
