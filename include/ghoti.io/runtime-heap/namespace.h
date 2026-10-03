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
 * @file namespace.h
 * @stability stable
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GRHEAP_NAMESPACE_H
#define GHOTI_IO_GRHEAP_NAMESPACE_H

#include <ghoti.io/runtime-heap/libver.h>

/// @cond HIDDEN_SYMBOLS

/* Public types, and the private ones an internal header may name through a
 * struct tag. GCU_* names are cutil's, and GRCORE_* names are runtime-core's;
 * each has already renamed them. */
#define GRHEAP_Allocator GHOTIIO_RUNTIME_HEAP(GRHEAP_Allocator)
#define GRHEAP_DestroyFn GHOTIIO_RUNTIME_HEAP(GRHEAP_DestroyFn)
#define GRHEAP_Handle GHOTIIO_RUNTIME_HEAP(GRHEAP_Handle)
#define GRHEAP_Header GHOTIIO_RUNTIME_HEAP(GRHEAP_Header)
#define GRHEAP_Heap GHOTIIO_RUNTIME_HEAP(GRHEAP_Heap)
#define GRHEAP_IdEntry GHOTIIO_RUNTIME_HEAP(GRHEAP_IdEntry)
#define GRHEAP_Mode GHOTIIO_RUNTIME_HEAP(GRHEAP_Mode)
#define GRHEAP_Options GHOTIIO_RUNTIME_HEAP(GRHEAP_Options)
#define GRHEAP_Result GHOTIIO_RUNTIME_HEAP(GRHEAP_Result)
#define GRHEAP_ShadowEntry GHOTIIO_RUNTIME_HEAP(GRHEAP_ShadowEntry)
#define GRHEAP_Span GHOTIIO_RUNTIME_HEAP(GRHEAP_Span)
#define GRHEAP_SpanKind GHOTIIO_RUNTIME_HEAP(GRHEAP_SpanKind)
#define GRHEAP_Stats GHOTIIO_RUNTIME_HEAP(GRHEAP_Stats)
#define GRHEAP_TraceFn GHOTIIO_RUNTIME_HEAP(GRHEAP_TraceFn)
#define GRHEAP_TraceMode GHOTIIO_RUNTIME_HEAP(GRHEAP_TraceMode)
#define GRHEAP_Tracer GHOTIIO_RUNTIME_HEAP(GRHEAP_Tracer)
#define GRHEAP_Type GHOTIIO_RUNTIME_HEAP(GRHEAP_Type)
#define GRHEAP_ValueCodec GHOTIIO_RUNTIME_HEAP(GRHEAP_ValueCodec)
#define GRHEAP_Verify GHOTIIO_RUNTIME_HEAP(GRHEAP_Verify)
#define GRHEAP_WeakEntry GHOTIIO_RUNTIME_HEAP(GRHEAP_WeakEntry)
#define GRHEAP_WeakKind GHOTIIO_RUNTIME_HEAP(GRHEAP_WeakKind)
#define GRHEAP_WeakSpec GHOTIIO_RUNTIME_HEAP(GRHEAP_WeakSpec)

/* Functions and data, public and private. */
#define grheap_alloc GHOTIIO_RUNTIME_HEAP(grheap_alloc)
#define grheap_alloc_sized GHOTIIO_RUNTIME_HEAP(grheap_alloc_sized)
#define grheap_allocator_default GHOTIIO_RUNTIME_HEAP(grheap_allocator_default)
#define grheap_arena_reset GHOTIIO_RUNTIME_HEAP(grheap_arena_reset)
#define grheap_arena_take GHOTIIO_RUNTIME_HEAP(grheap_arena_take)
#define grheap_cell_size GHOTIIO_RUNTIME_HEAP(grheap_cell_size)
#define grheap_class_of GHOTIIO_RUNTIME_HEAP(grheap_class_of)
#define grheap_collect GHOTIIO_RUNTIME_HEAP(grheap_collect)
#define grheap_collect_now GHOTIIO_RUNTIME_HEAP(grheap_collect_now)
#define grheap_contains GHOTIIO_RUNTIME_HEAP(grheap_contains)
#define grheap_decode GHOTIIO_RUNTIME_HEAP(grheap_decode)
#define grheap_each_object GHOTIIO_RUNTIME_HEAP(grheap_each_object)
#define grheap_env_torture GHOTIIO_RUNTIME_HEAP(grheap_env_torture)
#define grheap_env_verify GHOTIIO_RUNTIME_HEAP(grheap_env_verify)
#define grheap_failure GHOTIIO_RUNTIME_HEAP(grheap_failure)
#define grheap_from_core GHOTIIO_RUNTIME_HEAP(grheap_from_core)
#define grheap_handle_create GHOTIIO_RUNTIME_HEAP(grheap_handle_create)
#define grheap_handle_destroy GHOTIIO_RUNTIME_HEAP(grheap_handle_destroy)
#define grheap_handle_get GHOTIIO_RUNTIME_HEAP(grheap_handle_get)
#define grheap_handle_set GHOTIIO_RUNTIME_HEAP(grheap_handle_set)
#define grheap_heap_create GHOTIIO_RUNTIME_HEAP(grheap_heap_create)
#define grheap_heap_get GHOTIIO_RUNTIME_HEAP(grheap_heap_get)
#define grheap_id_of GHOTIIO_RUNTIME_HEAP(grheap_id_of)
#define grheap_identity_hash GHOTIIO_RUNTIME_HEAP(grheap_identity_hash)
#define grheap_ids_clear GHOTIIO_RUNTIME_HEAP(grheap_ids_clear)
#define grheap_ids_drop_dead GHOTIIO_RUNTIME_HEAP(grheap_ids_drop_dead)
#define grheap_ids_free GHOTIIO_RUNTIME_HEAP(grheap_ids_free)
#define grheap_key GHOTIIO_RUNTIME_HEAP(grheap_key)
#define grheap_locate GHOTIIO_RUNTIME_HEAP(grheap_locate)
#define grheap_mark GHOTIIO_RUNTIME_HEAP(grheap_mark)
#define grheap_mark_extra GHOTIIO_RUNTIME_HEAP(grheap_mark_extra)
#define grheap_mark_pinned GHOTIIO_RUNTIME_HEAP(grheap_mark_pinned)
#define grheap_mark_stack_free GHOTIIO_RUNTIME_HEAP(grheap_mark_stack_free)
#define grheap_object_containing GHOTIIO_RUNTIME_HEAP(grheap_object_containing)
#define grheap_object_of_id GHOTIIO_RUNTIME_HEAP(grheap_object_of_id)
#define grheap_options_create GHOTIIO_RUNTIME_HEAP(grheap_options_create)
#define grheap_options_destroy GHOTIIO_RUNTIME_HEAP(grheap_options_destroy)
#define grheap_options_get_auto_collect GHOTIIO_RUNTIME_HEAP(grheap_options_get_auto_collect)
#define grheap_options_get_gc_threshold GHOTIIO_RUNTIME_HEAP(grheap_options_get_gc_threshold)
#define grheap_options_get_mode GHOTIIO_RUNTIME_HEAP(grheap_options_get_mode)
#define grheap_options_get_torture GHOTIIO_RUNTIME_HEAP(grheap_options_get_torture)
#define grheap_options_get_value_codec GHOTIIO_RUNTIME_HEAP(grheap_options_get_value_codec)
#define grheap_options_get_verify GHOTIIO_RUNTIME_HEAP(grheap_options_get_verify)
#define grheap_options_set_auto_collect GHOTIIO_RUNTIME_HEAP(grheap_options_set_auto_collect)
#define grheap_options_set_gc_threshold GHOTIIO_RUNTIME_HEAP(grheap_options_set_gc_threshold)
#define grheap_options_set_mode GHOTIIO_RUNTIME_HEAP(grheap_options_set_mode)
#define grheap_options_set_torture GHOTIIO_RUNTIME_HEAP(grheap_options_set_torture)
#define grheap_options_set_value_codec GHOTIIO_RUNTIME_HEAP(grheap_options_set_value_codec)
#define grheap_options_set_verify GHOTIIO_RUNTIME_HEAP(grheap_options_set_verify)
#define grheap_pin GHOTIIO_RUNTIME_HEAP(grheap_pin)
#define grheap_release_all_spans GHOTIIO_RUNTIME_HEAP(grheap_release_all_spans)
#define grheap_result_string GHOTIIO_RUNTIME_HEAP(grheap_result_string)
#define grheap_root_add GHOTIIO_RUNTIME_HEAP(grheap_root_add)
#define grheap_root_remove GHOTIIO_RUNTIME_HEAP(grheap_root_remove)
#define grheap_roots_clear_for_reset GHOTIIO_RUNTIME_HEAP(grheap_roots_clear_for_reset)
#define grheap_roots_free GHOTIIO_RUNTIME_HEAP(grheap_roots_free)
#define grheap_run_all_destroys GHOTIIO_RUNTIME_HEAP(grheap_run_all_destroys)
#define grheap_shadow_clear GHOTIIO_RUNTIME_HEAP(grheap_shadow_clear)
#define grheap_shadow_forget_range GHOTIIO_RUNTIME_HEAP(grheap_shadow_forget_range)
#define grheap_shadow_free GHOTIIO_RUNTIME_HEAP(grheap_shadow_free)
#define grheap_shadow_set GHOTIIO_RUNTIME_HEAP(grheap_shadow_set)
#define grheap_size_of GHOTIIO_RUNTIME_HEAP(grheap_size_of)
#define grheap_span_create GHOTIIO_RUNTIME_HEAP(grheap_span_create)
#define grheap_span_find GHOTIIO_RUNTIME_HEAP(grheap_span_find)
#define grheap_span_free GHOTIIO_RUNTIME_HEAP(grheap_span_free)
#define grheap_stats GHOTIIO_RUNTIME_HEAP(grheap_stats)
#define grheap_store GHOTIIO_RUNTIME_HEAP(grheap_store)
#define grheap_store_range GHOTIIO_RUNTIME_HEAP(grheap_store_range)
#define grheap_store_word GHOTIIO_RUNTIME_HEAP(grheap_store_word)
#define grheap_sweep GHOTIIO_RUNTIME_HEAP(grheap_sweep)
#define grheap_take_cell GHOTIIO_RUNTIME_HEAP(grheap_take_cell)
#define grheap_trace_slot GHOTIIO_RUNTIME_HEAP(grheap_trace_slot)
#define grheap_trace_word GHOTIIO_RUNTIME_HEAP(grheap_trace_word)
#define grheap_type_of GHOTIIO_RUNTIME_HEAP(grheap_type_of)
#define grheap_unpin GHOTIIO_RUNTIME_HEAP(grheap_unpin)
#define grheap_value_codec_default GHOTIIO_RUNTIME_HEAP(grheap_value_codec_default)
#define grheap_verify GHOTIIO_RUNTIME_HEAP(grheap_verify)
#define grheap_verify_slot GHOTIIO_RUNTIME_HEAP(grheap_verify_slot)
#define grheap_verify_store GHOTIIO_RUNTIME_HEAP(grheap_verify_store)
#define grheap_verify_walk GHOTIIO_RUNTIME_HEAP(grheap_verify_walk)
#define grheap_version_number GHOTIIO_RUNTIME_HEAP(grheap_version_number)
#define grheap_version_string GHOTIIO_RUNTIME_HEAP(grheap_version_string)
#define grheap_weak_each_slot GHOTIIO_RUNTIME_HEAP(grheap_weak_each_slot)
#define grheap_weak_free GHOTIIO_RUNTIME_HEAP(grheap_weak_free)
#define grheap_weak_process GHOTIIO_RUNTIME_HEAP(grheap_weak_process)
#define grheap_weak_register GHOTIIO_RUNTIME_HEAP(grheap_weak_register)
#define grheap_weak_reserve GHOTIIO_RUNTIME_HEAP(grheap_weak_reserve)
#define grheap_weak_run_cleared GHOTIIO_RUNTIME_HEAP(grheap_weak_run_cleared)
#define grheap_weak_spec_fits GHOTIIO_RUNTIME_HEAP(grheap_weak_spec_fits)

/// @endcond

#endif /* GHOTI_IO_GRHEAP_NAMESPACE_H */
