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
 * The retention query (retention.h): the shortest chain of references from a
 * root to one object, found breadth-first over the graph the collector walks.
 *
 * It is the collector's walk with the marking taken out. The roots come in the
 * collector's order (registered root slots, handles, pinned objects, then each
 * context root source in turn), each object's references come from its type's
 * `trace` through a tracer mode of its own (GRHEAP_TRACE_RETAIN, beside MARK,
 * VERIFY and the image's modes), and an ephemeron's value is reached through
 * its key by the same rule `weak.c` uses. Nothing here touches a mark bit, a
 * pin, an ID, a statistic or the mark stack: the only state it keeps is its
 * own scratch, which it frees on every path.
 *
 * The visited set is keyed by address. That is legal for the same reason it is
 * in the ID index: no GC point can occur during the query (the heap is busy and
 * no allocation in the GC heap is allowed), so an address names one object
 * from the first discovery to the last read. A moving collector would have to
 * key by something a move keeps (a forwarding table, or the stable ID the
 * object would be given for the query), and the result's addresses would have
 * to be reported through the same table.
 *
 * Scratch is one array of nodes (which is also the queue: a node is expanded
 * in the order it was discovered) and an open-addressing table over it, both
 * through the context's counting allocator and both grown by doubling. A
 * failure to grow stops the search and is reported once the scratch is freed.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/retention.h>

#include "heap_internal.h"

#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
/* A conservative range may cover poisoned memory; that is what it is for. */
#define GRHEAP_NO_ASAN __attribute__((no_sanitize("address")))
#else
#define GRHEAP_NO_ASAN
#endif

#define NO_PARENT SIZE_MAX

/* One discovered object. The root fields are meaningful only when `parent` is
 * NO_PARENT. */
typedef struct Node {
  void * payload;
  size_t parent;        ///< Index of the node that references this one.
  size_t slot_offset;   ///< In the parent's payload (or the ephemeron's).
  void * via;           ///< The ephemeron, for an ephemeron hop; else NULL.
  GRHEAP_RootKind kind;
  const char * source;
  size_t ordinal;
  size_t offset;
} Node;

/* The query's state, handed to the tracer by pointer. */
struct GRHEAP_Retain {
  GRHEAP_Heap * heap;
  void * target;
  Node * nodes;
  size_t count;
  size_t capacity;
  size_t * table;        ///< Node index plus one; zero is empty.
  size_t table_capacity; ///< A power of two.
  bool failed;
  bool invalid;          ///< `failed` because the query itself cannot be answered.
  uint64_t refusals_before;
  bool found;
  size_t found_index;
  /* The root being reported. */
  GRHEAP_RootKind root_kind;
  const char * root_source;
  size_t root_ordinal;
  size_t root_offset;
  size_t slot_count;     ///< Precise slots the current source has reported.
  size_t range_count;    ///< Conservative ranges it has reported.
  /* The node being expanded, for the references its trace reports. */
  size_t holder;
};

typedef struct GRHEAP_Retain Retain;

/* The result. One block: this header, then `steps`. */
struct GRHEAP_Retention {
  const GRHEAP_Allocator * allocator;
  bool retained;
  GRHEAP_RetentionRoot root;
  size_t count;
  GRHEAP_RetentionStep steps[];
};

/* ---- The visited set ---------------------------------------------------- */

static size_t slot_of(const Retain * r, const void * payload) {
  uint64_t h = (uint64_t)(uintptr_t)payload;
  h = (h >> 4) * UINT64_C(0x9E3779B97F4A7C15);
  h ^= h >> 29;
  return (size_t)h & (r->table_capacity - 1u);
}

static Node * lookup(const Retain * r, const void * payload, size_t * out_index) {
  if (r->table_capacity == 0) {
    return NULL;
  }
  size_t i = slot_of(r, payload);
  while (r->table[i] != 0) {
    Node * n = &r->nodes[r->table[i] - 1u];
    if (n->payload == payload) {
      if (out_index != NULL) {
        *out_index = r->table[i] - 1u;
      }
      return n;
    }
    i = (i + 1u) & (r->table_capacity - 1u);
  }
  return NULL;
}

static void table_insert(Retain * r, size_t node_index) {
  size_t i = slot_of(r, r->nodes[node_index].payload);
  while (r->table[i] != 0) {
    i = (i + 1u) & (r->table_capacity - 1u);
  }
  r->table[i] = node_index + 1u;
}

/* Room for one more node and its table entry, or `failed`. The table is kept
 * at most half full. */
static bool reserve(Retain * r) {
  const GRHEAP_Allocator * a = r->heap->allocator;
  if (r->count == r->capacity) {
    size_t cap = r->capacity == 0 ? 64u : r->capacity * 2u;
    if (cap < r->capacity || cap > SIZE_MAX / sizeof(Node)) {
      r->failed = true;
      return false;
    }
    Node * grown = a->realloc_fn(a->ctx, r->nodes, cap * sizeof *grown);
    if (grown == NULL) {
      r->failed = true;
      return false;
    }
    r->nodes = grown;
    r->capacity = cap;
  }
  if ((r->count + 1u) * 2u > r->table_capacity) {
    size_t cap = r->table_capacity == 0 ? 128u : r->table_capacity * 2u;
    if (cap < r->table_capacity || cap > SIZE_MAX / sizeof(size_t)) {
      r->failed = true;
      return false;
    }
    size_t * grown = a->calloc_fn(a->ctx, cap, sizeof *grown);
    if (grown == NULL) {
      r->failed = true;
      return false;
    }
    a->free_fn(a->ctx, r->table);
    r->table = grown;
    r->table_capacity = cap;
    for (size_t i = 0; i < r->count; i++) {
      table_insert(r, i);
    }
  }
  return true;
}

/* Records `payload` as reached, if it is new. The first discovery is the
 * shortest by construction: nodes are expanded in discovery order. */
static void discover(Retain * r, void * payload, size_t parent,
    size_t slot_offset, void * via) {
  if (r->failed || r->found || lookup(r, payload, NULL) != NULL) {
    return;
  }
  if (!reserve(r)) {
    return;
  }
  Node * n = &r->nodes[r->count];
  n->payload = payload;
  n->parent = parent;
  n->slot_offset = slot_offset;
  n->via = via;
  n->kind = r->root_kind;
  n->source = r->root_source;
  n->ordinal = r->root_ordinal;
  n->offset = r->root_offset;
  table_insert(r, r->count);
  if (payload == r->target) {
    r->found = true;
    r->found_index = r->count;
  }
  r->count++;
}

/* ---- Roots, in the collector's order ------------------------------------ */

/* A root holding an address that must be exactly a live payload start; any
 * other word is skipped without a count (a query changes no statistic). */
static void root_precise(Retain * r, uint64_t address) {
  GRHEAP_Span * span;
  size_t index;
  if (address == 0 || (uint64_t)(uintptr_t)address != address ||
      !grheap_locate(r->heap, (uintptr_t)address, true, &span, &index)) {
    return;
  }
  discover(r, grheap_payload(span, index), NO_PARENT, 0, NULL);
}

static void visit_slot(void * user, uint64_t * slot) {
  Retain * r = user;
  uint64_t address;
  r->root_kind = GRHEAP_ROOT_CONTEXT_SOURCE;
  r->root_ordinal = r->slot_count++;
  if (grheap_decode(r->heap, *slot, &address)) {
    root_precise(r, address);
  }
}

GRHEAP_NO_ASAN static void visit_range(
    void * user, const GRCORE_ConservativeRange * range) {
  Retain * r = user;
  r->root_kind = GRHEAP_ROOT_CONSERVATIVE;
  r->root_ordinal = r->range_count++;
  if (range->shift < 64u && range->hi > range->lo) {
    uint64_t lo = (range->lo + 7u) & ~UINT64_C(7);
    if (lo >= range->lo && lo < range->hi) {
      uint64_t count = (range->hi - lo) / 8u;
      for (uint64_t i = 0; i < count && !r->found && !r->failed; i++) {
        uint64_t word;
        __builtin_memcpy(
            &word, (const void *)(uintptr_t)(lo + i * 8u), sizeof word);
        uint64_t address =
            ((word & range->mask) >> range->shift) + range->base;
        GRHEAP_Span * span;
        size_t index;
        if ((uint64_t)(uintptr_t)address != address ||
            !grheap_locate(r->heap, (uintptr_t)address, false, &span, &index)) {
          continue;
        }
        r->root_offset = (size_t)(lo + i * 8u - range->lo);
        discover(r, grheap_payload(span, index), NO_PARENT, 0, NULL);
      }
    }
  }
  r->root_offset = 0;
}

static void enumerate_roots(Retain * r) {
  GRHEAP_Heap * heap = r->heap;

  r->root_kind = GRHEAP_ROOT_C_ROOT;
  for (size_t i = 0; i < heap->nroots && !r->found && !r->failed; i++) {
    r->root_ordinal = i;
    root_precise(r, (uint64_t)(uintptr_t)*heap->roots[i]);
  }

  r->root_kind = GRHEAP_ROOT_HANDLE;
  r->root_ordinal = 0;
  for (struct GRHEAP_Handle * h = heap->handles;
       h != NULL && !r->found && !r->failed; h = h->next) {
    root_precise(r, (uint64_t)(uintptr_t)h->object);
    r->root_ordinal++;
  }

  r->root_kind = GRHEAP_ROOT_PIN;
  r->root_ordinal = 0;
  for (size_t i = 0; i < heap->nspans && !r->found && !r->failed; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind != GRHEAP_SPAN_BLOCK || s->npinned == 0) {
      continue;
    }
    for (size_t c = 0; c < s->ncells; c++) {
      if (s->pins[c] != 0) {
        discover(r, grheap_payload(s, c), NO_PARENT, 0, NULL);
        r->root_ordinal++;
      }
    }
  }

  /* Each source is enumerated on its own, so that its name is known. Within a
   * source the precise slots and the ranges are numbered separately, in the
   * order the source reports them. */
  size_t sources = grcore_context_root_source_count(heap->context);
  GRCORE_RootVisitor visitor = {r, visit_slot, visit_range};
  for (size_t i = 0; i < sources && !r->found && !r->failed; i++) {
    const GRCORE_RootSource * source;
    void * value;
    if (grcore_context_root_source(heap->context, i, &source, &value) !=
        GRCORE_OK) {
      /* A truncated root set would answer "not retained" for an object a later
       * source holds: fail the query instead. (Reached only if a source
       * removed another during the enumeration, which roots.h forbids.) */
      r->failed = true;
      r->invalid = true;
      break;
    }
    r->root_source = source->name;
    r->slot_count = 0;
    r->range_count = 0;
    source->enumerate(heap->context, value, &visitor);
  }
  r->root_source = NULL;
}

/* ---- Expansion ----------------------------------------------------------- */

void grheap_retention_trace(GRHEAP_Tracer * tracer, void * slot, bool encoded) {
  Retain * r = tracer->retain;
  if (r == NULL || r->found || r->failed) {
    return;
  }
  uint64_t address;
  if (encoded) {
    if (!grheap_decode(r->heap, *(const uint64_t *)slot, &address)) {
      return;
    }
  } else {
    address = (uint64_t)(uintptr_t)*(void * const *)slot;
  }
  GRHEAP_Span * span;
  size_t index;
  if (address == 0 || (uint64_t)(uintptr_t)address != address ||
      !grheap_locate(r->heap, (uintptr_t)address, true, &span, &index)) {
    return;
  }
  /* A slot outside the payload (out-of-line storage, which AD-11 permits) has
   * no offset in it: SIZE_MAX says "not inline". */
  const unsigned char * at = slot;
  const unsigned char * base = tracer->payload;
  size_t offset = at >= base && (size_t)(at - base) < grheap_header(tracer->payload)->size
      ? (size_t)(at - base)
      : SIZE_MAX;
  discover(r, grheap_payload(span, index), r->holder, offset, NULL);
}

static void expand(Retain * r, size_t index) {
  void * payload = r->nodes[index].payload;
  const GRHEAP_Type * type = grheap_header(payload)->type;
  if (type->trace == NULL) {
    return;
  }
  GRHEAP_Tracer tracer = {.heap = r->heap, .mode = GRHEAP_TRACE_RETAIN,
      .payload = payload, .retain = r};
  r->holder = index;
  type->trace(&tracer, payload);
}

/* Expands every discovered node, in discovery order, until the target is found
 * or none is left. Nodes discovered meanwhile join the end of the queue. */
static void drain(Retain * r, size_t * next) {
  while (*next < r->count && !r->found && !r->failed) {
    expand(r, (*next)++);
  }
}

/* How a weak slot reads, as weak.c reads it. */
typedef enum { SLOT_NULL, SLOT_IMMEDIATE, SLOT_INVALID, SLOT_LIVE } SlotState;

static SlotState classify(const GRHEAP_Heap * heap, const GRHEAP_WeakSpec * spec,
    const unsigned char * payload, size_t offset, void ** out_payload) {
  uint64_t address;
  memcpy(&address, payload + offset, sizeof address);
  if (spec->encoded && !grheap_decode(heap, address, &address)) {
    return SLOT_IMMEDIATE;
  }
  if (address == 0) {
    return SLOT_NULL;
  }
  GRHEAP_Span * span;
  size_t index;
  if ((uint64_t)(uintptr_t)address != address ||
      !grheap_locate(heap, (uintptr_t)address, true, &span, &index)) {
    return SLOT_INVALID;
  }
  *out_payload = grheap_payload(span, index);
  return SLOT_LIVE;
}

/* One round of the ephemeron fixpoint, as the collector runs it: every
 * ephemeron the walk has reached, whose key the walk has reached (or is an
 * immediate), reaches its value. Returns whether anything was discovered. */
static bool ephemeron_round(Retain * r) {
  GRHEAP_Heap * heap = r->heap;
  size_t before = r->count;
  for (size_t e = 0; e < heap->nweak && !r->found && !r->failed; e++) {
    unsigned char * payload = heap->weak[e].object;
    const GRHEAP_WeakSpec * spec = grheap_header(payload)->type->weak;
    size_t eph_index;
    if (spec->kind != GRHEAP_EPHEMERON ||
        lookup(r, payload, &eph_index) == NULL) {
      continue;
    }
    void * key = NULL;
    size_t parent;
    switch (classify(heap, spec, payload, spec->key_offset, &key)) {
      case SLOT_IMMEDIATE:
        parent = eph_index;
        break;
      case SLOT_LIVE:
        if (lookup(r, key, &parent) == NULL) {
          continue;
        }
        break;
      default:
        continue;
    }
    void * value = NULL;
    if (classify(heap, spec, payload, spec->value_offset, &value) ==
        SLOT_LIVE) {
      discover(r, value, parent, spec->value_offset, payload);
    }
  }
  return r->count != before;
}

/* ---- The result ----------------------------------------------------------- */

static GRHEAP_Retention * make_result(const Retain * r, const GRHEAP_Allocator * a) {
  size_t steps = 0;
  if (r->found) {
    for (size_t i = r->found_index;; i = r->nodes[i].parent) {
      steps++;
      if (r->nodes[i].parent == NO_PARENT) {
        break;
      }
    }
  }
  if (steps > (SIZE_MAX - sizeof(GRHEAP_Retention)) / sizeof(GRHEAP_RetentionStep)) {
    return NULL;
  }
  GRHEAP_Retention * out = a->calloc_fn(a->ctx, 1,
      sizeof *out + steps * sizeof(GRHEAP_RetentionStep));
  if (out == NULL) {
    return NULL;
  }
  out->allocator = a;
  out->retained = r->found;
  out->count = steps;
  if (!r->found) {
    return out;
  }
  size_t child = SIZE_MAX;
  size_t at = steps;
  for (size_t i = r->found_index;; i = r->nodes[i].parent) {
    const Node * n = &r->nodes[i];
    GRHEAP_RetentionStep * step = &out->steps[--at];
    step->object = n->payload;
    step->type = grheap_header(n->payload)->type;
    step->target = child == SIZE_MAX;
    if (child != SIZE_MAX) {
      step->slot_offset = r->nodes[child].slot_offset;
      step->ephemeron = r->nodes[child].via;
    }
    child = i;
    if (n->parent == NO_PARENT) {
      out->root.kind = n->kind;
      out->root.source = n->source;
      out->root.ordinal = n->ordinal;
      out->root.offset = n->offset;
      break;
    }
  }
  return out;
}

GRHEAP_Result grheap_retention_path(GRHEAP_Heap * heap, const void * object,
    const GRHEAP_Allocator * allocator, GRHEAP_Retention ** out_path) {
  if (!grheap_usable(heap) || out_path == NULL || object == NULL ||
      heap->busy != 0 ||
      grcore_context_state(heap->context) == GRCORE_CONTEXT_RUNNING) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Span * span;
  size_t index;
  if (!grheap_locate(heap, (uintptr_t)object, true, &span, &index)) {
    return GRHEAP_ERR_INVALID;
  }
  if (allocator == NULL) {
    allocator = grheap_allocator_default();
  }

  Retain r = {.heap = heap, .target = (void *)(uintptr_t)object};
  r.refusals_before = grcore_context_memory_refusals(heap->context);
  /* Busy: a `trace` function that allocates in the heap is refused, exactly as
   * during a collection, and a second query cannot start inside this one. */
  heap->busy++;
  enumerate_roots(&r);
  size_t next = 0;
  drain(&r, &next);
  while (!r.found && !r.failed && ephemeron_round(&r)) {
    drain(&r, &next);
  }
  heap->busy--;

  GRHEAP_Retention * out = NULL;
  GRHEAP_Result result = GRHEAP_OK;
  if (r.failed) {
    result = r.invalid ? GRHEAP_ERR_INVALID
                       : grheap_failure(heap, r.refusals_before);
  } else {
    out = make_result(&r, allocator);
    if (out == NULL) {
      result = GRHEAP_ERR_OOM;
    }
  }
  heap->allocator->free_fn(heap->allocator->ctx, r.nodes);
  heap->allocator->free_fn(heap->allocator->ctx, r.table);
  if (result == GRHEAP_OK) {
    *out_path = out;
  }
  return result;
}

void grheap_retention_release(GRHEAP_Retention * path) {
  if (path != NULL) {
    const GRHEAP_Allocator * a = path->allocator;
    a->free_fn(a->ctx, path);
  }
}

bool grheap_retention_retained(const GRHEAP_Retention * path) {
  return path != NULL && path->retained;
}

GRHEAP_Result grheap_retention_root(
    const GRHEAP_Retention * path, GRHEAP_RetentionRoot * out_root) {
  if (path == NULL || out_root == NULL || !path->retained) {
    return GRHEAP_ERR_INVALID;
  }
  *out_root = path->root;
  return GRHEAP_OK;
}

size_t grheap_retention_step_count(const GRHEAP_Retention * path) {
  return path == NULL ? 0 : path->count;
}

GRHEAP_Result grheap_retention_step(const GRHEAP_Retention * path, size_t index,
    GRHEAP_RetentionStep * out_step) {
  if (path == NULL || out_step == NULL || index >= path->count) {
    return GRHEAP_ERR_INVALID;
  }
  *out_step = path->steps[index];
  return GRHEAP_OK;
}

const char * grheap_retention_root_kind_string(GRHEAP_RootKind kind) {
  switch (kind) {
    case GRHEAP_ROOT_CONTEXT_SOURCE: return "context root source";
    case GRHEAP_ROOT_C_ROOT: return "C root";
    case GRHEAP_ROOT_HANDLE: return "handle";
    case GRHEAP_ROOT_PIN: return "pin";
    case GRHEAP_ROOT_CONSERVATIVE: return "conservative range";
  }
  return "unknown";
}

void grheap_retention_dump(const GRHEAP_Retention * path, FILE * out) {
  if (path == NULL || out == NULL) {
    return;
  }
  if (!path->retained) {
    fputs("not retained: no root reaches the object\n", out);
    return;
  }
  const GRHEAP_RetentionRoot * root = &path->root;
  fprintf(out, "root: %s", grheap_retention_root_kind_string(root->kind));
  if (root->source != NULL) {
    fprintf(out, " \"%s\"", root->source);
  }
  fprintf(out, ", #%zu", root->ordinal);
  if (root->kind == GRHEAP_ROOT_CONSERVATIVE) {
    fprintf(out, ", word at offset %zu", root->offset);
  }
  fputc('\n', out);
  for (size_t i = 0; i < path->count; i++) {
    const GRHEAP_RetentionStep * s = &path->steps[i];
    const char * name = s->type != NULL && s->type->name != NULL
        ? s->type->name
        : "?";
    if (s->target) {
      fprintf(out, "  step %zu: object type \"%s\" (the target)\n", i, name);
    } else if (s->ephemeron != NULL) {
      fprintf(out,
          "  step %zu: object type \"%s\", ephemeron hop (offset %zu is in the ephemeron's payload)\n",
          i, name, s->slot_offset);
    } else {
      fprintf(out, "  step %zu: object type \"%s\", slot offset %zu\n", i, name,
          s->slot_offset);
    }
  }
}
