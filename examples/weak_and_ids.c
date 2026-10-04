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
 * Weak cells, ephemerons and stable IDs, in a C program with no engine.
 *
 *  - A *weak cell* names an object without keeping it alive. When the object
 *    is collected the collector sets the cell's slot to NULL and, after the
 *    sweep, calls the cell's `cleared` function once.
 *  - An *ephemeron* is a key and a value: the value is kept alive only for as
 *    long as the key is alive by some other path. A property table keyed by
 *    object is the usual use.
 *  - A *stable ID* names an object without its address, so it means the same
 *    thing if a later collector moves the object. The identity hash is a
 *    function of it.
 *
 * Every object this program needs across an allocation is held by a handle,
 * so it also runs under `GRHEAP_TORTURE=1 GRHEAP_VERIFY=1`, which collect at
 * every allocation and check every store.
 *
 * It uses `runtime-heap` and `runtime-core` (the context the heap attaches
 * to) and nothing else. `make examples` builds and runs it, and it exits
 * non-zero if any claim it makes about the collector turns out false.
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Stops the program if a claim is false. Not assert(): an example built with
 * NDEBUG would then prove nothing. */
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "example failed: %s (line %d)\n", #condition, __LINE__); \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static int destroyed = 0;
static int cells_cleared = 0;
static int properties_cleared = 0;

/* ---- A thing: a leaf object that counts its own death ------------------ */

typedef struct {
  int number;
} Thing;

static void thing_destroy(void * payload) {
  (void)payload;
  destroyed++;
}

static const GRHEAP_Type thing_type = {
    "thing", sizeof(Thing), NULL, thing_destroy, NULL, NULL, NULL};

/* ---- A weak cell ------------------------------------------------------- */

typedef struct {
  void * target; /* weak: the type's trace does not report it */
} Ref;

/* Called once, after the sweep, for a cell whose target was collected. The
 * slot is already NULL. Plain C: it may not allocate in the heap. */
static void ref_cleared(void * payload) {
  (void)payload;
  cells_cleared++;
}

static const GRHEAP_WeakSpec ref_weak = {
    GRHEAP_WEAK_CELL, offsetof(Ref, target), 0, 0, false, ref_cleared};
static const GRHEAP_Type ref_type = {
    "weak-ref", sizeof(Ref), NULL, NULL, &ref_weak, NULL, NULL};

/* ---- An ephemeron ------------------------------------------------------ */

typedef struct {
  void * key;
  void * value;
} Property;

static void property_cleared(void * payload) {
  (void)payload;
  properties_cleared++;
}

static const GRHEAP_WeakSpec property_weak = {GRHEAP_EPHEMERON, 0,
    offsetof(Property, key), offsetof(Property, value), false,
    property_cleared};
static const GRHEAP_Type property_type = {
    "property", sizeof(Property), NULL, NULL, &property_weak, NULL, NULL};

/* Allocates and holds with a handle, which is how an object survives the next
 * allocation (a GC point) without being stored anywhere yet. */
static void * make(GRHEAP_Heap * heap, const GRHEAP_Type * type,
    GRHEAP_Handle ** out_handle) {
  void * object;
  CHECK(grheap_alloc(heap, type, &object) == GRHEAP_OK);
  CHECK(grheap_handle_create(heap, object, out_handle) == GRHEAP_OK);
  return object;
}

int main(void) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
  CHECK(grheap_heap_create(context, NULL, &heap) == GRHEAP_OK);

  /* ---- A weak cell --------------------------------------------------- */
  printf("a weak cell\n");
  GRHEAP_Handle * ref_handle;
  GRHEAP_Handle * thing_handle;
  Ref * ref = make(heap, &ref_type, &ref_handle);
  void * thing = make(heap, &thing_type, &thing_handle);
  CHECK(grheap_store(heap, ref, &ref->target, thing) == GRHEAP_OK);

  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(ref->target == thing && cells_cleared == 0);
  printf("  the target is alive (a handle holds it), so the cell kept it\n");

  /* Let go of the target. The cell does not keep it alive. */
  CHECK(grheap_handle_destroy(thing_handle) == GRHEAP_OK);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(ref->target == NULL);
  CHECK(destroyed == 1 && cells_cleared == 1);
  printf("  the target was collected: the slot is NULL and `cleared` ran once\n");

  /* A second collection has nothing to report. */
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(cells_cleared == 1);

  /* ---- An ephemeron -------------------------------------------------- */
  printf("an ephemeron\n");
  GRHEAP_Handle * property_handle;
  GRHEAP_Handle * key_handle;
  Property * property = make(heap, &property_type, &property_handle);
  void * key = make(heap, &thing_type, &key_handle);
  /* The key first, then the value: a value with no key is cleared. */
  CHECK(grheap_store(heap, property, &property->key, key) == GRHEAP_OK);
  void * value;
  CHECK(grheap_alloc(heap, &thing_type, &value) == GRHEAP_OK);
  CHECK(grheap_store(heap, property, &property->value, value) == GRHEAP_OK);

  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(property->value == value);
  CHECK(destroyed == 1); /* the value is reachable only through the key */
  printf("  the key is alive, so the value is kept\n");

  CHECK(grheap_handle_destroy(key_handle) == GRHEAP_OK);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(property->key == NULL && property->value == NULL);
  CHECK(destroyed == 3); /* the key and the value, both */
  CHECK(properties_cleared == 1);
  printf("  the key went, so the value went with it and `cleared` ran once\n");

  /* ---- Stable IDs ---------------------------------------------------- */
  printf("stable IDs\n");
  GRHEAP_Handle * object_handle;
  void * object = make(heap, &thing_type, &object_handle);
  uint64_t id, again, hash;
  CHECK(grheap_id_of(heap, object, &id) == GRHEAP_OK);
  CHECK(grheap_id_of(heap, object, &again) == GRHEAP_OK);
  CHECK(id == again && id != 0);
  CHECK(grheap_identity_hash(heap, object, &hash) == GRHEAP_OK);
  void * back;
  CHECK(grheap_object_of_id(heap, id, &back) == GRHEAP_OK && back == object);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(grheap_object_of_id(heap, id, &back) == GRHEAP_OK && back == object);
  printf("  ID %llu names the object, before and after a collection\n",
      (unsigned long long)id);

  CHECK(grheap_handle_destroy(object_handle) == GRHEAP_OK);
  CHECK(grheap_collect(heap) == GRHEAP_OK);
  CHECK(grheap_object_of_id(heap, id, &back) == GRHEAP_ERR_INVALID);
  void * next = make(heap, &thing_type, &object_handle);
  uint64_t next_id;
  CHECK(grheap_id_of(heap, next, &next_id) == GRHEAP_OK);
  CHECK(next_id > id); /* an ID is never reused */
  printf("  the object died: its ID is unknown, and the next one is %llu\n",
      (unsigned long long)next_id);

  GRHEAP_Stats stats;
  CHECK(grheap_stats(heap, &stats) == GRHEAP_OK);
  CHECK(stats.weak_cleared == 1 && stats.ephemerons_cleared == 1);
  CHECK(stats.ids_issued == 2 && stats.ids_live == 1);
  printf("  %llu weak cell and %llu ephemeron cleared, %llu IDs issued\n",
      (unsigned long long)stats.weak_cleared,
      (unsigned long long)stats.ephemerons_cleared,
      (unsigned long long)stats.ids_issued);

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
