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
 * Why is this object still alive? A C embedder leaks an object on purpose and
 * asks the heap for the chain of references that keeps it.
 *
 * A session object holds a cache, the cache holds an entry, and the entry
 * still points at a large report the program meant to drop. The program asks
 * `grheap_retention_path` for the report and prints the answer: the root (a
 * registered root slot), then each object on the way, with the slot in each
 * that holds the next. It then clears the stale reference, asks again, and
 * sees the report is no longer retained (the collector would free it), which is
 * a different answer from an error.
 *
 * The addresses in the answer are valid only until the heap next collects or
 * the context runs, so the program prints types and offsets, not addresses; a
 * program that wants a durable name asks `grheap_id_of` for the objects it
 * cares about.
 *
 * Build and run with `make examples`.
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "retention_path: check failed at line %d: %s\n",         \
          __LINE__, #condition);                                               \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct {
  void * cache;
  int requests;
} Session;

typedef struct {
  void * entry;
  void * spare;
} Cache;

typedef struct {
  void * stale_report; /* the leak: nobody reads this any more */
  int key;
} Entry;

static void session_trace(GRHEAP_Tracer * t, void * p) {
  grheap_trace_slot(t, &((Session *)p)->cache);
}
static void cache_trace(GRHEAP_Tracer * t, void * p) {
  grheap_trace_slot(t, &((Cache *)p)->entry);
  grheap_trace_slot(t, &((Cache *)p)->spare);
}
static void entry_trace(GRHEAP_Tracer * t, void * p) {
  grheap_trace_slot(t, &((Entry *)p)->stale_report);
}

static const GRHEAP_Type session_type = GRHEAP_TYPE_INIT(
    "session", sizeof(Session), session_trace, NULL, NULL, NULL, NULL);
static const GRHEAP_Type cache_type = GRHEAP_TYPE_INIT(
    "cache", sizeof(Cache), cache_trace, NULL, NULL, NULL, NULL);
static const GRHEAP_Type entry_type = GRHEAP_TYPE_INIT(
    "entry", sizeof(Entry), entry_trace, NULL, NULL, NULL, NULL);
static const GRHEAP_Type report_type = GRHEAP_TYPE_INIT(
    "report", 4096, NULL, NULL, NULL, NULL, NULL);

int main(void) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
  CHECK(grheap_heap_create(context, NULL, &heap) == GRHEAP_OK);

  /* Build the leak. Each object is stored into the one before it the moment it
   * exists, so a collection between allocations could not lose it. */
  void * session = NULL;
  CHECK(grheap_root_add(heap, &session) == GRHEAP_OK);
  CHECK(grheap_alloc(heap, &session_type, &session) == GRHEAP_OK);
  void * cache, * entry, * report;
  CHECK(grheap_alloc(heap, &cache_type, &cache) == GRHEAP_OK);
  CHECK(grheap_store(heap, session, &((Session *)session)->cache, cache) ==
      GRHEAP_OK);
  CHECK(grheap_alloc(heap, &entry_type, &entry) == GRHEAP_OK);
  CHECK(grheap_store(heap, cache, &((Cache *)cache)->entry, entry) == GRHEAP_OK);
  CHECK(grheap_alloc(heap, &report_type, &report) == GRHEAP_OK);
  CHECK(grheap_store(heap, entry, &((Entry *)entry)->stale_report, report) ==
      GRHEAP_OK);

  /* Why is the report alive? */
  GRHEAP_Retention * path;
  CHECK(grheap_retention_path(heap, report, NULL, &path) == GRHEAP_OK);
  CHECK(grheap_retention_retained(path));
  grheap_retention_dump(path, stdout);

  GRHEAP_RetentionRoot root;
  CHECK(grheap_retention_root(path, &root) == GRHEAP_OK);
  CHECK(root.kind == GRHEAP_ROOT_C_ROOT && root.ordinal == 0);
  CHECK(grheap_retention_step_count(path) == 4);
  const char * names[4] = {"session", "cache", "entry", "report"};
  size_t offsets[3] = {offsetof(Session, cache), offsetof(Cache, entry),
      offsetof(Entry, stale_report)};
  for (size_t i = 0; i < 4; i++) {
    GRHEAP_RetentionStep step;
    CHECK(grheap_retention_step(path, i, &step) == GRHEAP_OK);
    CHECK(strcmp(step.type->name, names[i]) == 0);
    CHECK(step.target == (i == 3));
    if (i < 3) {
      CHECK(step.slot_offset == offsets[i]);
    }
  }
  grheap_retention_release(path);

  /* The collector agrees: the report survives a collection. */
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(grheap_contains(heap, report));

  /* Fix the leak, and ask again. "Not retained" is an answer, not an error. */
  CHECK(grheap_store(heap, entry, &((Entry *)entry)->stale_report, NULL) ==
      GRHEAP_OK);
  CHECK(grheap_retention_path(heap, report, NULL, &path) == GRHEAP_OK);
  CHECK(!grheap_retention_retained(path));
  grheap_retention_dump(path, stdout);
  grheap_retention_release(path);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(!grheap_contains(heap, report)); /* and a collection frees it */

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  printf("ok\n");
  return 0;
}
