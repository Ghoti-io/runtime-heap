/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-heap.
 *
 * Ghoti.io Runtime-heap is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-heap is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The benchmark harness (AD-26).
 *
 * Every library ships one from its first commit, so that "performant" is a
 * claim with a way to check it. Besides the calibration case it holds the
 * cases of the collector: allocating a small object, allocating a 1 KiB
 * object, one full mark and sweep of a 10,000-node graph, a `grheap_store`,
 * a collection that clears 1,000 weak cells, and a stable-ID lookup. Every
 * world is made with GC torture and barrier-verify off, whatever the
 * environment says: a figure is a baseline, and a tortured run is read against
 * it.
 *
 * The calibration case is a fixed amount of integer work that touches no
 * library code, run the same way every real case will be, so a figure from a
 * real case can be read against the machine it was taken on: an allocation
 * that costs 20 ns means one thing next to a calibration of 1 ns per step and
 * another next to 6. A budget recorded without the calibration beside it
 * cannot be compared across hosts or compilers.
 *
 * Usage:
 *   bench           run every case: several repeats, report min and median
 *   bench --smoke   run every case once with a tiny workload (what `make
 *                   test` does); proves the harness builds, links and runs
 *
 * No numeric budget is asserted here. AD-26 records budgets once a first
 * measurement of a real case exists.
 */

/* clock_gettime(CLOCK_MONOTONIC) is POSIX, and -std=c17 hides it. A benchmark
 * wants a clock that cannot step backwards, so it asks for it by name. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>
#include <ghoti.io/runtime-core/b/roots.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  const char * name;
  /** Performs @p iterations units of work; returns a value derived from all
   *  of it so the compiler cannot discard the loop. */
  uint64_t (*run)(uint64_t iterations);
  uint64_t iterations;       /* per repeat, full run */
  uint64_t smoke_iterations; /* per repeat, --smoke */
} Case;

/* A case that cannot set itself up must not report a short, fast run as a
 * measurement. */
static _Noreturn void setup_failed(const char * what) {
  fprintf(stderr, "bench: setup failed: %s\n", what);
  abort();
}

static double now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* xorshift64: one dependent chain of shifts and xors per step. The chain is
 * serial on purpose, so the figure is latency-bound and does not move with
 * how wide the host's execution units are. */
static uint64_t calibration_run(uint64_t iterations) {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (uint64_t i = 0; i < iterations; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  return x;
}

/* A group, a context and its heap, torn down together. */
typedef struct {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
} World;

static void world_open(World * w) {
  if (grcore_group_create(NULL, NULL, &w->group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(w->group, NULL, &w->context) != GRCORE_OK) {
    setup_failed("context create");
  }
  /* The instruments are off by name, not by default: GRHEAP_TORTURE in the
   * environment would otherwise turn a baseline into a stress run. */
  GRHEAP_Options * options;
  if (grheap_options_create(NULL, &options) != GRHEAP_OK ||
      grheap_options_set_torture(options, false) != GRHEAP_OK ||
      grheap_options_set_verify(options, GRHEAP_VERIFY_OFF) != GRHEAP_OK) {
    setup_failed("options");
  }
  if (grheap_heap_create(w->context, options, &w->heap) != GRHEAP_OK) {
    setup_failed("heap create");
  }
  grheap_options_destroy(options);
}

static void world_close(World * w) {
  grcore_context_destroy(w->context);
  grcore_group_destroy(w->group);
}

typedef struct {
  void * next;
  void * other;
  uint64_t id;
} Node;

static void node_trace(GRHEAP_Tracer * tracer, void * payload) {
  Node * n = payload;
  grheap_trace_slot(tracer, &n->next);
  grheap_trace_slot(tracer, &n->other);
}

static const GRHEAP_Type node_type = {"node", sizeof(Node), node_trace, NULL, NULL, NULL, NULL};
static const GRHEAP_Type leaf_type = {"leaf", 16, NULL, NULL, NULL, NULL, NULL};

/* A small object, unreachable as soon as it is made: the price of an
 * allocation including the collections it triggers, the way a program that
 * makes short-lived garbage pays it. */
static uint64_t alloc_small_run(uint64_t iterations) {
  World w;
  world_open(&w);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    void * p;
    if (grheap_alloc(w.heap, &leaf_type, &p) != GRHEAP_OK) {
      setup_failed("alloc");
    }
    sink += (uint64_t)(uintptr_t)p;
  }
  world_close(&w);
  return sink;
}

/* A 1 KiB object, likewise: the cost of a bigger cell, its zeroing and a
 * collection that comes sooner for the bytes. */
static uint64_t alloc_1k_run(uint64_t iterations) {
  World w;
  world_open(&w);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    void * p;
    if (grheap_alloc_sized(w.heap, &leaf_type, 1024, &p) != GRHEAP_OK) {
      setup_failed("alloc 1k");
    }
    sink += (uint64_t)(uintptr_t)p;
  }
  world_close(&w);
  return sink;
}

#define GRAPH_NODES 10000u

/* One full mark and sweep of a 10,000-node graph that is all live: each node
 * points to the one before it and to an earlier node picked pseudo-randomly,
 * so marking follows both chains and cross-links. Sweeping finds nothing to
 * free, which is the cost of a collection that earns nothing. An iteration is
 * one collection. */
static uint64_t mark_sweep_run(uint64_t iterations) {
  World w;
  world_open(&w);
  void * nodes[GRAPH_NODES];
  void * head = NULL;
  if (grheap_root_add(w.heap, &head) != GRHEAP_OK) {
    setup_failed("root add");
  }
  uint64_t seed = 88172645463325252ull;
  for (uint32_t i = 0; i < GRAPH_NODES; i++) {
    void * p;
    if (grheap_alloc(w.heap, &node_type, &p) != GRHEAP_OK) {
      setup_failed("alloc node");
    }
    Node * n = p;
    n->id = i;
    n->next = head;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    n->other = i == 0 ? NULL : nodes[seed % i];
    nodes[i] = p;
    head = p; /* nothing allocates between here and the next link */
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    if (grheap_collect(w.heap) != GRHEAP_OK) {
      setup_failed("collect");
    }
    GRHEAP_Stats s;
    grheap_stats(w.heap, &s);
    sink += s.live_objects;
  }
  world_close(&w);
  return sink;
}

/* A store through the sanctioned function: a call, an owner check and a
 * counter, which is what a later barrier will add to. */
static uint64_t store_run(uint64_t iterations) {
  World w;
  world_open(&w);
  void * p;
  if (grheap_alloc(w.heap, &node_type, &p) != GRHEAP_OK) {
    setup_failed("alloc");
  }
  Node * n = p;
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    if (grheap_store(w.heap, n, &n->next, (i & 1u) ? p : NULL) != GRHEAP_OK) {
      setup_failed("store");
    }
    sink += (uint64_t)(uintptr_t)n->next;
  }
  world_close(&w);
  return sink;
}

#define WEAK_CELLS 1000u

typedef struct {
  void * next;   /* strong: the list of cells */
  void * target; /* weak */
} WeakCell;

static void weak_cell_trace(GRHEAP_Tracer * tracer, void * payload) {
  grheap_trace_slot(tracer, &((WeakCell *)payload)->next);
}

static const GRHEAP_WeakSpec weak_spec = {
    GRHEAP_WEAK_CELL, offsetof(WeakCell, target), 0, 0, false, NULL};
static const GRHEAP_Type weak_cell_type = {
    "weak-cell", sizeof(WeakCell), weak_cell_trace, NULL, &weak_spec, NULL, NULL};

/* One collection that clears 1,000 weak cells: the targets are made and
 * stored, nothing else holds them, and the collection finds them dead, nulls
 * the slots and walks the registry. An iteration is one such collection, so a
 * figure divided by 1,000 is the cost of clearing one cell. */
static uint64_t weak_clear_run(uint64_t iterations) {
  World w;
  world_open(&w);
  void * head = NULL;
  if (grheap_root_add(w.heap, &head) != GRHEAP_OK) {
    setup_failed("root add");
  }
  WeakCell * cells[WEAK_CELLS];
  for (uint32_t i = 0; i < WEAK_CELLS; i++) {
    void * p;
    if (grheap_alloc(w.heap, &weak_cell_type, &p) != GRHEAP_OK) {
      setup_failed("alloc weak cell");
    }
    WeakCell * c = p;
    c->next = head;
    head = p;
    cells[i] = c;
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    for (uint32_t k = 0; k < WEAK_CELLS; k++) {
      void * t;
      if (grheap_alloc(w.heap, &leaf_type, &t) != GRHEAP_OK) {
        setup_failed("alloc target");
      }
      if (grheap_store(w.heap, cells[k], &cells[k]->target, t) != GRHEAP_OK) {
        setup_failed("store target");
      }
    }
    if (grheap_collect(w.heap) != GRHEAP_OK) {
      setup_failed("collect");
    }
    GRHEAP_Stats s;
    grheap_stats(w.heap, &s);
    sink += s.weak_cleared;
  }
  world_close(&w);
  return sink;
}

#define ID_OBJECTS 10000u

/* A stable-ID lookup: the object-to-ID direction, an open-addressing probe,
 * for an object that already has its ID. An iteration is one lookup. */
static uint64_t id_lookup_run(uint64_t iterations) {
  World w;
  world_open(&w);
  void * head = NULL;
  if (grheap_root_add(w.heap, &head) != GRHEAP_OK) {
    setup_failed("root add");
  }
  void * nodes[ID_OBJECTS];
  for (uint32_t i = 0; i < ID_OBJECTS; i++) {
    void * p;
    if (grheap_alloc(w.heap, &node_type, &p) != GRHEAP_OK) {
      setup_failed("alloc node");
    }
    Node * n = p;
    n->next = head;
    head = p;
    nodes[i] = p;
    uint64_t id;
    if (grheap_id_of(w.heap, p, &id) != GRHEAP_OK) {
      setup_failed("id of");
    }
  }
  uint64_t seed = 88172645463325252ull;
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    uint64_t id;
    if (grheap_id_of(w.heap, nodes[seed % ID_OBJECTS], &id) != GRHEAP_OK) {
      setup_failed("id of");
    }
    sink += id;
  }
  world_close(&w);
  return sink;
}

/* ---- images ------------------------------------------------------------ */

static void image_roots_enumerate(
    GRCORE_Context * context, void * value, const GRCORE_RootVisitor * visitor) {
  (void)context;
  visitor->slot(visitor->user, value);
}

static const GRCORE_RootSource image_roots = {"bench", image_roots_enumerate};

static const GRHEAP_Type * image_resolve(void * user, const char * name) {
  (void)user;
  return strcmp(name, "node") == 0 ? &node_type : NULL;
}

/* The 10,000-node graph of `mark_sweep_run`, rooted by a root source instead
 * of a C root (an image refuses a C root), in a world of its own. */
static void image_graph(World * w, uint64_t * head_word) {
  world_open(w);
  if (grcore_context_add_root_source(w->context, &image_roots, head_word) !=
      GRCORE_OK) {
    setup_failed("root source");
  }
  void * nodes[GRAPH_NODES];
  uint64_t seed = 88172645463325252ull;
  for (uint32_t i = 0; i < GRAPH_NODES; i++) {
    void * p;
    if (grheap_alloc(w->heap, &node_type, &p) != GRHEAP_OK) {
      setup_failed("alloc node");
    }
    Node * n = p;
    n->id = i;
    n->next = (void *)(uintptr_t)*head_word;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    n->other = i == 0 ? NULL : nodes[seed % i];
    nodes[i] = p;
    *head_word = (uint64_t)(uintptr_t)p;
  }
}

/* One image of that graph taken and released. An iteration is one take. */
static uint64_t image_take_run(uint64_t iterations) {
  World w;
  uint64_t head = 0;
  image_graph(&w, &head);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRHEAP_Image * image;
    if (grheap_image_take(w.heap, NULL, &image) != GRHEAP_OK) {
      setup_failed("image take");
    }
    sink += grheap_image_size(image);
    grheap_image_release(image);
  }
  world_close(&w);
  return sink;
}

/* One image of that graph instantiated into a world of its own, including
 * making and destroying that world (a heap must be empty to take an image, and
 * an empty one is a new one). An iteration is one such restore. */
static uint64_t image_restore_run(uint64_t iterations) {
  World src;
  uint64_t head = 0;
  image_graph(&src, &head);
  GRHEAP_Image * image;
  if (grheap_image_take(src.heap, NULL, &image) != GRHEAP_OK) {
    setup_failed("image take");
  }
  uint64_t sink = 0;
  GRHEAP_RestoreEnv env = {image_resolve, NULL};
  for (uint64_t i = 0; i < iterations; i++) {
    World w;
    uint64_t root = 0;
    world_open(&w);
    if (grcore_context_add_root_source(w.context, &image_roots, &root) !=
        GRCORE_OK) {
      setup_failed("root source");
    }
    if (grheap_image_instantiate(w.heap, image, &env) != GRHEAP_OK) {
      setup_failed("image instantiate");
    }
    sink += root != 0;
    world_close(&w);
  }
  grheap_image_release(image);
  world_close(&src);
  return sink;
}

/* One retention query over a 100,000-object graph that is all live, for an
 * object nothing reaches: the search walks every object and finds nothing, which
 * is the most a query can cost on a heap this size. An iteration is one query,
 * scratch and result included. */
#define RETENTION_NODES 100000u

static uint64_t retention_run(uint64_t iterations) {
  World w;
  world_open(&w);
  void * head = NULL;
  void * first = NULL;
  if (grheap_root_add(w.heap, &head) != GRHEAP_OK) {
    setup_failed("root add");
  }
  uint64_t seed = 88172645463325252ull;
  static void * nodes[RETENTION_NODES];
  for (uint32_t i = 0; i < RETENTION_NODES; i++) {
    void * p;
    if (grheap_alloc(w.heap, &node_type, &p) != GRHEAP_OK) {
      setup_failed("alloc node");
    }
    Node * n = p;
    n->id = i;
    n->next = head;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    n->other = i == 0 ? NULL : nodes[seed % i];
    nodes[i] = p;
    if (i == 0) {
      first = p;
    }
    head = p; /* nothing allocates between here and the next link */
  }
  (void)first;
  void * lost;
  if (grheap_alloc(w.heap, &leaf_type, &lost) != GRHEAP_OK) {
    setup_failed("alloc lost");
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRHEAP_Retention * path;
    if (grheap_retention_path(w.heap, lost, NULL, &path) != GRHEAP_OK ||
        grheap_retention_retained(path)) {
      setup_failed("retention query");
    }
    sink += RETENTION_NODES;
    grheap_retention_release(path);
  }
  world_close(&w);
  return sink;
}

static const Case cases[] = {
    {"calibration", calibration_run, 200u * 1000u * 1000u, 1000u * 1000u},
    {"alloc-small", alloc_small_run, 20u * 1000u * 1000u, 100000u},
    {"alloc-1k", alloc_1k_run, 2u * 1000u * 1000u, 10000u},
    {"mark-sweep-10k", mark_sweep_run, 2000u, 3u},
    {"store", store_run, 200u * 1000u * 1000u, 100000u},
    {"weak-clear-1k", weak_clear_run, 20000u, 3u},
    {"id-lookup", id_lookup_run, 50u * 1000u * 1000u, 100000u},
    {"image-take-10k", image_take_run, 2000u, 3u},
    {"image-restore-10k", image_restore_run, 1000u, 3u},
    {"retention-100k", retention_run, 100u, 2u},
};

#define REPEATS 7

static int compare_double(const void * a, const void * b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char ** argv) {
  int smoke = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      smoke = 1;
    } else {
      fprintf(stderr, "bench: unknown argument '%s'\n", argv[i]);
      return 2;
    }
  }

  /* Naming the library's version proves the harness linked the library it
   * claims to measure, and ties every figure to the build that produced it. */
  printf("runtime-heap %s, %s run\n", grheap_version_string(),
      smoke ? "smoke" : "full");

  int repeats = smoke ? 1 : REPEATS;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    uint64_t n = smoke ? cases[c].smoke_iterations : cases[c].iterations;
    double ns[REPEATS];
    uint64_t sink = 0;
    for (int r = 0; r < repeats; r++) {
      double start = now_ns();
      sink ^= cases[c].run(n);
      ns[r] = now_ns() - start;
    }
    qsort(ns, (size_t)repeats, sizeof(ns[0]), compare_double);
    double best = ns[0] / (double)n;
    double median = ns[repeats / 2] / (double)n;
    if (!(best > 0.0)) {
      fprintf(stderr, "bench: %s measured no time; the clock is unusable\n",
          cases[c].name);
      return 1;
    }
    printf("%-18s best %10.3f ns/op  median %10.3f ns/op  (%llu ops x %d, "
           "check %016llx)\n",
        cases[c].name, best, median, (unsigned long long)n, repeats,
        (unsigned long long)sink);
  }
  return 0;
}
