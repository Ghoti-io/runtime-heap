/**
 * @file
 *
 * Creating a heap, allocating from it, and asking what it holds.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>
#include <thread>

namespace {

size_t page_size() {
  return grcore_page_provider_default()->page_size;
}

size_t block_size() {
  return std::max<size_t>(65536, page_size());
}

size_t round_up(size_t n, size_t to) {
  return (n + to - 1) / to * to;
}

/* The size classes the spec fixes: 32 to 128 in steps of 16 (a 16-byte cell
 * cannot exist, since the header alone is 16 and a pad byte is required), then
 * four per doubling to 8192. */
std::vector<size_t> classes() {
  std::vector<size_t> v;
  for (size_t c = 32; c <= 128; c += 16) {
    v.push_back(c);
  }
  for (size_t lo = 128; lo < 8192; lo *= 2) {
    for (size_t k = 1; k <= 4; k++) {
      v.push_back(lo + k * lo / 4);
    }
  }
  return v;
}

} // namespace

TEST(Heap, CreateAttachesOneHeapToTheContextAndGetFindsIt) {
  WorldConfig config;
  config.create_heap = false;
  HeapWorld w(config);
  EXPECT_EQ(grheap_heap_get(w.ctx), nullptr);
  GRHEAP_Heap * heap = nullptr;
  ASSERT_EQ(grheap_heap_create(w.ctx, nullptr, &heap), GRHEAP_OK);
  ASSERT_NE(heap, nullptr);
  EXPECT_EQ(grheap_heap_get(w.ctx), heap);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 1u);
  w.heap = heap;
}

TEST(Heap, ASecondHeapOnTheSameContextIsRefusedAndTheFirstIsIntact) {
  HeapWorld w;
  Node * kept = new_node(w.heap, 1);
  GRHEAP_Heap * second = reinterpret_cast<GRHEAP_Heap *>(0x1);
  EXPECT_EQ(grheap_heap_create(w.ctx, nullptr, &second), GRHEAP_ERR_INVALID);
  EXPECT_EQ(second, reinterpret_cast<GRHEAP_Heap *>(0x1)); // not written
  EXPECT_EQ(grheap_heap_get(w.ctx), w.heap);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 1u);
  EXPECT_TRUE(grheap_contains(w.heap, kept));
  void * more;
  EXPECT_EQ(grheap_alloc(w.heap, &kLeaf, &more), GRHEAP_OK);
}

TEST(Heap, CreateRefusesNullsAndANonOwner) {
  WorldConfig config;
  config.create_heap = false;
  HeapWorld w(config);
  GRHEAP_Heap * heap;
  EXPECT_EQ(grheap_heap_create(nullptr, nullptr, &heap), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_heap_create(w.ctx, nullptr, nullptr), GRHEAP_ERR_INVALID);
  GRHEAP_Result r = GRHEAP_OK;
  std::thread other([&] { r = grheap_heap_create(w.ctx, nullptr, &heap); });
  other.join();
  EXPECT_EQ(r, GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_heap_get(w.ctx), nullptr);
  EXPECT_EQ(grheap_heap_get(nullptr), nullptr);
}

TEST(Heap, CreateWhileTheContextIsRunningIsRefusedAndChangesNothing) {
  WorldConfig config;
  config.create_heap = false;
  HeapWorld w(config);
  GRHEAP_Result r = GRHEAP_OK;
  GRHEAP_Heap * heap = nullptr;
  Fn fn{[&](GRCORE_Context * c) {
    r = grheap_heap_create(c, nullptr, &heap);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(r, GRHEAP_ERR_INVALID);
  EXPECT_EQ(heap, nullptr);
  EXPECT_EQ(grheap_heap_get(w.ctx), nullptr);
}

TEST(Heap, CreateMovesAllocationFailureIntoItsResultAndLeavesNoHeap) {
  // Sweep N from 1: every failing allocation is OOM and leaves no heap, no
  // registration and no block behind; the first N that succeeds ends it.
  bool saw_failure = false;
  for (long n = 1; n < 20; n++) {
    WorldConfig config;
    config.create_heap = false;
    HeapWorld w(config);
    w.allocator.calls = 0;
    w.allocator.fail_at = n;
    GRHEAP_Heap * heap = nullptr;
    GRHEAP_Result r = grheap_heap_create(w.ctx, nullptr, &heap);
    w.allocator.fail_at = 0;
    if (r == GRHEAP_OK) {
      EXPECT_EQ(grheap_heap_get(w.ctx), heap);
      EXPECT_TRUE(saw_failure);
      break;
    }
    saw_failure = true;
    EXPECT_EQ(r, GRHEAP_ERR_OOM) << "N=" << n;
    EXPECT_EQ(heap, nullptr);
    EXPECT_EQ(grheap_heap_get(w.ctx), nullptr);
    EXPECT_EQ(grcore_context_registration_count(w.ctx), 0u);
    // And the context can still take a heap afterwards.
    EXPECT_EQ(grheap_heap_create(w.ctx, nullptr, &heap), GRHEAP_OK);
  }
  EXPECT_TRUE(saw_failure);
}

TEST(Alloc, ReturnsAZeroFilledSixteenByteAlignedPayloadOfTheTypesSize) {
  HeapWorld w;
  static const GRHEAP_Type big = GRHEAP_TYPE_INIT("big", 100, nullptr, nullptr, nullptr, nullptr, nullptr);
  void * p = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &big, &p), GRHEAP_OK);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);
  const unsigned char * bytes = static_cast<const unsigned char *>(p);
  for (size_t i = 0; i < 100; i++) {
    ASSERT_EQ(bytes[i], 0) << "byte " << i;
  }
  size_t size = 0;
  ASSERT_EQ(grheap_size_of(w.heap, p, &size), GRHEAP_OK);
  EXPECT_EQ(size, 100u);
  EXPECT_EQ(grheap_type_of(w.heap, p), &big);
}

TEST(Alloc, SizedAllocationOverridesTheTypesSizeAndAllowsZero) {
  HeapWorld w(steady());
  void * p;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 1000, &p), GRHEAP_OK);
  size_t size = 0;
  ASSERT_EQ(grheap_size_of(w.heap, p, &size), GRHEAP_OK);
  EXPECT_EQ(size, 1000u);
  void * z;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 0, &z), GRHEAP_OK);
  ASSERT_EQ(grheap_size_of(w.heap, z, &size), GRHEAP_OK);
  EXPECT_EQ(size, 0u);
  EXPECT_NE(p, z);
  // An absurd size is a failed allocation, not a wrapped one.
  void * none = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, SIZE_MAX, &none), GRHEAP_ERR_OOM);
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, SIZE_MAX - 20, &none),
      GRHEAP_ERR_OOM);
  EXPECT_EQ(none, reinterpret_cast<void *>(0x1));
}

TEST(Alloc, EverySizeClassBoundaryLandsInTheClassTheLayoutFixes) {
  // A payload of p bytes needs 16 + p + 1 bytes, rounded to 16: the largest
  // payload of a class with cell c is c - 17, and one more byte is the next
  // class. Each class owns one block, so the first object of a class maps a
  // block and later ones of the same class map none.
  HeapWorld w(steady());
  const size_t block = block_size();
  uint64_t mapped = w.stats().mapped_bytes;
  EXPECT_EQ(mapped, 0u);
  std::vector<size_t> cls = classes();
  ASSERT_EQ(cls.size(), 31u);
  size_t prev = 16;
  for (size_t c : cls) {
    // The smallest payload that belongs to c: one past the previous class's
    // largest, or for 32 the empty payload.
    size_t smallest = c == 32 ? 0 : prev - 16;
    size_t largest = c - 17;
    void * lo;
    void * hi;
    ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, smallest, &lo), GRHEAP_OK);
    uint64_t after_first = w.stats().mapped_bytes;
    EXPECT_EQ(after_first - mapped, block) << "class " << c << " opens a block";
    ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, largest, &hi), GRHEAP_OK);
    EXPECT_EQ(w.stats().mapped_bytes, after_first)
        << "class " << c << " holds payloads " << smallest << ".." << largest;
    mapped = after_first;

    // Aligned, zero-filled, and the right size, at both ends.
    for (void * p : {lo, hi}) {
      EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);
      size_t size;
      ASSERT_EQ(grheap_size_of(w.heap, p, &size), GRHEAP_OK);
      const unsigned char * b = static_cast<const unsigned char *>(p);
      for (size_t i = 0; i < size; i++) {
        ASSERT_EQ(b[i], 0) << "class " << c << " byte " << i;
      }
    }
    // The two are in the same block, so they are less than a block apart.
    EXPECT_LT(std::abs(static_cast<long>(reinterpret_cast<uintptr_t>(hi) -
                  reinterpret_cast<uintptr_t>(lo))),
        static_cast<long>(block));
    prev = c;
  }
  EXPECT_EQ(w.stats().live_objects, 62u);
}

TEST(Alloc, AnObjectWhoseCellPassesEightKibGetsAMappingOfItsOwn) {
  HeapWorld w(steady());
  const size_t page = page_size();
  // 8192 - 17 is the largest small payload; one more byte is large.
  void * small;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 8192 - 17, &small), GRHEAP_OK);
  EXPECT_EQ(w.stats().mapped_bytes, block_size());
  void * large;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 8192 - 16, &large), GRHEAP_OK);
  // Cell 8208, a page-rounded mapping.
  EXPECT_EQ(w.stats().mapped_bytes, block_size() + round_up(8208, page));
  size_t size;
  ASSERT_EQ(grheap_size_of(w.heap, large, &size), GRHEAP_OK);
  EXPECT_EQ(size, 8192u - 16u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(large) % 16, 0u);
  // The whole payload is writable and was zero.
  unsigned char * b = static_cast<unsigned char *>(large);
  for (size_t i = 0; i < size; i++) {
    ASSERT_EQ(b[i], 0);
  }
  memset(b, 0xAB, size);
  // A second large object is a second mapping.
  void * large2;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 3 * page, &large2), GRHEAP_OK);
  EXPECT_EQ(w.stats().mapped_bytes,
      block_size() + round_up(8208, page) + round_up(16 + 3 * page + 1, page));
  EXPECT_TRUE(grheap_contains(w.heap, large));
  EXPECT_TRUE(grheap_contains(w.heap, large2));
  EXPECT_FALSE(grheap_contains(w.heap, b + 16)); // interior
}

TEST(Alloc, ALargeObjectIsFreedAndItsMappingReturnedWhenUnreachable) {
  HeapWorld w;
  void * large;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 100000, &large), GRHEAP_OK);
  EXPECT_EQ(w.pages.live, 1);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, large));
  EXPECT_EQ(w.pages.live, 0);
  EXPECT_EQ(w.stats().mapped_bytes, 0u);
  EXPECT_EQ(w.stats().live_objects, 0u);
  EXPECT_EQ(w.stats().frees, 1u);
}

TEST(Alloc, ARootedLargeObjectSurvivesAndKeepsItsMapping) {
  HeapWorld w;
  void * large = nullptr;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 100000, &large), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &large), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, large));
  EXPECT_EQ(w.pages.live, 1);
  ASSERT_EQ(grheap_root_remove(w.heap, &large), GRHEAP_OK);
}

TEST(Alloc, RefusesNullArgumentsAndANonOwner) {
  HeapWorld w;
  void * p = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_alloc(nullptr, &kLeaf, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, nullptr, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, &kLeaf, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc_sized(w.heap, nullptr, 8, &p), GRHEAP_ERR_INVALID);
  GRHEAP_Result r = GRHEAP_OK;
  GRHEAP_Stats before = w.stats();
  std::thread other([&] {
    void * q;
    r = grheap_alloc(w.heap, &kLeaf, &q);
  });
  other.join();
  EXPECT_EQ(r, GRHEAP_ERR_INVALID);
  EXPECT_EQ(p, reinterpret_cast<void *>(0x1));
  EXPECT_EQ(w.stats().allocations, before.allocations);
}

TEST(Alloc, InsideATraceFunctionIsRefusedAndChangesNothing) {
  HeapWorld w;
  static GRHEAP_Result inside_trace;
  static GRHEAP_Heap * the_heap;
  the_heap = w.heap;
  inside_trace = GRHEAP_OK;
  static const GRHEAP_Type trying = GRHEAP_TYPE_INIT("trying", 16,
      [](GRHEAP_Tracer *, void *) {
        void * q;
        inside_trace = grheap_alloc(the_heap, &kLeaf, &q);
      },
      nullptr, nullptr, nullptr, nullptr);
  void * root = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &trying, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  uint64_t allocations = w.stats().allocations;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(inside_trace, GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().allocations, allocations);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Alloc, InsideADestroyFunctionIsRefusedAndSoIsACollection) {
  HeapWorld w;
  static GRHEAP_Result alloc_in_destroy;
  static GRHEAP_Result collect_in_destroy;
  static GRHEAP_Heap * the_heap;
  the_heap = w.heap;
  alloc_in_destroy = collect_in_destroy = GRHEAP_OK;
  static const GRHEAP_Type dying = GRHEAP_TYPE_INIT("dying", 16, nullptr, [](void *) {
    void * q;
    alloc_in_destroy = grheap_alloc(the_heap, &kLeaf, &q);
    collect_in_destroy = grheap_collect(the_heap);
  }, nullptr, nullptr, nullptr);
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &dying, &p), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(alloc_in_destroy, GRHEAP_ERR_INVALID);
  EXPECT_EQ(collect_in_destroy, GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().frees, 1u);
}

TEST(Contains, IsTrueOnlyForTheExactPayloadStartOfALiveObject) {
  HeapWorld w;
  Node * n = new_node(w.heap, 1);
  EXPECT_TRUE(grheap_contains(w.heap, n));
  auto * bytes = reinterpret_cast<unsigned char *>(n);
  EXPECT_FALSE(grheap_contains(w.heap, bytes + 1));            // interior
  EXPECT_FALSE(grheap_contains(w.heap, bytes + sizeof(Node))); // one past
  EXPECT_FALSE(grheap_contains(w.heap, bytes - 16));           // the header
  EXPECT_FALSE(grheap_contains(w.heap, nullptr));
  int local = 0;
  EXPECT_FALSE(grheap_contains(w.heap, &local)); // not ours
  EXPECT_FALSE(grheap_contains(nullptr, n));
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, n)); // freed
}

TEST(Contains, AnAddressInTheUnusedTailOfABlockIsNotAnObject) {
  HeapWorld w;
  void * p;
  // 48-byte cells: a 64 KiB block holds 1365 of them and wastes 16 bytes.
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 20, &p), GRHEAP_OK);
  // Allocated cells are all below the bump index; an address just past the
  // last allocated cell is a cell that has never been used.
  auto * next = static_cast<unsigned char *>(p) + 48;
  EXPECT_FALSE(grheap_contains(w.heap, next));
}

TEST(Queries, TypeAndSizeOfRefuseWhatIsNotALivePayloadStart) {
  HeapWorld w;
  Node * n = new_node(w.heap, 1);
  EXPECT_EQ(grheap_type_of(w.heap, n), &kNode);
  EXPECT_EQ(grheap_type_of(w.heap, reinterpret_cast<char *>(n) + 8), nullptr);
  EXPECT_EQ(grheap_type_of(w.heap, nullptr), nullptr);
  size_t size = 77;
  EXPECT_EQ(grheap_size_of(w.heap, reinterpret_cast<char *>(n) + 8, &size),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_size_of(w.heap, n, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(size, 77u); // not written on failure
}

TEST(Stats, CountAllocationsLiveObjectsAndMappedBytes) {
  HeapWorld w(steady());
  GRHEAP_Stats s0 = w.stats();
  EXPECT_EQ(s0.live_objects, 0u);
  EXPECT_EQ(s0.mapped_bytes, 0u);
  EXPECT_EQ(s0.allocations, 0u);
  for (int i = 0; i < 10; i++) {
    new_node(w.heap, static_cast<uint64_t>(i), &kPlainNode);
  }
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.allocations, 10u);
  EXPECT_EQ(s.live_objects, 10u);
  // sizeof(Node) is 24: header 16 + 24 + pad 1 = 41 -> a 48-byte cell.
  EXPECT_EQ(s.live_bytes, 10u * 48u);
  EXPECT_EQ(s.mapped_bytes, block_size());
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  s = w.stats();
  EXPECT_EQ(s.collections, 1u);
  EXPECT_EQ(s.frees, 10u);
  EXPECT_EQ(s.live_objects, 0u);
  EXPECT_EQ(s.live_bytes, 0u);
  EXPECT_EQ(s.mapped_bytes, 0u); // the empty block went back to the provider
  GRHEAP_Stats none;
  EXPECT_EQ(grheap_stats(nullptr, &none), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_stats(w.heap, nullptr), GRHEAP_ERR_INVALID);
}

TEST(Accounting, EveryBlockTheHeapHoldsIsChargedToTheContext) {
  HeapWorld w;
  uint64_t before = grcore_context_memory_in_use(w.ctx);
  uint64_t blocks_before = grcore_context_memory_blocks(w.ctx);
  new_node(w.heap, 1, &kPlainNode);
  // The page block, and the metadata that describes it, are the context's.
  EXPECT_GE(grcore_context_memory_in_use(w.ctx), before + block_size());
  EXPECT_GE(grcore_context_memory_blocks(w.ctx), blocks_before + 2);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  // The empty block and its span record were released.
  EXPECT_LT(grcore_context_memory_in_use(w.ctx), before + block_size());
}

TEST(Accounting, DestroyingTheContextRunsEveryHookAndFreesEverything) {
  destroyed_log().clear();
  {
    HeapWorld w(steady());
    Node * a = new_node(w.heap, 1);
    Node * b = new_node(w.heap, 2);
    // Two live objects and a large one with a hook.
    static const GRHEAP_Type big = GRHEAP_TYPE_INIT("big", 40000, nullptr, [](void *) {
      destroyed_log().push_back(3);
    }, nullptr, nullptr, nullptr);
    void * p;
    ASSERT_EQ(grheap_alloc(w.heap, &big, &p), GRHEAP_OK);
    (void)a;
    (void)b;
    EXPECT_TRUE(destroyed_log().empty());
  } // ~HeapWorld holds allocator.live and pages.live to zero
  EXPECT_EQ(destroyed_count(1), 1u);
  EXPECT_EQ(destroyed_count(2), 1u);
  EXPECT_EQ(destroyed_count(3), 1u);
}

namespace {

/* Records what the context's meter reads at the point its key is torn down. */
struct MeterProbe {
  uint64_t blocks = ~UINT64_C(0);
  uint64_t in_use = ~UINT64_C(0);
};

void probe_destroy(GRCORE_Context * context, void * value) {
  auto * p = static_cast<MeterProbe *>(value);
  p->blocks = grcore_context_memory_blocks(context);
  p->in_use = grcore_context_memory_in_use(context);
}

/* Registered first, so it is torn down last: after the heap's own key. */
const GRCORE_Key kProbeKey = GRCORE_KEY_INIT("meter-probe", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_NONE, probe_destroy, nullptr, nullptr, nullptr, nullptr);

MeterProbe teardown_meter(bool with_heap) {
  MeterProbe probe;
  {
    WorldConfig config;
    config.create_heap = false;
    HeapWorld w(config);
    EXPECT_EQ(grcore_context_register(w.ctx, &kProbeKey, &probe), GRCORE_OK);
    if (with_heap) {
      EXPECT_EQ(grheap_heap_create(w.ctx, nullptr, &w.heap), GRHEAP_OK);
      void * keep[4] = {};
      for (int i = 0; i < 4; i++) {
        EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, i == 3 ? 200000 : 40,
                      &keep[i]),
            GRHEAP_OK);
        GRHEAP_Handle * h;
        EXPECT_EQ(grheap_handle_create(w.heap, keep[i], &h), GRHEAP_OK);
        EXPECT_EQ(grheap_pin(w.heap, keep[i]), GRHEAP_OK);
        EXPECT_EQ(grheap_root_add(w.heap, &keep[i]), GRHEAP_OK);
      }
      EXPECT_EQ(grheap_collect(w.heap), GRHEAP_OK);
      EXPECT_GT(grcore_context_memory_blocks(w.ctx), 8u);
    }
  }
  return probe;
}

} // namespace

TEST(Accounting, WhenTheHeapsKeyIsTornDownTheContextsMeterIsBackWhereItWasWithoutAHeap) {
  MeterProbe bare = teardown_meter(false);
  MeterProbe used = teardown_meter(true);
  ASSERT_NE(bare.blocks, ~UINT64_C(0));
  ASSERT_NE(used.blocks, ~UINT64_C(0));
  EXPECT_EQ(used.blocks, bare.blocks);
  EXPECT_EQ(used.in_use, bare.in_use);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// A small object occupies its size class's cell, which can be larger than the
// 16-byte-rounded cell it asked for. Allocation and sweep must count the same.
TEST(Stats, LiveBytesAgreeBeforeAndAfterACollectionForAnyClassSize) {
  HeapWorld w;
  static const GRHEAP_Type kBlob = GRHEAP_TYPE_INIT("blob", 0, nullptr, nullptr, nullptr, nullptr, nullptr);
  // 112 + 17 -> a 144-byte need, held in the 160 class; 1000 -> a 1024 class.
  for (size_t bytes : {size_t(112), size_t(1000), size_t(130), size_t(8000)}) {
    void * p;
    ASSERT_EQ(grheap_alloc_sized(w.heap, &kBlob, bytes, &p), GRHEAP_OK);
    GRHEAP_Handle * h;
    ASSERT_EQ(grheap_handle_create(w.heap, p, &h), GRHEAP_OK);
  }
  uint64_t before = w.stats().live_bytes;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  GRHEAP_Stats after = w.stats();
  EXPECT_EQ(after.live_objects, 4u);
  EXPECT_EQ(after.live_bytes, before);
}

static GRHEAP_Handle * g_hook_handle = nullptr;
static GRHEAP_Result g_hook_result = GRHEAP_OK;
static void hook_destroy(void *) {
  g_hook_result = grheap_handle_set(g_hook_handle, nullptr);
}

// A destroy hook runs with the heap busy and may not retarget a handle.
TEST(Stats, AHandleCannotBeRetargetedFromADestroyHook) {
  HeapWorld w(steady());
  static const GRHEAP_Type kHooked = GRHEAP_TYPE_INIT("hooked", 8, nullptr, hook_destroy, nullptr, nullptr, nullptr);
  void * dying;
  void * kept;
  ASSERT_EQ(grheap_alloc(w.heap, &kHooked, &dying), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &kHooked, &kept), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_create(w.heap, kept, &g_hook_handle), GRHEAP_OK);
  g_hook_result = GRHEAP_OK;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(g_hook_result, GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_get(g_hook_handle), kept);
  g_hook_handle = nullptr;
}
