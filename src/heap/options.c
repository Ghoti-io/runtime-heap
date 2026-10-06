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
 * Heap options: an opaque object and its setters.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <stdlib.h>
#include <string.h>

bool grheap_env_torture(void) {
  const char * v = getenv("GRHEAP_TORTURE");
  return v != NULL && strcmp(v, "1") == 0;
}

#ifdef GRHEAP_RELOCATE
bool grheap_env_relocate(void) {
  const char * v = getenv("GRHEAP_RELOCATE");
  return v != NULL && strcmp(v, "1") == 0;
}
#endif

bool grheap_relocation_available(void) {
#ifdef GRHEAP_RELOCATE
  return true;
#else
  return false;
#endif
}

GRHEAP_Verify grheap_env_verify(void) {
  const char * v = getenv("GRHEAP_VERIFY");
  if (v == NULL) {
    return GRHEAP_VERIFY_OFF;
  }
  if (strcmp(v, "1") == 0) {
    return GRHEAP_VERIFY_ABORT;
  }
  if (strcmp(v, "count") == 0) {
    return GRHEAP_VERIFY_COUNT;
  }
  return GRHEAP_VERIFY_OFF;
}

void grheap_value_codec_default(GRHEAP_ValueCodec * codec) {
  if (codec == NULL) {
    return;
  }
  codec->tag_mask = 0;
  codec->tag_value = 0;
  codec->mask = UINT64_MAX;
  codec->shift = 0;
  codec->base = 0;
}

GRHEAP_Result grheap_options_create(
    const GRHEAP_Allocator * allocator, GRHEAP_Options ** out_options) {
  if (out_options == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  if (allocator == NULL) {
    allocator = grheap_allocator_default();
  }
  GRHEAP_Options * o = allocator->calloc_fn(allocator->ctx, 1, sizeof *o);
  if (o == NULL) {
    return GRHEAP_ERR_OOM;
  }
  o->allocator = allocator;
  o->mode = GRHEAP_MODE_COLLECTED;
  grheap_value_codec_default(&o->codec);
  o->threshold = GRHEAP_DEFAULT_GC_THRESHOLD;
  o->auto_collect = true;
  o->torture = grheap_env_torture();
  o->verify = grheap_env_verify();
#ifdef GRHEAP_RELOCATE
  o->relocate = grheap_env_relocate();
#endif
  *out_options = o;
  return GRHEAP_OK;
}

void grheap_options_destroy(GRHEAP_Options * options) {
  if (options != NULL) {
    options->allocator->free_fn(options->allocator->ctx, options);
  }
}

GRHEAP_Result grheap_options_set_mode(
    GRHEAP_Options * options, GRHEAP_Mode mode) {
  if (options == NULL ||
      (mode != GRHEAP_MODE_COLLECTED && mode != GRHEAP_MODE_ARENA)) {
    return GRHEAP_ERR_INVALID;
  }
  options->mode = mode;
  return GRHEAP_OK;
}

GRHEAP_Mode grheap_options_get_mode(const GRHEAP_Options * options) {
  return options == NULL ? GRHEAP_MODE_COLLECTED : options->mode;
}

GRHEAP_Result grheap_options_set_value_codec(
    GRHEAP_Options * options, const GRHEAP_ValueCodec * codec) {
  if (options == NULL || codec == NULL || codec->shift >= 64u ||
      (codec->tag_value & ~codec->tag_mask) != 0) {
    return GRHEAP_ERR_INVALID;
  }
  options->codec = *codec;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_options_get_value_codec(
    const GRHEAP_Options * options, GRHEAP_ValueCodec * out_codec) {
  if (options == NULL || out_codec == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  *out_codec = options->codec;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_options_set_gc_threshold(
    GRHEAP_Options * options, uint64_t bytes) {
  if (options == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  options->threshold = bytes;
  return GRHEAP_OK;
}

uint64_t grheap_options_get_gc_threshold(const GRHEAP_Options * options) {
  return options == NULL ? GRHEAP_DEFAULT_GC_THRESHOLD : options->threshold;
}

GRHEAP_Result grheap_options_set_auto_collect(
    GRHEAP_Options * options, bool enabled) {
  if (options == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  options->auto_collect = enabled;
  return GRHEAP_OK;
}

bool grheap_options_get_auto_collect(const GRHEAP_Options * options) {
  return options == NULL ? true : options->auto_collect;
}

GRHEAP_Result grheap_options_set_torture(
    GRHEAP_Options * options, bool enabled) {
  if (options == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  options->torture = enabled;
  return GRHEAP_OK;
}

bool grheap_options_get_torture(const GRHEAP_Options * options) {
  return options == NULL ? false : options->torture;
}

GRHEAP_Result grheap_options_set_verify(
    GRHEAP_Options * options, GRHEAP_Verify mode) {
  if (options == NULL ||
      (mode != GRHEAP_VERIFY_OFF && mode != GRHEAP_VERIFY_COUNT &&
          mode != GRHEAP_VERIFY_ABORT)) {
    return GRHEAP_ERR_INVALID;
  }
  options->verify = mode;
  return GRHEAP_OK;
}

GRHEAP_Verify grheap_options_get_verify(const GRHEAP_Options * options) {
  return options == NULL ? GRHEAP_VERIFY_OFF : options->verify;
}

#ifdef GRHEAP_RELOCATE
GRHEAP_Result grheap_options_set_relocate(
    GRHEAP_Options * options, bool enabled) {
  if (options == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  options->relocate = enabled;
  return GRHEAP_OK;
}

bool grheap_options_get_relocate(const GRHEAP_Options * options) {
  return options == NULL ? false : options->relocate;
}
#endif
