# Design

**Status:** In progress. Describes what exists: a precise, non-moving,
stop-the-world mark and sweep collector attached to a `runtime-core` context,
with `gc_store`, registered roots, handles, pins and an arena mode, taken from
the runtime stack's architecture spine (AD-2, AD-3, AD-11, AD-12, AD-13, AD-14,
AD-16 to AD-21, AD-26), with weak cells, ephemerons, stable IDs and an identity
hash, the two verification instruments (GC torture and barrier-verify), the
barrier contract and CI. What is still not here is listed at the end.

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
dropped: it checks an a/b split this library does not have. It adds
`check-dynamic-gates`, which does the same for the two instruments (see
"Torture" and "Barrier-verify" below). The examples run as part of `make test`.

## Benchmarks

`make bench` runs the harness (AD-26): a calibration case, then alloc small,
alloc 1 KiB, one full mark and sweep of a 10,000-node graph, `grheap_store`,
and (torture off, which is the baseline a tortured run is read against) the
clearing of 1,000 weak cells and a stable-ID lookup. No numeric budget is asserted; AD-26 records budgets once a first measurement
exists. `make test` runs it once, with a tiny workload, to prove it builds,
links and runs.

## GC torture

AD-16: "GC torture collects at every GC point." The option is `set_torture`
(or `GRHEAP_TORTURE=1`, which sets the default of every new options object; an
explicit setter wins, so a test that asserts timing can opt out inside a suite
run under the variable). A collected heap then collects at the start of every
allocation, before the cell is taken, and in every ACT poll that runs. That is
every GC point the heap itself owns (AD-17); the others (a call out of guest
code, a frame push) belong to the engine that has them.

A collection finds a missing root only if something then *reads* the freed
object, and a freed cell still lives inside a mapped block, so ASan reports
nothing. So after each sweep every freed cell is **poisoned**: every byte after
its free-list link word becomes `0xDD`. (The link word is the cell's type word;
a cell on a free list has no type, so there is nothing else to clear.) Only
cells of size-class blocks are poisoned: a large object's mapping is returned
to the provider whole, so a dangling read of one faults instead. A read
through a dangling pointer then sees `0xDDDDDDDDDDDDDDDD`, which no valid
payload holds, and `check-dynamic-gates` requires exactly that.

Two details matter. First, torture is a collection, not a mode: it runs the
same code as any other, so a defect it finds is a defect of a collection that
could happen without it. Second, the planted program has to be arranged so the
freed cell is still poisoned when read: a survivor in the same size class keeps
the block mapped, and the next allocation is of another class so it does not
reuse the cell (a reused cell is zeroed, which would still fail the program's
check but would not show the instrument working).

Torture collects whatever `set_auto_collect` says. Poison and torture change no
result the program computes, but torture changes *when* `destroy` and `cleared`
run and the collection counters, and verify's shadow is charged to the
context's memory budget. An arena never collects, so torture does nothing there. A heap is "tortured"
only in collected mode; the statistic `torture_collections` counts the
collections torture made, a subset of `collections`.

**Rejected: ASan manual poisoning** (`ASAN_POISON_MEMORY_REGION` on a freed
cell). It would give an exact report at the faulting read, and only in an ASan
build; torture would then need ASan to be an instrument at all, the same
tooling gap as the TSan job, and a Valgrind or release run could not use it.
Poison in the cell works in every build, and the ASan build gets nothing less
(its own checks all still run).

**Rejected: a stress threshold of zero.** Threshold zero stops being
"collect before every allocation" once a collection finds something live
(the threshold in force is then `max(threshold, live bytes)`), so it is not
stress. Torture ignores the threshold.

**Rejected: randomised collection points.** A seed makes a failure
reproducible, and a collection at every point makes it certain; the second
is stronger, and the first is only needed when the second is too slow.

## Barrier-verify

AD-11: "Every pointer store goes through `gc_store`... Barrier-verify runs in
every heap mode that has barriers." No mode of this heap has barriers, so
barrier-verify here **polices the discipline a barrier will rely on**; it does
no barrier work (AD-11: modes without barriers are context policy). The
option is `set_verify(OFF | COUNT | ABORT)`, and `GRHEAP_VERIFY=1` (ABORT) or
`=count` sets the default of a new options object.

When on, the heap keeps a **shadow table**: for every slot of a heap object
written through `grheap_store`, `_range` or `_word`, the last value written.
A slot never stored to is expected to be zero (objects are zero-filled, and a
store of zero removes its entry). `grheap_verify` and the start of every
collection walk every live object's `trace` slots (the tracer is put in a
verify mode and `trace` runs as it always does) and every weak and ephemeron
slot, and count each slot whose value differs from the shadow. A write that
bypassed the store is exactly such a slot. Each store also checks its holder:
a non-NULL holder whose cell does not contain the slot, or a NULL holder for a
slot that lies inside a live heap object, is a violation. The store still
happens and still returns OK, so that COUNT can finish a run and show the
count. ABORT prints the type name and the slot to stderr and calls `abort()`.

The shadow of a dead object is removed in the sweep before its `destroy` hook
runs, so a cell that comes back as another type, with its traced slots
elsewhere, is not blamed for the old occupant. If the table cannot grow, the
heap stops trusting it (`grheap_verify` says `ERR_OOM`) instead of reading an
absent entry as a bypass. Verify changes no result the heap computes, though its
tables are charged to the context's memory budget; a test runs one program with
it on and off and compares what is left.

What it does not do: it sees a bypass only at the next walk (not at the write,
which is a plain store the heap is never called for), and a slot not reported by
`trace` is not compared. Both are the point: this mode has no barrier work, so
the question verify answers is "would a barrier have been told".

**Rejected: an `mprotect` write barrier.** It finds the write when it happens,
and it is page-granular (a block holds thousands of objects), needs a signal
handler in a library, does not exist on MSYS2, and turns every legitimate store
into a fault storm unless every store goes through the very function being
checked.

**Rejected: card marking.** It is what a generational barrier does, and nothing
here is generational. A card table would record where a store happened and
never what it wrote, so it cannot tell a store through `grheap_store` from one
that bypassed it.

**Rejected: checking only at collection.** It is what the shadow does, plus a
value to compare against. A scan with no shadow can only say a pointer is
valid, not that it arrived by the right path.

## The barrier contract

This is the contract between a heap mode, the code that stores pointers into
heap objects (hand-written C, an interpreter, JIT code) and barrier-verify
(AD-7, AD-11, AD-16). AD-7 says it has an unshared and a shared variant and
that milestone 1 implements only the unshared one.

### Unshared variant (implemented)

An *unshared* heap belongs to one context and one thread at a time (AD-6):
there is no concurrent mutator and no concurrent collector. The barrier is for
modes that *may* need work on a store (generational, incremental); the modes
implemented, an arena and a stop-the-world full mark, need none.

**What `grheap_store*` must do.** Write the value (or values) to the slot or
slots; for every call, count it (`stores`); and, if verify is on, check the
holder and record the written values in the shadow. A mode with a barrier adds
its work here and in no caller: the call is an exported function (not an inline
macro), the holder is a parameter, and `_range` is one call so a mode can
barrier a copied range once. The holder is passed even where today's mode does
not use it, since a remembered-set barrier needs the object.

**What a compiled store must emit.** The JIT's store is equivalent to a call of
the function: it must (a) write the slot, (b) make exactly the barrier work
the heap mode's descriptor prescribes (nothing, in a mode without one), and (c)
tell verify what it wrote when verify is on, which the JIT does by calling the
function while verify is on and inlining the plain store otherwise. An
engine's interpreter calls the function.

**What the holder is.** The object whose payload contains the slot. NULL only
for a slot that is in no heap object (a registered root slot, a C variable, a
root-source slot). Out-of-line storage that a `trace` function reports is
covered by the same rule, with the *owning* object as holder, and verify counts
such a store as a bad holder; the structure has to live in the object for verify
to see it.

**What verify checks.** (1) Every slot a `trace` function reports, and every
weak and ephemeron slot, holds what the last `grheap_store*` wrote to it, or
zero if none did. (2) Each store's holder contains the slot; a NULL holder is
never given for a slot inside a heap object. It runs at the start of every
collection and in `grheap_verify`, and the first violation aborts (ABORT) or is
counted (COUNT).

**What the collector may assume.** Nothing between collections: in this mode a
collection reads every slot afresh. A mode with a barrier replaces that with
the barrier's records, and verify then compares the slot with the record and
with the shadow both.

### Shared variant (designed, not built)

A *shared* heap (AD-7's tier 2, a reserved seam) is read and written by several
contexts at once, and holds only objects that reference other shared objects:
"a shared object never references an unshared one, and a handle to an
unshared object is itself unshared".

- **The store.** An atomic release store of the slot (an `__atomic_store_n`
  with `__ATOMIC_RELEASE`, the same ordering mapping AD-7 gives the JIT), so a
  reader that loads the slot with acquire sees an initialised object. Plain
  loads of reference slots are acquire loads; relaxed word-wise accesses stay
  the rule for byte regions only.
- **Remembered sets.** Each context keeps its own remembered set: the shared
  slots it has written that point into its own unshared young space are not
  allowed (the rule above), so the set records shared slots that point to shared
  objects the *collector* has not yet seen. Per-context sets need no lock to
  append to and are drained at the group rendezvous, which AD-7 says waits only
  for contexts running guest code.
- **AD-4.** Another thread posts a request, never touches a context's state,
  and a collection of a shared heap is requested the same way: the collector is
  a handler in the ACT phase of the group's coordinating context, and the
  other contexts reach the rendezvous at their next poll.
- **AD-20.** A shared heap dies with its group, not with a context; handles to
  shared objects are shared handles and migrate with nothing. An unshared heap
  keeps the rule that it dies with its context.
- **Verify.** The shadow becomes per-context shadows merged at the rendezvous;
  a shared slot is compared at the rendezvous, never mid-store.

**Rejected: one global variant.** An atomic store on every pointer write in
an unshared heap would cost every engine for a property only the shared heap
needs. The two variants have different call signatures only in their
descriptor, not at the call site.

**Rejected: a lock around the store.** It serialises the mutators and adds a
priority-inversion hazard to the poll's fast path; the release store and the
rendezvous give the same visibility with no lock.

**Rejected: writing the contract with the first barrier mode.** The earlier
draft of this section said the contract would be written together with the
first mode that has a barrier. That ordered the work backwards: a verify mode
with no written contract has nothing to check against, and the shared variant
changes the shape of `_range`. It is written now and the barrier work replaces
the "nothing" in it.

## Weak cells and ephemerons

AD-11: "`gc` reports a cleared weak cell to its engine through the cell's
descriptor." AD-12: "weak references and ephemerons are part of `gc`'s design".
A type may carry a `GRHEAP_WeakSpec`: a weak cell (one slot) or an ephemeron
(a key slot and a value slot), each a raw pointer or a codec-encoded word, and
a `cleared` callback. The type's `trace` must not report these slots; the heap
handles them.

A collection runs, in this order:

```
verify? -> mark roots -> ephemeron fixpoint -> clear weak/ephemeron -> drop dead ids
        -> drop dead shadow entries -> sweep (destroy, poison) -> cleared() callbacks
```

1. **Fixpoint.** For each live ephemeron whose key is marked, mark its value
   and drain; repeat until a pass marks nothing (the value of one ephemeron may
   be the key of another).
2. **Clear.** A live weak cell whose target is not marked gets NULL (word 0);
   a live ephemeron whose key is not marked gets NULL in key and value. They are
   queued as cleared. A dead cell is dropped from the registry without a call.
3. **Sweep**, then
4. **`cleared(payload)`**, once per cell that had a slot cleared, with the heap
   busy: it can read the cell and nothing allocates.

Weak slots are judged only after the fixpoint, so a weak cell never sees a
target that an ephemeron value was about to keep. A pinned or conservatively
found target is marked and so stays. A slot that is not NULL and does not
decode to a live payload start is counted in `invalid_pointers` and cleared; an
encoded word that fails the tag test is an immediate and left alone (an
ephemeron keyed by an immediate keeps its value, so "a value with a NULL key is
cleared" holds for raw-pointer slots only).

The queue is not a separate structure. The registry entry of each cell has a
`pending` flag; the collector sets it while clearing and the callbacks pass
reads it after the sweep. So the collector allocates nothing, which it cannot
do (a collection never fails). The registry itself is an array appended to at
allocation, **grown before the cell is taken**, so an allocation that fails
changes nothing a caller can see. Arena mode registers nothing and never
clears.

**Rejected: a weak cell as a slot kind** (a `grheap_trace_weak` a type's `trace`
reports). It reads like the rest of tracing and is wrong in two places: `trace`
is called more than once per collection (on an overflow rescan), and a weak
slot has to be judged after marking, not during it, so every `trace` call would
need to remember weak slots for a pass the collector has not started. A spec
the heap owns is read once, at the right time.

**Rejected: a registry the engine owns.** An engine would register each cell
(a call per allocation, in a language that makes thousands), and a cell it
forgot would never clear. The type says what the object is; the heap finds it.

**Rejected: a callback queue the engine owns** (the heap reports and the engine
queues). AD-11 says the *engine* owns its own job queue, and it does: `cleared`
is the report. But a heap that calls the engine from inside the collector is a
heap that runs engine code in the collector, which AD-12 forbids. The heap
owns the queue's *timing* (after the sweep, busy) and the engine owns what
`cleared` does with it.

**Rejected: running `cleared` before the sweep.** It would see cells whose
neighbours are half-destroyed, and could not be told that the target's
`destroy` had already run.

## Stable IDs and the identity hash

AD-12: nothing outside `gc` keys by address; an ID is unique within its heap and
holds its object weakly; issuing one never triggers a GC; an identity hash comes
from `gc`. `grheap_id_of` returns the object's ID, assigning the next one
(monotonic from 1, never reused) if it has none. `grheap_object_of_id` resolves
one, and refuses an unknown or dead ID. `grheap_identity_hash` is a mix of the
ID, so it is the same whether or not the object moves.

Two structures, both in the context's allocator: a **dense array** of
`{id, object}` sorted by ID (IDs are appended in increasing order, so it stays
sorted, and lookup by ID is a binary search), and an **index** from object
address to array position, open addressing with linear probing and no
deletion, rebuilt when the array is compacted by a collection that dropped IDs
and when it grows. The index is the one place in the heap that is keyed by an
address, which AD-12 allows inside `gc`; **a moving collector re-keys it** (it
would rebuild it from the new addresses) and nothing outside the heap would
notice. A collection removes the entry of every unmarked object before its
`destroy` hook runs.

Growth failure is `ERR_LIMIT` or `ERR_OOM` per the heap's rule, with nothing
changed: the new index is built before the array grows, and each is discarded
if the other cannot be had.

**Rejected: an ID in the header.** It costs 8 bytes in every object for a
property most objects never use, and a number stored in the object has to be
kept in step with the table that finds it (an ID with no entry, or an entry for
an object that no longer has one).

**Rejected: the address as the ID, or as the hash.** It is the first thing
AD-12 prohibits: the day an object moves, every hash table keyed by it is wrong.

**Rejected: a hash table keyed by ID.** The array is sorted for free, a lookup
by ID is a binary search with no hashing, and the dense array is what a debugger
walks to list the IDs it has handed out. The only hash table is the one the
other direction needs.

**Rejected: a separate identity hash.** One mechanism, one table; the hash of an
object that has been asked for its hash is its ID's mix, and asking assigns the
ID.

## CI

`.github/workflows/ci.yml` runs the jobs AD-16 names for `gc`: both compilers
(with install and a consumer), ASan+UBSan, TSan, Valgrind, the torture modes
(`make test-torture` and `make check-dynamic-gates`), a coverage floor of 80, and
Windows under MSYS2. The setup action builds cutil and `runtime-core` into one
prefix in the workspace, the way `bootstrap.sh` does. It is the same shape as
`runtime-core`'s and `compress`'s. It cannot be run from here, and it has not
been run: it was checked only for being valid YAML that names these jobs.

## What is not here

No moving, no generations, no concurrent or incremental marking, and no stack
scanning of its own: a stack's words come from a root source, the embedder's or
A's. No real barrier (store or remembered-set work): the contract above is the
design, and barrier-verify polices the discipline. No shared heap: the shared
variant of the contract is designed, not built (AD-7, tier 2). A weak cell's
`cleared` callback cannot allocate in the heap; an engine that wants to enqueue
a job copies what it needs into its own memory.
