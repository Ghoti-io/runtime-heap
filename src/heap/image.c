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
 * Heap images (AD-11, AD-12): taking one from the objects a context's roots
 * reach, instantiating one into a fresh heap, and the heap key's snapshot
 * hooks that carry one inside a context snapshot.
 *
 * **Take.** The roots a context's sources report are walked in the order the
 * collector walks them; every object found is numbered in discovery order, and
 * each object's payload is copied and then walked with its type's `trace`,
 * with the tracer in IMAGE mode. A slot the type reports that names a live
 * object becomes a record `{offset, target index, kind}` and the copy's slot
 * is cleared (to zero for a raw pointer; for an encoded word to the word
 * without its address bits, so a tag, if the codec has one, survives). The
 * types are recorded by name. A type's `snapshot` hook then gets the copy to
 * clear any host pointer it holds and to write what the pointer named.
 *
 * **Instantiate.** In three steps that the heap key spreads over a restore:
 * APPLY makes every object (a cell from the heap's own allocator, charged to
 * the context's budget; no collection can run), copies the payloads, patches
 * the recorded slots with the new addresses, runs the `restore` hooks, shadows
 * the slots for barrier-verify, and adopts the stable IDs; PREPARE checks that
 * the context's root sources report as many slots as the image has; COMMIT
 * writes them. A failure before COMMIT removes exactly the objects APPLY made
 * (every other cell is marked live and a sweep frees the rest, so the sweep's
 * own bookkeeping, the `destroy` hooks and the shadow forgetting apply).
 *
 * The image is one block of bytes with a header of section offsets, in native
 * byte order: it is an in-memory object and not a file format. Everything an
 * instantiation reads from it is bounds-checked first, because the same code
 * reads a blob a context snapshot carried.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <ghoti.io/runtime-heap/image.h>

#include <stdlib.h>
#include <string.h>

/* ---- the layout of an image ------------------------------------------ */

#define IMAGE_MAGIC UINT64_C(0x3130474D49485247) /* "GRHIMG01" */
#define IMAGE_VERSION UINT64_C(1)

typedef struct ImgHeader {
  uint64_t magic;
  uint64_t version;
  uint64_t tag_mask, tag_value, mask, shift, base; /* the codec */
  uint64_t next_id;
  uint64_t nobjects, ntypes, nroots, nslots;
  uint64_t types_off, objects_off, slots_off, roots_off;
  uint64_t data_off, extra_off, strings_off;
  uint64_t total;
} ImgHeader;

typedef struct ImgType {
  uint64_t name_off;
  uint64_t name_len;
} ImgType;

typedef struct ImgObject {
  uint32_t type;
  uint32_t nslots;
  uint64_t size;
  uint64_t payload_off; /* from data_off */
  uint64_t id;          /* zero: none */
  uint64_t slots_first; /* index into the slot table */
  uint64_t extra_off;   /* from extra_off */
  uint64_t extra_len;
} ImgObject;

typedef struct ImgSlot {
  uint32_t offset; /* in the payload */
  uint32_t target; /* an object index */
  uint32_t kind;   /* 0: a raw pointer; 1: an encoded word */
  uint32_t pad;
} ImgSlot;

typedef struct ImgRoot {
  uint64_t word;        /* the word, or the template of an encoded pointer */
  uint64_t target_plus1; /* zero: the word is as it is */
} ImgRoot;

enum { SLOT_RAW = 0, SLOT_WORD = 1 };

struct GRHEAP_Image {
  GRHEAP_Allocator allocator; /* a copy: the image outlives its caller's */
  size_t refs;                /* `__atomic` builtins only */
  size_t size;
  _Alignas(16) unsigned char bytes[];
};

static size_t align8(size_t n) {
  return (n + 7u) & ~(size_t)7u;
}

/* ---- the codec, both ways ---------------------------------------------- */

static uint64_t encode_word(
    const GRHEAP_ValueCodec * c, uint64_t template_word, uint64_t address) {
  return template_word | (((address - c->base) << c->shift) & c->mask);
}

/* ---- the image object --------------------------------------------------- */

GRHEAP_Image * grheap_image_retain(GRHEAP_Image * image) {
  if (image != NULL) {
    __atomic_add_fetch(&image->refs, 1, __ATOMIC_RELAXED);
  }
  return image;
}

void grheap_image_release(GRHEAP_Image * image) {
  if (image == NULL) {
    return;
  }
  if (__atomic_sub_fetch(&image->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    GRHEAP_Allocator a = image->allocator;
    a.free_fn(a.ctx, image);
  }
}

size_t grheap_image_refcount(const GRHEAP_Image * image) {
  return image == NULL ? 0 : __atomic_load_n(&image->refs, __ATOMIC_ACQUIRE);
}

size_t grheap_image_size(const GRHEAP_Image * image) {
  return image == NULL ? 0 : image->size;
}

const void * grheap_image_bytes(const GRHEAP_Image * image, size_t * out_size) {
  if (out_size != NULL) {
    *out_size = image == NULL ? 0 : image->size;
  }
  return image == NULL ? NULL : image->bytes;
}

static bool read_header(const unsigned char * bytes, size_t size, ImgHeader * h);

size_t grheap_image_object_count(const GRHEAP_Image * image) {
  ImgHeader h;
  return image != NULL && read_header(image->bytes, image->size, &h)
      ? (size_t)h.nobjects
      : 0;
}

size_t grheap_image_root_count(const GRHEAP_Image * image) {
  ImgHeader h;
  return image != NULL && read_header(image->bytes, image->size, &h)
      ? (size_t)h.nroots
      : 0;
}

/* ---- the writer and the reader a type's hooks are given ------------------ */

typedef struct Buffer {
  unsigned char * p;
  size_t len;
  size_t cap;
} Buffer;

/* Room for `more` bytes at the end, through `a`; false leaves it as it was. */
static bool buffer_reserve(const GRHEAP_Allocator * a, Buffer * b, size_t more) {
  if (more > SIZE_MAX - b->len) {
    return false;
  }
  size_t need = b->len + more;
  if (need <= b->cap) {
    return true;
  }
  size_t cap = b->cap == 0 ? 256u : b->cap;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u) {
      return false;
    }
    cap *= 2u;
  }
  unsigned char * grown = a->realloc_fn(a->ctx, b->p, cap);
  if (grown == NULL) {
    return false;
  }
  b->p = grown;
  b->cap = cap;
  return true;
}

/* A growing array of `element` bytes each: room for one more after `count`.
 * Returns the array (moved if it grew), or NULL with the array as it was. An
 * array with room is never NULL, so NULL always means failure. */
static void * array_reserve(const GRHEAP_Allocator * a, void * array,
    size_t * cap, size_t count, size_t element) {
  if (count < *cap) {
    return array;
  }
  size_t grown_cap = *cap == 0 ? 16u : *cap * 2u;
  if (grown_cap > SIZE_MAX / element) {
    return NULL;
  }
  void * grown = a->realloc_fn(a->ctx, array, grown_cap * element);
  if (grown == NULL) {
    return NULL;
  }
  *cap = grown_cap;
  return grown;
}

typedef struct GRHEAP_Take {
  GRHEAP_Heap * heap;
  const GRHEAP_Allocator * a;
  GRHEAP_Result failure; /* the first, sticky */
  bool range_seen;

  ImgObject * objs;
  void ** payloads;
  size_t nobjs, objs_cap, payloads_cap;
  uint32_t * table; /* object index + 1, open addressing by address */
  size_t table_cap;

  const GRHEAP_Type ** types;
  size_t ntypes, types_cap;

  ImgSlot * slots;
  size_t nslots, slots_cap;

  ImgRoot * roots;
  size_t nroots, roots_cap;

  Buffer data;
  Buffer extra;
  Buffer names; /* NUL-terminated type names, in type order */
  uint64_t * name_offsets;
  size_t name_offsets_cap;
} GRHEAP_Take;

struct GRHEAP_ImageWriter {
  GRHEAP_Take * take;
};

GRHEAP_Result grheap_image_writer_write(
    GRHEAP_ImageWriter * writer, const void * data, size_t size) {
  if (writer == NULL || (data == NULL && size != 0)) {
    return GRHEAP_ERR_INVALID;
  }
  if (size == 0) {
    return GRHEAP_OK;
  }
  GRHEAP_Take * t = writer->take;
  if (!buffer_reserve(t->a, &t->extra, size)) {
    return GRHEAP_ERR_OOM;
  }
  memcpy(t->extra.p + t->extra.len, data, size);
  t->extra.len += size;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_image_writer_string(
    GRHEAP_ImageWriter * writer, const char * text) {
  size_t length = text == NULL ? 0 : strlen(text);
  uint64_t n = length;
  GRHEAP_Result r = grheap_image_writer_write(writer, &n, sizeof n);
  if (r != GRHEAP_OK) {
    return r;
  }
  return grheap_image_writer_write(writer, text == NULL ? "" : text, length + 1);
}

struct GRHEAP_ImageReader {
  const unsigned char * data;
  size_t size;
  size_t at;
  bool failed;
};

GRHEAP_Result grheap_image_reader_read(
    GRHEAP_ImageReader * reader, void * out, size_t size) {
  if (reader == NULL || (out == NULL && size != 0)) {
    return GRHEAP_ERR_INVALID;
  }
  if (reader->failed || size > reader->size - reader->at) {
    reader->failed = true;
    return GRHEAP_ERR_CORRUPT;
  }
  if (size != 0) {
    memcpy(out, reader->data + reader->at, size);
  }
  reader->at += size;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_image_reader_string(
    GRHEAP_ImageReader * reader, const char ** out_text, size_t * out_length) {
  uint64_t n;
  if (out_text == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Result r = grheap_image_reader_read(reader, &n, sizeof n);
  if (r != GRHEAP_OK) {
    return r;
  }
  if (n >= reader->size - reader->at ||
      reader->data[reader->at + (size_t)n] != '\0') {
    reader->failed = true;
    return GRHEAP_ERR_CORRUPT;
  }
  *out_text = (const char *)reader->data + reader->at;
  reader->at += (size_t)n + 1;
  if (out_length != NULL) {
    *out_length = (size_t)n;
  }
  return GRHEAP_OK;
}

/* ---- take: the table of objects by address -------------------------------- */

static size_t address_hash(uintptr_t address) {
  uint64_t x = (uint64_t)(address >> 4);
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return (size_t)x;
}

static void take_fail(GRHEAP_Take * t, GRHEAP_Result why) {
  if (t->failure == GRHEAP_OK) {
    t->failure = why;
  }
}

static bool table_rebuild(GRHEAP_Take * t, size_t cap) {
  uint32_t * fresh = t->a->calloc_fn(t->a->ctx, cap, sizeof *fresh);
  if (fresh == NULL) {
    return false;
  }
  for (size_t i = 0; i < t->nobjs; i++) {
    size_t at = address_hash((uintptr_t)t->payloads[i]) & (cap - 1u);
    while (fresh[at] != 0) {
      at = (at + 1u) & (cap - 1u);
    }
    fresh[at] = (uint32_t)(i + 1u);
  }
  t->a->free_fn(t->a->ctx, t->table);
  t->table = fresh;
  t->table_cap = cap;
  return true;
}

/* The index of `payload` in the image, numbering it if it is new. Returns
 * UINT32_MAX after recording a failure. */
static uint32_t take_intern(GRHEAP_Take * t, void * payload) {
  if (t->table_cap != 0) {
    size_t at = address_hash((uintptr_t)payload) & (t->table_cap - 1u);
    while (t->table[at] != 0) {
      if (t->payloads[t->table[at] - 1u] == payload) {
        return t->table[at] - 1u;
      }
      at = (at + 1u) & (t->table_cap - 1u);
    }
  }
  if (t->nobjs >= UINT32_MAX - 1u) {
    take_fail(t, GRHEAP_ERR_LIMIT);
    return UINT32_MAX;
  }
  if (t->table_cap == 0 || (t->nobjs + 1u) * 2u > t->table_cap) {
    size_t cap = t->table_cap == 0 ? 64u : t->table_cap * 2u;
    if (!table_rebuild(t, cap)) {
      take_fail(t, GRHEAP_ERR_OOM);
      return UINT32_MAX;
    }
  }
  void * objs = array_reserve(
      t->a, t->objs, &t->objs_cap, t->nobjs, sizeof *t->objs);
  if (objs == NULL) {
    take_fail(t, GRHEAP_ERR_OOM);
    return UINT32_MAX;
  }
  t->objs = objs;
  void * payloads = array_reserve(
      t->a, t->payloads, &t->payloads_cap, t->nobjs, sizeof *t->payloads);
  if (payloads == NULL) {
    take_fail(t, GRHEAP_ERR_OOM);
    return UINT32_MAX;
  }
  t->payloads = payloads;
  uint32_t index = (uint32_t)t->nobjs++;
  memset(&t->objs[index], 0, sizeof t->objs[index]);
  t->payloads[index] = payload;
  size_t at = address_hash((uintptr_t)payload) & (t->table_cap - 1u);
  while (t->table[at] != 0) {
    at = (at + 1u) & (t->table_cap - 1u);
  }
  t->table[at] = index + 1u;
  return index;
}

/* What a word names: false if it is not a pointer at all or a null pointer;
 * `*out_payload` is set when it names a live object. */
typedef enum { WORD_PLAIN = 0, WORD_OBJECT, WORD_STALE } WordKind;

static WordKind classify(const GRHEAP_Heap * heap, uint64_t word, bool encoded,
    uint64_t * out_address) {
  uint64_t address;
  if (encoded) {
    if (!grheap_decode(heap, word, &address)) {
      return WORD_PLAIN;
    }
  } else {
    address = word;
  }
  if (address == 0) {
    return WORD_PLAIN;
  }
  GRHEAP_Span * span;
  size_t index;
  if ((uint64_t)(uintptr_t)address != address ||
      !grheap_locate(heap, (uintptr_t)address, true, &span, &index)) {
    return WORD_STALE;
  }
  *out_address = address;
  return WORD_OBJECT;
}

void grheap_image_trace(GRHEAP_Tracer * tracer, void * slot, bool encoded) {
  GRHEAP_Take * t = tracer->take;
  if (t->failure != GRHEAP_OK) {
    return;
  }
  /* A slot outside the payload cannot be patched at the destination: the
   * type keeps references somewhere the image does not copy. */
  uintptr_t s = (uintptr_t)slot;
  uintptr_t b = (uintptr_t)tracer->base;
  if (tracer->size < 8u || s < b || s - b > tracer->size - 8u ||
      (s - b) > UINT32_MAX) {
    take_fail(t, GRHEAP_ERR_INVALID);
    return;
  }
  const GRHEAP_ValueCodec * c = &t->heap->codec;
  uint64_t word;
  memcpy(&word, slot, sizeof word);
  uint64_t address = 0;
  WordKind kind = classify(t->heap, word, encoded, &address);
  if (kind == WORD_PLAIN) {
    return;
  }
  uint64_t template_word = encoded ? (word & ~c->mask) : 0;
  if (kind == WORD_OBJECT) {
    /* The word must be exactly what the destination will rebuild. */
    if (encoded && encode_word(c, template_word, address) != word) {
      take_fail(t, GRHEAP_ERR_INVALID);
      return;
    }
    uint32_t target = take_intern(t, (void *)(uintptr_t)address);
    if (target == UINT32_MAX) {
      return;
    }
    void * slots = array_reserve(
        t->a, t->slots, &t->slots_cap, t->nslots, sizeof *t->slots);
    if (slots == NULL) {
      take_fail(t, GRHEAP_ERR_OOM);
      return;
    }
    t->slots = slots;
    t->slots[t->nslots++] = (ImgSlot){(uint32_t)(s - b), target,
        encoded ? SLOT_WORD : SLOT_RAW, 0};
  }
  /* An object slot and a stale one are both cleared in the copy: a stale one
   * names nothing live, and an address must not be in an image. */
  memcpy(slot, &template_word, sizeof template_word);
}

static void take_root_slot(void * user, uint64_t * slot) {
  GRHEAP_Take * t = user;
  if (t->failure != GRHEAP_OK) {
    return;
  }
  const GRHEAP_ValueCodec * c = &t->heap->codec;
  uint64_t word = *slot;
  uint64_t address = 0;
  WordKind kind = classify(t->heap, word, true, &address);
  void * roots = array_reserve(
      t->a, t->roots, &t->roots_cap, t->nroots, sizeof *t->roots);
  if (roots == NULL) {
    take_fail(t, GRHEAP_ERR_OOM);
    return;
  }
  t->roots = roots;
  ImgRoot root = {word, 0};
  if (kind != WORD_PLAIN) {
    root.word = word & ~c->mask;
    if (kind == WORD_OBJECT) {
      if (encode_word(c, root.word, address) != word) {
        take_fail(t, GRHEAP_ERR_INVALID);
        return;
      }
      uint32_t target = take_intern(t, (void *)(uintptr_t)address);
      if (target == UINT32_MAX) {
        return;
      }
      root.target_plus1 = (uint64_t)target + 1u;
    }
  }
  t->roots[t->nroots++] = root;
}

static void take_root_range(void * user, const GRCORE_ConservativeRange * range) {
  GRHEAP_Take * t = user;
  if (range->hi > range->lo) {
    t->range_seen = true;
  }
}

/* The index of `type` in the take's table, adding it. UINT32_MAX after a
 * failure. */
static uint32_t take_type(GRHEAP_Take * t, const GRHEAP_Type * type) {
  for (size_t i = 0; i < t->ntypes; i++) {
    if (t->types[i] == type) {
      return (uint32_t)i;
    }
  }
  if (type->name == NULL || grheap_type_weak(type) != NULL) {
    take_fail(t, GRHEAP_ERR_INVALID);
    return UINT32_MAX;
  }
  for (size_t i = 0; i < t->ntypes; i++) {
    if (strcmp(t->types[i]->name, type->name) == 0) {
      take_fail(t, GRHEAP_ERR_INVALID); /* two types, one name */
      return UINT32_MAX;
    }
  }
  size_t length = strlen(type->name) + 1u;
  void * types = array_reserve(
      t->a, (void *)t->types, &t->types_cap, t->ntypes, sizeof *t->types);
  if (types == NULL) {
    take_fail(t, GRHEAP_ERR_OOM);
    return UINT32_MAX;
  }
  t->types = types;
  void * offsets = array_reserve(t->a, t->name_offsets, &t->name_offsets_cap,
      t->ntypes, sizeof *t->name_offsets);
  if (offsets == NULL || !buffer_reserve(t->a, &t->names, length)) {
    /* `types` stays grown: the table keeps it for the next type. */
    if (offsets != NULL) {
      t->name_offsets = offsets;
    }
    take_fail(t, GRHEAP_ERR_OOM);
    return UINT32_MAX;
  }
  t->name_offsets = offsets;
  t->name_offsets[t->ntypes] = t->names.len;
  memcpy(t->names.p + t->names.len, type->name, length);
  t->names.len += length;
  t->types[t->ntypes] = type;
  return (uint32_t)t->ntypes++;
}

/* Copies object `i` and walks the copy. */
static void take_object(GRHEAP_Take * t, size_t i) {
  GRHEAP_Heap * heap = t->heap;
  void * payload = t->payloads[i];
  const GRHEAP_Header * header = grheap_header(payload);
  const GRHEAP_Type * type = header->type;
  uint64_t size = header->size;
  uint32_t type_index = take_type(t, type);
  if (type_index == UINT32_MAX) {
    return;
  }
  size_t off = align8(t->data.len);
  if (size > SIZE_MAX / 2u || !buffer_reserve(t->a, &t->data,
          off - t->data.len + align8((size_t)size))) {
    take_fail(t, GRHEAP_ERR_OOM);
    return;
  }
  memset(t->data.p + t->data.len, 0, off - t->data.len);
  memcpy(t->data.p + off, payload, (size_t)size);
  memset(t->data.p + off + size, 0, align8((size_t)size) - (size_t)size);
  t->data.len = off + align8((size_t)size);

  ImgObject * obj = &t->objs[i];
  obj->type = type_index;
  obj->size = size;
  obj->payload_off = off;
  obj->slots_first = t->nslots;
  obj->id = grheap_id_lookup(heap, payload);

  unsigned char * copy = t->data.p + off;
  if (type->trace != NULL) {
    GRHEAP_Tracer tracer = {.heap = heap, .mode = GRHEAP_TRACE_IMAGE};
    tracer.take = t;
    tracer.base = copy;
    tracer.size = (size_t)size;
    tracer.payload = copy;
    type->trace(&tracer, copy);
  }
  /* `objs` may have moved while the walk numbered new objects. */
  obj = &t->objs[i];
  obj->nslots = (uint32_t)(t->nslots - obj->slots_first);
  if (t->failure != GRHEAP_OK) {
    return;
  }
  obj->extra_off = t->extra.len;
  if (grheap_type_snapshot(type) != NULL) {
    GRHEAP_ImageWriter writer = {t};
    GRHEAP_Result r = grheap_type_snapshot(type)(heap->context, copy, &writer);
    if (r != GRHEAP_OK) {
      take_fail(t, r);
      return;
    }
  }
  t->objs[i].extra_len = t->extra.len - t->objs[i].extra_off;
}

static void take_free(GRHEAP_Take * t) {
  const GRHEAP_Allocator * a = t->a;
  a->free_fn(a->ctx, t->objs);
  a->free_fn(a->ctx, t->payloads);
  a->free_fn(a->ctx, t->table);
  a->free_fn(a->ctx, (void *)t->types);
  a->free_fn(a->ctx, t->slots);
  a->free_fn(a->ctx, t->roots);
  a->free_fn(a->ctx, t->data.p);
  a->free_fn(a->ctx, t->extra.p);
  a->free_fn(a->ctx, t->names.p);
  a->free_fn(a->ctx, t->name_offsets);
}

/* Lays out the sections and builds the image block. */
static GRHEAP_Result take_assemble(GRHEAP_Take * t, GRHEAP_Image ** out) {
  const GRHEAP_Heap * heap = t->heap;
  size_t cursor = align8(sizeof(ImgHeader));
  ImgHeader h;
  memset(&h, 0, sizeof h);
  h.magic = IMAGE_MAGIC;
  h.version = IMAGE_VERSION;
  h.tag_mask = heap->codec.tag_mask;
  h.tag_value = heap->codec.tag_value;
  h.mask = heap->codec.mask;
  h.shift = heap->codec.shift;
  h.base = heap->codec.base;
  h.next_id = heap->next_id;
  h.nobjects = t->nobjs;
  h.ntypes = t->ntypes;
  h.nroots = t->nroots;
  h.nslots = t->nslots;
  /* The caps keep every product below SIZE_MAX / 2: objects are bounded by
   * the uint32 index, and the arrays by what was allocated. */
  h.types_off = cursor;
  cursor += t->ntypes * sizeof(ImgType);
  h.objects_off = cursor;
  cursor += t->nobjs * sizeof(ImgObject);
  h.slots_off = cursor;
  cursor += t->nslots * sizeof(ImgSlot);
  h.roots_off = cursor;
  cursor += t->nroots * sizeof(ImgRoot);
  h.strings_off = cursor;
  cursor += align8(t->names.len);
  h.data_off = cursor;
  cursor += align8(t->data.len);
  h.extra_off = cursor;
  cursor += align8(t->extra.len);
  h.total = cursor;
  if (cursor < h.data_off || cursor < h.extra_off) {
    return GRHEAP_ERR_LIMIT;
  }
  GRHEAP_Image * image =
      t->a->calloc_fn(t->a->ctx, 1, sizeof *image + cursor);
  if (image == NULL) {
    return GRHEAP_ERR_OOM;
  }
  image->allocator = *t->a;
  image->refs = 1;
  image->size = cursor;
  unsigned char * b = image->bytes;
  memcpy(b, &h, sizeof h);
  for (size_t i = 0; i < t->ntypes; i++) {
    ImgType rec = {h.strings_off + t->name_offsets[i],
        strlen(t->types[i]->name)};
    memcpy(b + h.types_off + i * sizeof rec, &rec, sizeof rec);
  }
  if (t->nobjs != 0) {
    memcpy(b + h.objects_off, t->objs, t->nobjs * sizeof(ImgObject));
  }
  if (t->nslots != 0) {
    memcpy(b + h.slots_off, t->slots, t->nslots * sizeof(ImgSlot));
  }
  if (t->nroots != 0) {
    memcpy(b + h.roots_off, t->roots, t->nroots * sizeof(ImgRoot));
  }
  if (t->names.len != 0) {
    memcpy(b + h.strings_off, t->names.p, t->names.len);
  }
  if (t->data.len != 0) {
    memcpy(b + h.data_off, t->data.p, t->data.len);
  }
  if (t->extra.len != 0) {
    memcpy(b + h.extra_off, t->extra.p, t->extra.len);
  }
  *out = image;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_image_take(GRHEAP_Heap * heap,
    const GRHEAP_Allocator * allocator, GRHEAP_Image ** out_image) {
  if (!grheap_usable(heap) || out_image == NULL || heap->busy != 0) {
    return GRHEAP_ERR_INVALID;
  }
  /* What a heap can hold that an image cannot carry: a pointer the heap does
   * not own and so cannot rewrite, a keep-alive the destination has no way to
   * recreate, and a slot that is not traced. Refused before anything is
   * walked. */
  if (heap->nroots != 0 || heap->handles != NULL || heap->stats.pins != 0 ||
      heap->nweak != 0) {
    return GRHEAP_ERR_INVALID;
  }
  for (size_t i = 0; i < heap->nspans; i++) {
    if (heap->spans[i]->npinned != 0) {
      return GRHEAP_ERR_INVALID;
    }
  }
  GRHEAP_Take t;
  memset(&t, 0, sizeof t);
  t.heap = heap;
  t.a = allocator != NULL ? allocator : grheap_allocator_default();

  GRCORE_RootVisitor visitor = {&t, take_root_slot, take_root_range};
  GRHEAP_Result r = GRHEAP_OK;
  if (grcore_context_enumerate_roots(heap->context, &visitor) != GRCORE_OK) {
    r = GRHEAP_ERR_INVALID;
  }
  if (r == GRHEAP_OK && t.range_seen) {
    take_fail(&t, GRHEAP_ERR_INVALID);
  }
  heap->busy++; /* a hook that allocates in the heap is refused */
  for (size_t i = 0; r == GRHEAP_OK && t.failure == GRHEAP_OK && i < t.nobjs; i++) {
    take_object(&t, i);
  }
  heap->busy--;
  if (r == GRHEAP_OK) {
    r = t.failure;
  }
  if (r == GRHEAP_OK) {
    r = take_assemble(&t, out_image);
  }
  take_free(&t);
  return r;
}

/* ---- reading an image --------------------------------------------------- */

static bool read_header(const unsigned char * bytes, size_t size, ImgHeader * h) {
  if (size < sizeof *h || ((uintptr_t)bytes & 7u) != 0) {
    return false;
  }
  memcpy(h, bytes, sizeof *h);
  if (h->magic != IMAGE_MAGIC || h->version != IMAGE_VERSION ||
      h->total != size) {
    return false;
  }
  /* Every section inside the block, in order, aligned. The counts are checked
   * against the room each section has, which also bounds every product. */
  uint64_t offsets[] = {h->types_off, h->objects_off, h->slots_off,
      h->roots_off, h->strings_off, h->data_off, h->extra_off, h->total};
  uint64_t floor = align8(sizeof *h);
  for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
    if (offsets[i] < floor || offsets[i] > h->total || (offsets[i] & 7u) != 0) {
      return false;
    }
    floor = offsets[i];
  }
  if (h->ntypes > (h->objects_off - h->types_off) / sizeof(ImgType) ||
      h->nobjects > (h->slots_off - h->objects_off) / sizeof(ImgObject) ||
      h->nslots > (h->roots_off - h->slots_off) / sizeof(ImgSlot) ||
      h->nroots > (h->strings_off - h->roots_off) / sizeof(ImgRoot) ||
      h->nobjects >= UINT32_MAX) {
    return false;
  }
  return true;
}

typedef struct View {
  const unsigned char * bytes;
  size_t size;
  ImgHeader h;
} View;

static GRHEAP_Result view_open(View * v, const void * bytes, size_t size) {
  v->bytes = bytes;
  v->size = size;
  return read_header(v->bytes, v->size, &v->h) ? GRHEAP_OK : GRHEAP_ERR_CORRUPT;
}

static ImgObject view_object(const View * v, size_t i) {
  ImgObject o;
  memcpy(&o, v->bytes + v->h.objects_off + i * sizeof o, sizeof o);
  return o;
}

static ImgSlot view_slot(const View * v, size_t i) {
  ImgSlot s;
  memcpy(&s, v->bytes + v->h.slots_off + i * sizeof s, sizeof s);
  return s;
}

static ImgRoot view_root(const View * v, size_t i) {
  ImgRoot r;
  memcpy(&r, v->bytes + v->h.roots_off + i * sizeof r, sizeof r);
  return r;
}

/* ---- instantiate: the pieces ---------------------------------------------- */

typedef struct GRHEAP_Pending GRHEAP_Pending;

struct GRHEAP_Pending {
  void ** addrs;        /* the objects made, in the image's order */
  size_t count;         /* how many are made so far */
  size_t nobjects;
  const unsigned char * roots; /* in the image, which outlives the restore */
  size_t nroots;
  bool ids;             /* IDs were adopted */
  bool prepared;
};

static void pending_destroy(GRHEAP_Heap * heap, GRHEAP_Pending * p) {
  if (p == NULL) {
    return;
  }
  heap->allocator->free_fn(heap->allocator->ctx, p->addrs);
  heap->allocator->free_fn(heap->allocator->ctx, p);
}

void grheap_pending_free(GRHEAP_Heap * heap) {
  pending_destroy(heap, heap->pending);
  heap->pending = NULL;
}

/* Frees exactly the objects a restore made. In a collected heap every other
 * object is marked and a sweep frees the rest, with its `destroy` hooks and its
 * shadow bookkeeping. An arena cannot free one object; what a failed restore
 * made stays until the arena is reset, unreachable. */
static void remove_objects(GRHEAP_Heap * heap, void ** addrs, size_t count) {
  if (heap->mode != GRHEAP_MODE_COLLECTED || count == 0) {
    return;
  }
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind != GRHEAP_SPAN_BLOCK) {
      continue;
    }
    size_t words = grheap_bit_words(s->bump);
    for (size_t w = 0; w < words; w++) {
      s->mark[w] = s->alloc[w];
    }
  }
  for (size_t i = 0; i < count; i++) {
    GRHEAP_Span * s;
    size_t index;
    if (grheap_locate(heap, (uintptr_t)addrs[i], true, &s, &index)) {
      grheap_bit_clear(s->mark, index);
    }
  }
  grheap_sweep(heap);
}

bool grheap_restore_env_valid(const GRHEAP_RestoreEnv * env) {
  /* See grheap_type_valid: judged by `size` alone. */
  return env != NULL && env->size >= GRHEAP_RESTORE_ENV_MIN_SIZE &&
      env->size % _Alignof(GRHEAP_RestoreEnv) == 0;
}

/* Checks the image against the heap, without changing anything but the
 * transient arrays it returns: the codec, the resolved types, every record's
 * bounds. `*out_types` is `ntypes` resolved types on success. */
static GRHEAP_Result validate(GRHEAP_Heap * heap, const View * v,
    const GRHEAP_RestoreEnv * env, bool whole_heap, const GRHEAP_Type *** out_types) {
  if (!grheap_usable(heap) || heap->busy != 0 ||
      !grheap_restore_env_valid(env) || env->resolve == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  const GRHEAP_ValueCodec * c = &heap->codec;
  if (v->h.tag_mask != c->tag_mask || v->h.tag_value != c->tag_value ||
      v->h.mask != c->mask || v->h.shift != c->shift || v->h.base != c->base) {
    return GRHEAP_ERR_INVALID; /* a different value encoding */
  }
  if (heap->nids != 0 || heap->next_id != 0 || heap->pending != NULL ||
      (whole_heap && heap->stats.live_objects != 0)) {
    return GRHEAP_ERR_INVALID; /* not empty */
  }
  uint64_t refusals = grcore_context_memory_refusals(heap->context);
  const GRHEAP_Type ** types = NULL;
  if (v->h.ntypes != 0) {
    types = heap->allocator->calloc_fn(
        heap->allocator->ctx, (size_t)v->h.ntypes, sizeof *types);
    if (types == NULL) {
      return grheap_failure(heap, refusals);
    }
  }
  GRHEAP_Result r = GRHEAP_OK;
  uint64_t strings_end = v->h.data_off;
  for (size_t i = 0; r == GRHEAP_OK && i < v->h.ntypes; i++) {
    ImgType rec;
    memcpy(&rec, v->bytes + v->h.types_off + i * sizeof rec, sizeof rec);
    if (rec.name_off < v->h.strings_off || rec.name_off >= strings_end ||
        rec.name_len >= strings_end - rec.name_off ||
        v->bytes[rec.name_off + rec.name_len] != 0) {
      r = GRHEAP_ERR_CORRUPT;
      break;
    }
    const GRHEAP_Type * type =
        env->resolve(env->user, (const char *)v->bytes + rec.name_off);
    if (!grheap_type_valid(type) || grheap_type_weak(type) != NULL) {
      r = GRHEAP_ERR_INVALID;
      break;
    }
    types[i] = type;
  }
  uint64_t data_size = v->h.extra_off - v->h.data_off;
  uint64_t extra_size = v->h.total - v->h.extra_off;
  for (size_t i = 0; r == GRHEAP_OK && i < v->h.nobjects; i++) {
    ImgObject o = view_object(v, i);
    if (o.type >= v->h.ntypes || o.size > data_size ||
        o.payload_off > data_size - o.size || o.slots_first > v->h.nslots ||
        o.nslots > v->h.nslots - o.slots_first || o.extra_off > extra_size ||
        o.extra_len > extra_size - o.extra_off || o.id > v->h.next_id ||
        o.size > SIZE_MAX / 4u) {
      r = GRHEAP_ERR_CORRUPT;
      break;
    }
    if (o.extra_len != 0 && grheap_type_restore(types[o.type]) == NULL) {
      r = GRHEAP_ERR_INVALID;
      break;
    }
    for (uint64_t k = 0; k < o.nslots; k++) {
      ImgSlot s = view_slot(v, (size_t)(o.slots_first + k));
      if (s.target >= v->h.nobjects || s.offset > o.size ||
          o.size - s.offset < 8u || s.kind > SLOT_WORD) {
        r = GRHEAP_ERR_CORRUPT;
        break;
      }
    }
  }
  for (size_t i = 0; r == GRHEAP_OK && i < v->h.nroots; i++) {
    ImgRoot root = view_root(v, i);
    if (root.target_plus1 > v->h.nobjects) {
      r = GRHEAP_ERR_CORRUPT;
    }
  }
  if (r != GRHEAP_OK) {
    heap->allocator->free_fn(heap->allocator->ctx, (void *)types);
    return r;
  }
  *out_types = types;
  return GRHEAP_OK;
}

/* Walks one restored payload in SHADOW mode so that barrier-verify finds every
 * traced slot stored, as the verify instrument requires of a heap object. */
static void shadow_object(GRHEAP_Heap * heap, void * payload) {
  const GRHEAP_Type * type = grheap_header(payload)->type;
  if (type->trace != NULL) {
    GRHEAP_Tracer tracer = {.heap = heap, .mode = GRHEAP_TRACE_SHADOW};
    type->trace(&tracer, payload);
  }
}

/* APPLY: makes the objects. On success the heap holds them, `*out_pending`
 * knows them, and the root slots are not yet written. */
static GRHEAP_Result apply(GRHEAP_Heap * heap, const View * v,
    const GRHEAP_RestoreEnv * env, bool whole_heap, GRHEAP_Pending ** out_pending) {
  const GRHEAP_Type ** types = NULL;
  GRHEAP_Result r = validate(heap, v, env, whole_heap, &types);
  if (r != GRHEAP_OK) {
    return r;
  }
  uint64_t refusals = grcore_context_memory_refusals(heap->context);
  const GRHEAP_Allocator * a = heap->allocator;
  GRHEAP_Pending * pending = a->calloc_fn(a->ctx, 1, sizeof *pending);
  if (pending == NULL) {
    a->free_fn(a->ctx, (void *)types);
    return grheap_failure(heap, refusals);
  }
  pending->nobjects = (size_t)v->h.nobjects;
  pending->roots = v->bytes + v->h.roots_off;
  pending->nroots = (size_t)v->h.nroots;
  if (pending->nobjects != 0) {
    pending->addrs = a->calloc_fn(a->ctx, pending->nobjects, sizeof(void *));
    if (pending->addrs == NULL) {
      pending_destroy(heap, pending);
      a->free_fn(a->ctx, (void *)types);
      return grheap_failure(heap, refusals);
    }
  }
  size_t id_count = 0;
  for (size_t i = 0; i < pending->nobjects; i++) {
    id_count += view_object(v, i).id != 0;
  }
  if (id_count != 0 && !grheap_ids_reserve_total(heap, id_count)) {
    r = grheap_failure(heap, refusals);
  }

  /* The cells. Nothing can collect between here and the end: the heap's own
   * allocation path, where a collection could run, is not used. */
  for (size_t i = 0; r == GRHEAP_OK && i < pending->nobjects; i++) {
    ImgObject o = view_object(v, i);
    size_t cell;
    if (!grheap_cell_size((size_t)o.size, &cell)) {
      r = GRHEAP_ERR_OOM;
      break;
    }
    void * cellp = heap->mode == GRHEAP_MODE_ARENA
        ? grheap_arena_take(heap, cell)
        : grheap_take_cell(heap, cell);
    if (cellp == NULL) {
      r = grheap_failure(heap, refusals);
      break;
    }
    GRHEAP_Header * header = cellp;
    header->type = types[o.type];
    header->size = o.size;
    void * payload = (unsigned char *)cellp + GRHEAP_HEADER_BYTES;
    memcpy(payload, v->bytes + v->h.data_off + o.payload_off, (size_t)o.size);
    pending->addrs[i] = payload;
    pending->count = i + 1;
    heap->stats.allocations++;
    heap->stats.live_objects++;
    size_t charged = cell;
    if (cell <= GRHEAP_SMALL_CELL_MAX) {
      (void)grheap_class_of(cell, &charged);
    }
    heap->stats.live_bytes += charged;
    heap->since_collect += charged;
  }

  /* The references, from indices to the new addresses. */
  for (size_t i = 0; r == GRHEAP_OK && i < pending->nobjects; i++) {
    ImgObject o = view_object(v, i);
    unsigned char * payload = pending->addrs[i];
    for (uint64_t k = 0; k < o.nslots; k++) {
      ImgSlot s = view_slot(v, (size_t)(o.slots_first + k));
      uint64_t address = (uint64_t)(uintptr_t)pending->addrs[s.target];
      uint64_t word;
      memcpy(&word, payload + s.offset, sizeof word);
      word = s.kind == SLOT_RAW ? address
                                : encode_word(&heap->codec, word, address);
      memcpy(payload + s.offset, &word, sizeof word);
    }
  }

  /* The types' own host pointers, by name. */
  heap->busy++;
  for (size_t i = 0; r == GRHEAP_OK && i < pending->nobjects; i++) {
    ImgObject o = view_object(v, i);
    if (grheap_type_restore(types[o.type]) == NULL) {
      continue;
    }
    GRHEAP_ImageReader reader = {v->bytes + v->h.extra_off + o.extra_off,
        (size_t)o.extra_len, 0, false};
    r = grheap_type_restore(types[o.type])(
        heap->context, pending->addrs[i], &reader, env->user);
  }
  heap->busy--;

  if (r == GRHEAP_OK && heap->verify != GRHEAP_VERIFY_OFF) {
    heap->busy++;
    for (size_t i = 0; i < pending->nobjects; i++) {
      shadow_object(heap, pending->addrs[i]);
    }
    heap->busy--;
  }
  a->free_fn(a->ctx, (void *)types);

  if (r == GRHEAP_OK && id_count != 0) {
    size_t n = 0;
    for (size_t i = 0; i < pending->nobjects; i++) {
      ImgObject o = view_object(v, i);
      if (o.id != 0) {
        heap->ids[n].id = o.id;
        heap->ids[n].object = pending->addrs[i];
        n++;
      }
    }
    grheap_ids_adopt(heap, n, v->h.next_id);
    pending->ids = true;
  } else if (r == GRHEAP_OK && v->h.next_id != 0) {
    /* Numbers were issued and nothing alive holds one: the next must still be
     * above them. */
    grheap_ids_adopt(heap, 0, v->h.next_id);
    pending->ids = true;
  }
  if (r != GRHEAP_OK) {
    remove_objects(heap, pending->addrs, pending->count);
    pending_destroy(heap, pending);
    return r;
  }
  *out_pending = pending;
  return GRHEAP_OK;
}

/* Counts the slots the context's sources report, and whether any range. */
typedef struct RootCount {
  size_t slots;
  bool range;
} RootCount;

static void count_slot(void * user, uint64_t * slot) {
  (void)slot;
  ((RootCount *)user)->slots++;
}

static void count_range(void * user, const GRCORE_ConservativeRange * range) {
  if (range->hi > range->lo) {
    ((RootCount *)user)->range = true;
  }
}

/* PREPARE: COMMIT cannot fail if the destination reports the layout the image
 * recorded. */
static GRHEAP_Result prepare(GRHEAP_Heap * heap, GRHEAP_Pending * p) {
  RootCount count = {0, false};
  GRCORE_RootVisitor visitor = {&count, count_slot, count_range};
  if (grcore_context_enumerate_roots(heap->context, &visitor) != GRCORE_OK ||
      count.range || count.slots != p->nroots) {
    return GRHEAP_ERR_INVALID;
  }
  p->prepared = true;
  return GRHEAP_OK;
}

typedef struct RootWrite {
  const GRHEAP_Heap * heap;
  const GRHEAP_Pending * p;
  size_t next;
} RootWrite;

static void write_slot(void * user, uint64_t * slot) {
  RootWrite * w = user;
  ImgRoot root;
  memcpy(&root, w->p->roots + w->next * sizeof root, sizeof root);
  w->next++;
  if (root.target_plus1 == 0) {
    *slot = root.word;
  } else {
    *slot = encode_word(&w->heap->codec, root.word,
        (uint64_t)(uintptr_t)w->p->addrs[root.target_plus1 - 1u]);
  }
}

static void commit(GRHEAP_Heap * heap, GRHEAP_Pending * p) {
  RootWrite w = {heap, p, 0};
  GRCORE_RootVisitor visitor = {&w, write_slot, NULL};
  (void)grcore_context_enumerate_roots(heap->context, &visitor);
}

static void abandon(GRHEAP_Heap * heap, GRHEAP_Pending * p) {
  remove_objects(heap, p->addrs, p->count);
  if (p->ids) {
    grheap_ids_unadopt(heap);
  }
}

GRHEAP_Result grheap_image_instantiate(GRHEAP_Heap * heap,
    const GRHEAP_Image * image, const GRHEAP_RestoreEnv * env) {
  View v;
  if (image == NULL || heap == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Result r = view_open(&v, image->bytes, image->size);
  if (r != GRHEAP_OK) {
    return r;
  }
  GRHEAP_Pending * pending = NULL;
  r = apply(heap, &v, env, true, &pending);
  if (r != GRHEAP_OK) {
    return r;
  }
  r = prepare(heap, pending);
  if (r != GRHEAP_OK) {
    abandon(heap, pending);
    pending_destroy(heap, pending);
    return r;
  }
  commit(heap, pending);
  pending_destroy(heap, pending);
  return GRHEAP_OK;
}

/* ---- the heap key's hooks ---------------------------------------------------- */

GRCORE_Result grheap_key_snapshot(
    GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * writer) {
  (void)context;
  GRHEAP_Image * image;
  GRHEAP_Result r = grheap_image_take(
      value, grcore_snapshot_writer_allocator(writer), &image);
  if (r != GRHEAP_OK) {
    return (GRCORE_Result)r;
  }
  GRCORE_Result w = grcore_snapshot_writer_write(writer, image->bytes, image->size);
  grheap_image_release(image);
  return w;
}

static GRCORE_Result open_blob(GRCORE_SnapshotReader * reader, View * v) {
  const void * bytes;
  size_t n = grcore_snapshot_reader_remaining(reader);
  GRCORE_Result r = grcore_snapshot_reader_view(reader, n, &bytes);
  if (r != GRCORE_OK) {
    return r;
  }
  return (GRCORE_Result)view_open(v, bytes, n);
}

GRCORE_Result grheap_key_restore(GRCORE_Context * context, void * value,
    GRCORE_SnapshotReader * reader, void * env, GRCORE_RestoreMode mode) {
  (void)context;
  GRHEAP_Heap * heap = value;
  View v;
  GRCORE_Result r = open_blob(reader, &v);
  if (r != GRCORE_OK) {
    return r;
  }
  if (mode == GRCORE_RESTORE_CHECK) {
    const GRHEAP_Type ** types = NULL;
    GRHEAP_Result c = validate(heap, &v, env, false, &types);
    if (c == GRHEAP_OK) {
      heap->allocator->free_fn(heap->allocator->ctx, (void *)types);
    }
    return (GRCORE_Result)c;
  }
  GRHEAP_Pending * pending = NULL;
  GRHEAP_Result a = apply(heap, &v, env, false, &pending);
  if (a == GRHEAP_OK) {
    heap->pending = pending;
  }
  return (GRCORE_Result)a;
}

GRCORE_Result grheap_key_settle(GRCORE_Context * context, void * value,
    void * env, GRCORE_SettleMode mode) {
  (void)context;
  (void)env;
  GRHEAP_Heap * heap = value;
  GRHEAP_Pending * p = heap->pending;
  if (p == NULL) {
    return mode == GRCORE_SETTLE_PREPARE ? GRCORE_ERR_INTERNAL : GRCORE_OK;
  }
  switch (mode) {
    case GRCORE_SETTLE_PREPARE:
      return (GRCORE_Result)prepare(heap, p);
    case GRCORE_SETTLE_COMMIT:
      commit(heap, p);
      break;
    case GRCORE_SETTLE_ABANDON:
      abandon(heap, p);
      break;
  }
  grheap_pending_free(heap);
  return GRCORE_OK;
}
