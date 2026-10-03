# Design

**Status:** In progress. Describes what exists: a precise, non-moving,
stop-the-world mark and sweep collector attached to a `runtime-core` context,
with `gc_store`, registered roots, handles, pins and an arena mode, taken from
the runtime stack's architecture spine (AD-2, AD-3, AD-11, AD-12, AD-13, AD-14,
AD-17 to AD-21, AD-26). What is not here yet is listed at the end: weak cells,
stable IDs, GC torture, barrier-verify, the barrier contract and CI.

## What this library is

`runtime-heap` is the collector. It owns the memory of the objects an engine or
a C embedder allocates, finds what is reachable from the roots a context
reports, and frees the rest. It knows no engine: an object is described to it
by a type (a name, a default size, an optional `trace`, an optional `destroy`),
and a value encoding is described to it as data (a tag test, a mask, a shift
and a base). It attaches to a `runtime-core` context through one
cardinality-one key in the poll's ACT phase, draws its pages from the
context's counting page provider and everything else from the context's
counting allocator, and dies with the context.

## Why its own library, not part of `runtime-core`

AD-3: "`gc` is its own library from day one." Two reasons carry it. The first
is the dependency direction (AD-1, AD-2): `core` never calls `gc`, so `gc`
cannot live inside it without `core` growing a layer of its own that knows
about objects. The second is the C embedder: a program that wants a collected
graph, with no engine and no frames, links `runtime-heap` and `runtime-core`'s
contexts and nothing else (`examples/cyclic_graph.c`). If the collector were
inside `core` that program would carry A, the frame protocol, for no reason.

The edge is enforced, not remembered. `make check-edges` fails on any include
of `runtime-core/a/` or of `runtime-core.h` (whose umbrella includes A) and on
any `NEEDED` entry that is not cutil, `runtime-core` or this library. A and B
ship in one shared object (AD-3), so the link line cannot tell them apart, and
the include rule is what holds the line.

**Rejected: inside `runtime-core`.** It would put a service in the kernel, and
it would make "the collector works without an engine" a property of a build
flag instead of a dependency graph.

**Rejected: a generic allocator interface that `core` calls.** The context
already hands out a counting page provider and allocator. Giving it a
collector hook would be the kernel calling a service (AD-1), when the poll
already lets the service be called instead (the ACT handler).

## Why mark and sweep, and not copying or reference counting

The spine says the first implementation does not move (AD-11), and designs for
moving anyway. A mark-sweep heap is the smallest thing that satisfies the
contract: precise tracing from described objects, conservative scanning of
ranges a root source supplies, pinning, and a stable address.

**Rejected: a copying collector.** It would need every root to be updatable at
every GC point (AD-12, AD-17), which the first engine does not yet provide, and
conservative roots (C stack segments) would have to pin whole blocks. It is
the next implementation, not this one; this one keeps its contract identical
so the next changes nothing for callers (a conservative hit pins "for that
cycle", which in a non-moving heap is a mark and costs nothing).

**Rejected: reference counting.** A cycle is the first graph anyone builds
(`examples/cyclic_graph.c` is exactly that), and a count cannot free it. A
cycle collector on top of counts is a tracing collector with extra bookkeeping
on every store, which is the cost `gc_store` exists to avoid paying until a
mode needs it.

## Layout: bump or size classes, headers or side tables

Every object is a 16-byte header `{type pointer, payload size}`, then the
payload, then at least one pad byte, rounded up to 16: a *cell*. The payload
starts 16 bytes in, so it is 16-byte aligned. The pad byte is not
decoration. AD-11 says the heap "pads objects so that a pointer one past the
end never lands on the next object", and a pointer one past the end is what a
loop that has finished iterating leaves in a register. With the pad it is
inside its own cell, and a conservative scan marks the right object;
`APointerOneAfterThePayloadNeverLandsOnTheNextObject` lays down runs of
equal objects at ten sizes and requires exactly one to survive.

Small cells (up to 8,192 bytes) come from *blocks* of `max(64 KiB, page size)`,
one block per size class at a time: 32 to 128 bytes in steps of 16, then four
classes per doubling (160, 192, 224, 256, 320, ...) to 8,192. (A 16-byte cell
cannot exist: the header alone is 16 and a pad byte is required.) Four per
doubling bounds the waste in a cell at about 20 percent while keeping 31
classes. A larger object gets one page-rounded mapping of its own. Each block
has an allocated-bit map and a mark-bit map, a free list threaded through its
free cells, and a bump index: the cells at and above it are the zero pages the
provider gave and have never been touched, so a fresh block costs nothing to
"initialise".

All spans, blocks and large mappings alike, are kept in one array sorted by
base address. An address resolves to a span by binary search and, for a
block, to a cell by one division; an oversized object is a block with one
cell, so lookup, marking and sweeping have one code path. That lookup is the
whole of "is this word a pointer into the heap?", which is the question a
conservative scan asks of every word of a stack.

**Rejected: bump allocation only.** It is what the arena mode is, and it
cannot free one object. A collected heap needs to give cells back.

**Rejected: side tables (an object-start map and a type table kept apart from
the objects).** They save the 16 bytes per object, and cost a second memory
access on every mark and a map that must be kept consistent with the objects
it describes. The header makes an object self-describing: given a payload
start, its type and size are one load away, which is also what makes the
tracer and the destroy hook callable on whatever the sweep finds.

**Rejected: a header with a mark bit.** The mark bits live in a bitmap per
block. Marking never writes to an object, so a collection that finds nothing dead
writes to no object memory at all.

## Roots: raw pointers, handles, pins

A C caller needs to keep a pointer across a collection, and three mechanisms
exist because three situations do.

- A **registered root slot** is the address of a C variable. It is the cheapest
  and the most natural: the variable is read at each collection, so there is no
  call to make when its value changes.
- A **handle** is a heap-owned cell for code that has no variable to register:
  a callback's argument, an array element in a C structure that moves.
- A **pin** keeps an object alive and, in a later moving mode, in place. It is
  counted, per object, up to 255.

**Rejected: only handles.** It would be the safest API (nothing but a handle
could hold an object) and the slowest for the code that matters: an
interpreter's inner loop holds raw pointers and cannot afford an indirection
on each. AD-11 is explicit that `gc`'s object-graph API for C embedders uses
raw pointers; the discipline is that a raw pointer is valid until the next GC
point, and one held longer is a root, a handle or a pin.

**Rejected: pins as a hash table of object to count.** A table needs an
allocation per pinned object and a lookup on every mark of a pinned object. A
count per cell, in a byte table its block makes on the first pin, is a store
and a load, and a block counts how many of its cells are pinned so marking
skips a block that has none. 255 is what a byte holds; the 256th pin of one
object is `ERR_LIMIT`, since a pin count that high is a leak.

## The value codec

Roots come from root sources (`runtime-core`'s `b/roots.h`), which hand the heap
precise slots as `uint64_t *` and conservative ranges as plain data. The heap
cannot know how an engine encodes a value, and AD-11 says it assumes no value
encoding. So the encoding is data: a word is a pointer if and only if
`(word & tag_mask) == tag_value`, and then it names
`((word & mask) >> shift) + base`. A conservative range carries its own mask,
shift and base (it is `runtime-core`'s `GRCORE_ConservativeRange`) and no
tag, since a stack word that fails a tag test is not known to be an integer.

The default is the identity, "every word is a pointer", with a zero mask and
value, so that `(word & 0) == 0` is true and no special case is needed for "no
tag test".

**Rejected: a fixed tagging scheme.** NaN-boxing, low-bit tagging and
pointer-compression are each right for some engine and wrong for the next; one
the heap hard-coded would be the heap deciding the second engine's value
representation (AD-9, AD-18). The codec is a few words of data and costs one
mask, one shift and one add.

**Rejected: a decode callback.** It would be general, and an indirect call per
slot in the mark loop, which is the hottest loop in the library.

## Precise slots and conservative ranges

A precise slot must decode to the *exact start* of a live payload. Anything
else is counted in `invalid_pointers` and skipped. A conservative range
accepts any address inside an allocated cell, interior and one-past-the-end
pointers included, and counts the word in `conservative_hits`.

The two policies differ on purpose. A precise slot is a statement by the
engine, "this is a reference", and one that is not a payload start is a bug
that should be visible rather than papered over; the counter is where it is.
A conservative range is a statement that nothing is known, and a stack holds
interior pointers (an index into an array, a pointer into the middle of a
string) that are perfectly good references, so refusing them would free
objects that are in use.

**Rejected: scanning stacks in the heap.** AD-11 and AD-18 put it in the root
sources: the owner thread's C segments are a range `core` records when
ownership is taken, spilled registers, ASan fake-stack frames and registered
segments are ranges a source reports, and the heap scans only what it is given.
Doing it here would mean the heap finding stack bounds, which means a platform
layer, which means `gc` knowing how threads work. The scan function carries
`no_sanitize("address")`: reading a stack word that ASan has poisoned is what
the scan is for.

## Marking without recursion, and without failing

Marking uses an explicit stack of payloads, so a chain of 100,000 objects
needs no C recursion. The stack grows through the context's allocator, and
the collector must never fail, so when it cannot grow the push is dropped and
a flag is set. The object stays marked (that is what ends cycles). After the
stack drains, every marked object is traced again, and the passes repeat until
one finishes without a dropped push. A dropped push is always for a newly
marked object, so each such pass marks something new and the loop ends. It is
slow when the allocator is failing (a chain found backwards costs one pass per
link) and correct, which is the trade AD-21 wants for a collection triggered by
memory pressure: the allocator is most likely to refuse exactly then.

The stack is freed at the end of every collection. Keeping it would hold memory
between collections for a structure whose size follows the heap's shape, and a
collection that starts from nothing is also the one a test can force to
overflow.

## When it collects

- On `grheap_collect`.
- At the top of an allocation, when the cell bytes allocated since the last
  collection reach the threshold (default 256 KiB). After a collection the
  threshold in force is `max(threshold, live bytes)`, so a heap that is mostly
  live is not collected every few allocations. Auto-collect is an option.
- Once, with a retry, when a block or a large mapping cannot be had.
  Collecting frees whole blocks, and a freed block is a mapping the retry may
  get. A failure that survives the retry is the caller's.
- In the key's ACT handler, when the poll says the context is over its memory
  budget and asks the handlers that can to reclaim (`reclaim_requested`). B
  defers its memory verdict by one poll for exactly this, so the collection
  happens before the verdict (CAP-2, AD-21). An arena ignores the request:
  nothing it holds can be given back until it is reset.

Every allocation is a GC point (AD-17). An allocation, a handle, a pin or a
second collection attempted during a collection or from a `trace` or `destroy`
function is refused with `ERR_INVALID` rather than allowed to corrupt a walk.

## LIMIT and OOM

A failed allocation says why. If the context's `memory_refusals` rose while the
attempt ran, the budget refused it and the result is `ERR_LIMIT`; otherwise the
allocator or the page provider failed and it is `ERR_OOM`. The distinction is
what lets a host tell "this guest used its quota" from "the machine is out of
memory", and it is read from a counter rather than inferred from the shape of
the failure, so that a refusal on an unexpected allocation (a span table
growing, a pin table) is classified the same way as one on the page.

## `gc_store` is a function

AD-11: every pointer store goes through `gc_store(holder, slot, value)`. In a
stop-the-world non-moving full mark there is no barrier work to do, so each
call writes and counts. It is an exported function and not an inline macro so
that a mode with a barrier changes one file and no caller: a macro would have
been expanded into every engine and every JIT-generated stub, and "add a
barrier" would have meant rebuilding the world. The price is a call on a path
that does nothing yet (`store` in `make bench` measures it).

**Rejected: an inline macro.** For the reason above, and because the JIT will
emit a barrier from a descriptor, not from a header (AD-14), so the macro would
be the thing that had to be kept in step with it.

## Arena mode

An arena bump allocates from chunks the size of a block and never frees one
object. `grheap_arena_reset` runs every `destroy` hook and gives every chunk
back, keeping the heap; destroying the context does the same and then frees the
heap. Handles, pins and roots are accepted and keep nothing alive, so code
written against one mode runs in the other; a reset clears every handle to NULL
and drops the pins, and leaves registered root slots registered (they are the
caller's memory, and an arena does not scan them). Heap modes without barriers,
an arena and a stop-the-world full mark, are context policy (AD-11).

## Ownership and threads

The heap and its handles belong to the context's owning thread (AD-6, AD-20).
Every function that changes the heap checks `grcore_context_is_owner` and refuses
another thread with `ERR_INVALID`; the exceptions are the queries (`grheap_heap_get`,
`grheap_handle_get`, which answers NULL) and the options and trace functions, which
touch no heap state. So a heap whose context has been released is unusable by
anyone until it is acquired, and migrates with the context. The check is the
context's atomic owner word, released and acquired by the context's own
calls, which is what makes a hand-off free of data races under ThreadSanitizer
(`ThreadsTest`).

A returned object is the heap's: a caller never frees one. The pointer
`grheap_alloc` returns is borrowed, valid until a collection finds the object
unreachable (CONVENTIONS.md section 13).

## Teardown

There is no public heap destroy (AD-20). The key's destructor runs `destroy`
hooks for every live object (so a resource an object owns outside the heap is
released), hands every page back, and frees the metadata, all through the
context's own allocator and provider. After the context is destroyed the test
allocator holds no live block and the provider no mapping, and the context's
meter reads what it would with no heap at all
(`WhenTheHeapsKeyIsTornDownTheContextsMeterIsBackWhereItWasWithoutAHeap`).

## Gates

`make test` runs the gates `runtime-core` runs, less the one that does not
apply: `check-symbols`, `check-aliasing`, `check-stamps`, `check-labels` (every
header is `@stability stable`; there is no `free` layer here), `check-edges`
(the allowlist, plus the rule against `runtime-core/a/` and the umbrella) and
`check-gates`, which runs each of them against a planted defect and a control
and against an empty tree, and fails unless each behaves. `check-direction` is
dropped: it checks an a/b split this library does not have. The examples run
as part of `make test`.

## Benchmarks

`make bench` runs the harness (AD-26): a calibration case, then alloc small,
alloc 1 KiB, one full mark and sweep of a 10,000-node graph, and `grheap_store`.
No numeric budget is asserted; AD-26 records budgets once a first measurement
exists. `make test` runs it once, with a tiny workload, to prove it builds,
links and runs.

## Barrier contract: story 7

*Reserved.* This section will state what a GC or a JIT must do when a heap
mode has barriers: what `gc_store` must record, what a compiled store must
emit, and how barrier-verify checks that no store bypassed it (AD-11, AD-16).
It is deliberately empty here: the contract is written together with the first
mode that has a barrier and with the verify mode that polices it, so that it
describes something that exists.

## What is not here

Weak cells and ephemerons, stable IDs and identity hashes, GC torture
(collect at every GC point), barrier-verify, the barrier contract above, and
CI. No moving, no generations, no concurrent or incremental marking, and no
stack scanning of its own: a stack's words come from a root source, the
embedder's or A's.
