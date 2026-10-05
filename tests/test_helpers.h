/**
 * @file
 *
 * Shared helpers for the Ghoti.io Runtime-heap unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRHEAP_TEST_HELPERS_H
#define GHOTI_IO_GRHEAP_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>
#include <ghoti.io/runtime-core/b/options.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/b/roots.h>
#include <ghoti.io/runtime-core/b/run.h>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>

/* A base allocator that counts, tracks live blocks, and can fail the Nth
 * allocation call (malloc, calloc or realloc, counted from 1), or every call
 * from the Nth on (`sticky`: a failure that survives the retry an allocation
 * makes after a collection), or every call while `fail_all` is set. The tests
 * that sweep N from 1 upward use it to reach every allocation-failure arm. */
struct TrackingAllocator {
  GRHEAP_Allocator vtable;
  long calls = 0;
  long fail_at = 0;      // 0: never fail
  bool sticky = false;   // with fail_at: fail every call from the Nth on
  bool fail_all = false; // fail every call while set
  long live = 0;         // successful allocations not yet freed

  TrackingAllocator(const TrackingAllocator &) = delete;
  TrackingAllocator & operator=(const TrackingAllocator &) = delete;
  TrackingAllocator() {
    vtable.ctx = this;
    vtable.malloc_fn = [](void * c, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::malloc(n ? n : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::calloc(n ? n : 1, m ? m : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * q = std::realloc(p, n ? n : 1);
      if (p == nullptr && q != nullptr) {
        t->live++;
      }
      return q;
    };
    vtable.free_fn = [](void * c, void * p) {
      if (p != nullptr) {
        static_cast<TrackingAllocator *>(c)->live--;
      }
      std::free(p);
    };
  }
  bool fails() {
    calls++;
    return fail_all ||
        (fail_at != 0 && (sticky ? calls >= fail_at : calls == fail_at));
  }
  const GRHEAP_Allocator * get() const { return &vtable; }
};

/* A page provider over the default one that counts live mappings and can be
 * told to fail, always (`fail`) or for the next `fail_next` maps. */
struct FakePages {
  GRCORE_PageProvider vtable =
      GRCORE_PAGE_PROVIDER_INIT(nullptr, 0, nullptr, nullptr, nullptr);
  bool fail = false;
  long fail_next = 0;
  long maps = 0;
  long live = 0;

  FakePages(const FakePages &) = delete;
  FakePages & operator=(const FakePages &) = delete;
  FakePages() {
    vtable.ctx = this;
    vtable.page_size = grcore_page_provider_default()->page_size;
    vtable.map = [](void * c, size_t n) -> void * {
      auto * f = static_cast<FakePages *>(c);
      if (f->fail) {
        return nullptr;
      }
      if (f->fail_next > 0) {
        f->fail_next--;
        return nullptr;
      }
      const GRCORE_PageProvider * d = grcore_page_provider_default();
      void * p = d->map(d->ctx, n);
      if (p != nullptr) {
        f->live++;
        f->maps++;
      }
      return p;
    };
    vtable.unmap = [](void * c, void * p, size_t n) {
      const GRCORE_PageProvider * d = grcore_page_provider_default();
      d->unmap(d->ctx, p, n);
      static_cast<FakePages *>(c)->live--;
    };
  }
};

/* What a test wants from its world. */
struct WorldConfig {
  GRHEAP_Mode mode = GRHEAP_MODE_COLLECTED;
  bool auto_collect = true;
  uint64_t threshold = GRHEAP_DEFAULT_GC_THRESHOLD;
  bool codec_set = false;
  GRHEAP_ValueCodec codec = {};
  uint64_t memory = GRCORE_UNLIMITED;
  uint64_t reserve = GRCORE_DEFAULT_MEMORY_RESERVE;
  bool create_heap = true;
  // -1 leaves the instrument at what the environment says (GRHEAP_TORTURE,
  // GRHEAP_VERIFY, which `make test-torture` sets); 0 and 1 (verify: 0 off,
  // 1 count, 2 abort) are explicit and win. A test that asserts exactly when a
  // collection happens, or that holds an object only in a C variable across an
  // allocation, turns torture off here.
  int torture = -1;
  int verify = -1;
};

/* `config` with torture off: for a test that asserts exactly when a collection
 * happens, or that holds an object only in a C variable across an allocation
 * (which torture exists to catch). */
inline WorldConfig steady(WorldConfig config = WorldConfig()) {
  config.torture = 0;
  return config;
}

/* A group over the tracking allocator and fake pages, one context, and (by
 * default) its heap. The destructor destroys them and then holds the
 * library to the contract that matters most here: after the context is gone
 * the allocator holds no live block and no mapping is left. (Every block the
 * context's meter counted reaches one of those two, so a meter that read
 * non-zero at the end would mean one of them leaked.) */
struct HeapWorld {
  TrackingAllocator allocator;
  FakePages pages;
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;
  GRHEAP_Heap * heap = nullptr;

  explicit HeapWorld(const WorldConfig & config = WorldConfig()) {
    GRCORE_Options * o;
    EXPECT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
    grcore_options_set_memory_bytes(o, config.memory);
    grcore_options_set_memory_reserve(o, config.reserve);
    EXPECT_EQ(grcore_group_create(allocator.get(), &pages.vtable, &group),
        GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, o, &ctx), GRCORE_OK);
    grcore_options_destroy(o);
    if (config.create_heap) {
      GRHEAP_Options * h;
      EXPECT_EQ(grheap_options_create(nullptr, &h), GRHEAP_OK);
      grheap_options_set_mode(h, config.mode);
      grheap_options_set_auto_collect(h, config.auto_collect);
      grheap_options_set_gc_threshold(h, config.threshold);
      if (config.codec_set) {
        EXPECT_EQ(grheap_options_set_value_codec(h, &config.codec), GRHEAP_OK);
      }
      if (config.torture >= 0) {
        grheap_options_set_torture(h, config.torture != 0);
      }
      if (config.verify >= 0) {
        grheap_options_set_verify(h, static_cast<GRHEAP_Verify>(config.verify));
      }
      EXPECT_EQ(grheap_heap_create(ctx, h, &heap), GRHEAP_OK);
      grheap_options_destroy(h);
    }
  }
  HeapWorld(const HeapWorld &) = delete;
  HeapWorld & operator=(const HeapWorld &) = delete;
  ~HeapWorld() {
    if (ctx != nullptr) {
      if (!grcore_context_is_owner(ctx)) {
        EXPECT_EQ(grcore_context_acquire(ctx), GRCORE_OK);
      }
      EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
    }
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
    EXPECT_EQ(allocator.live, 0) << "blocks leaked past context destroy";
    EXPECT_EQ(pages.live, 0) << "mappings leaked past context destroy";
  }

  GRHEAP_Stats stats() const {
    GRHEAP_Stats s = {};
    EXPECT_EQ(grheap_stats(heap, &s), GRHEAP_OK);
    return s;
  }
};

/* ---- A graph node ------------------------------------------------------ */

/* Two pointer slots and an identity. The slots are `void *` because that is
 * what grheap_trace_slot is declared to take, and a Node * slot reported
 * through it would be a pointer cast the aliasing rules do not bless. */
struct Node {
  void * a;
  void * b;
  uint64_t id;
};

inline std::vector<uint64_t> & destroyed_log() {
  static std::vector<uint64_t> log;
  return log;
}

inline void node_trace(GRHEAP_Tracer * t, void * payload) {
  auto * n = static_cast<Node *>(payload);
  grheap_trace_slot(t, &n->a);
  grheap_trace_slot(t, &n->b);
}

inline void node_destroy(void * payload) {
  destroyed_log().push_back(static_cast<Node *>(payload)->id);
}

inline const GRHEAP_Type kNode = GRHEAP_TYPE_INIT("node", sizeof(Node), node_trace, node_destroy, nullptr, nullptr, nullptr);

/* A node with no destroy hook, for the tests that count frees by statistics. */
inline const GRHEAP_Type kPlainNode = GRHEAP_TYPE_INIT("plain", sizeof(Node), node_trace, nullptr, nullptr, nullptr, nullptr);

/* A leaf: no pointers, no hook. */
inline const GRHEAP_Type kLeaf = GRHEAP_TYPE_INIT("leaf", 16, nullptr, nullptr, nullptr, nullptr, nullptr);

inline Node * new_node(GRHEAP_Heap * heap, uint64_t id,
    const GRHEAP_Type * type = &kNode) {
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(heap, type, &p), GRHEAP_OK);
  auto * n = static_cast<Node *>(p);
  if (n != nullptr) {
    n->id = id;
  }
  return n;
}

inline bool destroyed(uint64_t id) {
  for (uint64_t d : destroyed_log()) {
    if (d == id) {
      return true;
    }
  }
  return false;
}

inline size_t destroyed_count(uint64_t id) {
  size_t n = 0;
  for (uint64_t d : destroyed_log()) {
    if (d == id) {
      n++;
    }
  }
  return n;
}

/* ---- Root sources ------------------------------------------------------- */

/* A root source that reports a list of precise slots and conservative ranges
 * it holds. */
struct HeldRoots {
  std::vector<uint64_t *> slots;
  std::vector<GRCORE_ConservativeRange> ranges;
};

inline void held_enumerate(GRCORE_Context *, void * value,
    const GRCORE_RootVisitor * visitor) {
  auto * held = static_cast<HeldRoots *>(value);
  if (visitor->slot != nullptr) {
    for (uint64_t * s : held->slots) {
      visitor->slot(visitor->user, s);
    }
  }
  if (visitor->range != nullptr) {
    for (const auto & r : held->ranges) {
      visitor->range(visitor->user, &r);
    }
  }
}

inline const GRCORE_RootSource kHeldSource = GRCORE_ROOT_SOURCE_INIT("held", held_enumerate);

/* A range covering `count` words from `first`, read as raw addresses. */
inline GRCORE_ConservativeRange range_of(const void * first, size_t count) {
  auto lo = reinterpret_cast<uint64_t>(first);
  return GRCORE_ConservativeRange{lo, lo + count * 8u, UINT64_MAX, 0, 0};
}

inline uint64_t addr(const void * p) {
  return reinterpret_cast<uint64_t>(p);
}

/* ---- Running ------------------------------------------------------------ */

/* An entry function whose body is a lambda. */
struct Fn {
  std::function<GRCORE_Step(GRCORE_Context *)> body;
};
inline GRCORE_Step fn_entry(GRCORE_Context * c, void * state) {
  return static_cast<Fn *>(state)->body(c);
}

#endif // GHOTI_IO_GRHEAP_TEST_HELPERS_H
