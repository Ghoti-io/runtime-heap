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
 * @file runtime-heap.h
 * @stability stable
 *
 * Umbrella header for Ghoti.io Runtime-heap.
 *
 * The collector of the language runtime stack: a precise, non-moving,
 * stop-the-world mark and sweep heap that attaches to a `runtime-core`
 * context (AD-3, AD-11). It works for a C embedder with no engine. It
 * includes `runtime-core`'s B headers (the context, the key, the page
 * provider and the root sources) and none of A's (AD-2): this header, and
 * every header it includes, is checked for that by `make check-edges`.
 */

#ifndef GHOTI_IO_GRHEAP_RUNTIME_HEAP_H
#define GHOTI_IO_GRHEAP_RUNTIME_HEAP_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/allocator.h>
#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/libver.h>

#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/id.h>
#include <ghoti.io/runtime-heap/options.h>
#include <ghoti.io/runtime-heap/roots.h>
#include <ghoti.io/runtime-heap/store.h>
#include <ghoti.io/runtime-heap/type.h>

#endif /* GHOTI_IO_GRHEAP_RUNTIME_HEAP_H */
