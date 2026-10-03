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
 * A C program with no engine builds a cyclic object graph in a collected heap,
 * keeps it alive three ways, drops it and watches it be collected; then it
 * does the same work in an arena and drops the whole heap at once.
 *
 * It uses `runtime-heap` and `runtime-core` (the context the heap attaches
 * to) and nothing else: no engine, no frames, no A. `make examples` builds and
 * runs it, and it exits non-zero if any claim it makes about the collector
 * turns out false.
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdio.h>
#include <stdlib.h>

/* An object with one reference to another object, and a name. The heap knows
 * nothing about this layout: the type below says where the pointer is. */
typedef struct {
  void * peer;
  const char * name;
} Node;

static int destroyed = 0;
static int verbose = 1;

/* Reports each pointer slot of one node to the collector. */
static void node_trace(GRHEAP_Tracer * tracer, void * payload) {
  Node * node = payload;
  grheap_trace_slot(tracer, &node->peer);
}

/* Runs once when the node is freed. Plain C: it must not touch other heap
 * objects, which may already be gone. */
static void node_destroy(void * payload) {
  Node * node = payload;
  if (verbose) {
    printf("  destroy %s\n", node->name);
  }
  destroyed++;
}

static const GRHEAP_Type node_type = {
    "node", sizeof(Node), node_trace, node_destroy};

/* Stops the program if a claim is false. Not assert(): an example built with
 * NDEBUG would then prove nothing. */
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "example failed: %s (line %d)\n", #condition, __LINE__); \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/* Makes a node and sets its name. The node is unreachable until the caller
 * keeps it, and the next allocation is a point where it could be collected. */
static Node * make_node(GRHEAP_Heap * heap, const char * name) {
  void * object;
  CHECK(grheap_alloc(heap, &node_type, &object) == GRHEAP_OK);
  Node * node = object;
  node->name = name;
  return node;
}

/* A <-> B, joined with grheap_store, the only way to write a pointer into a
 * heap object. */
static Node * make_cycle(GRHEAP_Heap * heap, const char * a, const char * b,
    Node ** out_b) {
  Node * first = make_node(heap, a);
  Node * second = make_node(heap, b);
  CHECK(grheap_store(heap, first, &first->peer, second) == GRHEAP_OK);
  CHECK(grheap_store(heap, second, &second->peer, first) == GRHEAP_OK);
  *out_b = second;
  return first;
}

int main(void) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);

  /* ---- A collected heap ------------------------------------------------ */
  printf("collected heap\n");
  CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
  CHECK(grheap_heap_create(context, NULL, &heap) == GRHEAP_OK);

  /* 1. Kept by a registered root slot. A slot is how a C variable holds a
   *    pointer across a collection. */
  void * root = NULL;
  CHECK(grheap_root_add(heap, &root) == GRHEAP_OK);
  Node * b;
  root = make_cycle(heap, "A", "B", &b);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(destroyed == 0);
  CHECK(grheap_contains(heap, root) && grheap_contains(heap, b));
  printf("  rooted cycle survived a collection\n");

  /* Drop the root: the cycle is unreachable, and a cycle is no obstacle. */
  root = NULL;
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(destroyed == 2);
  printf("  unrooted cycle was collected, both hooks ran\n");
  CHECK(grheap_root_remove(heap, &root) == GRHEAP_OK);

  /* 2. Kept by a handle: a heap-owned cell for code that has no variable
   *    to register. */
  GRHEAP_Handle * handle;
  Node * d;
  Node * c = make_cycle(heap, "C", "D", &d);
  CHECK(grheap_handle_create(heap, c, &handle) == GRHEAP_OK);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(destroyed == 2 && grheap_handle_get(handle) == c);
  CHECK(grheap_handle_destroy(handle) == GRHEAP_OK);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(destroyed == 4);
  printf("  a handle kept a cycle until it was destroyed\n");

  /* 3. Kept by a pin: counted, so it nests. */
  Node * f;
  Node * e = make_cycle(heap, "E", "F", &f);
  CHECK(grheap_pin(heap, e) == GRHEAP_OK);
  CHECK(grheap_pin(heap, e) == GRHEAP_OK);
  CHECK(grheap_unpin(heap, e) == GRHEAP_OK);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(destroyed == 4); /* one pin left */
  CHECK(grheap_unpin(heap, e) == GRHEAP_OK);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(destroyed == 6);
  printf("  a pin kept a cycle until the last unpin\n");

  GRHEAP_Stats stats;
  CHECK(grheap_stats(heap, &stats) == GRHEAP_OK);
  printf("  %llu collections, %llu objects freed, %llu live, %llu stores\n",
      (unsigned long long)stats.collections, (unsigned long long)stats.frees,
      (unsigned long long)stats.live_objects, (unsigned long long)stats.stores);
  CHECK(stats.live_objects == 0);

  /* There is no heap destroy: the heap dies with its context. */
  CHECK(grcore_context_destroy(context) == GRCORE_OK);

  /* ---- An arena ------------------------------------------------------- */
  printf("arena\n");
  destroyed = 0;
  verbose = 0; /* two hundred hooks would drown the output */
  GRHEAP_Options * options;
  CHECK(grheap_options_create(NULL, &options) == GRHEAP_OK);
  CHECK(grheap_options_set_mode(options, GRHEAP_MODE_ARENA) == GRHEAP_OK);
  CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
  CHECK(grheap_heap_create(context, options, &heap) == GRHEAP_OK);
  grheap_options_destroy(options);

  for (int i = 0; i < 100; i++) {
    Node * other;
    make_cycle(heap, "arena-a", "arena-b", &other);
  }
  /* Nothing is rooted, and nothing is collected: an arena only drops whole. */
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(destroyed == 0);
  CHECK(grheap_stats(heap, &stats) == GRHEAP_OK);
  CHECK(stats.live_objects == 200 && stats.collections == 0);
  /* Reset runs every hook and gives every page back, keeping the heap. */
  CHECK(grheap_arena_reset(heap) == GRHEAP_OK);
  CHECK(destroyed == 200);
  CHECK(grheap_stats(heap, &stats) == GRHEAP_OK);
  CHECK(stats.live_objects == 0 && stats.mapped_bytes == 0);
  Node * again = make_node(heap, "after-reset");
  CHECK(grheap_contains(heap, again));
  printf("  200 hooks ran, no memory held, the heap is still usable\n");

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
