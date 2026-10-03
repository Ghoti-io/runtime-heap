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
 * @file core.h
 * @stability stable
 *
 * Result codes and the version this build reports.
 */

#ifndef GHOTI_IO_GRHEAP_CORE_H
#define GHOTI_IO_GRHEAP_CORE_H

#include <ghoti.io/runtime-heap/allocator.h>
#include <ghoti.io/runtime-heap/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of an operation.
 *
 * Zero is success.  ::GRHEAP_RESULT_COUNT closes the enum so a test can check
 * the string table is complete.
 *
 * This is the suite's fixed vocabulary, numbered as in every other library
 * and as in runtime-core's ::GRCORE_Result up to ::GRHEAP_ERR_INTERNAL. It has
 * no ERR_GUEST: a collector has no guest, and an allocation that ran out of
 * the context's memory budget is ::GRHEAP_ERR_LIMIT, which is how the budget's
 * refusal reaches a caller (as opposed to ::GRHEAP_ERR_OOM, where the
 * allocator or the page provider itself failed).
 */
typedef enum {
  GRHEAP_OK = 0,          ///< The operation succeeded.
  GRHEAP_ERR_IO,          ///< A read, write, or seek failed.
  GRHEAP_ERR_FORMAT,      ///< Well-formed bytes, but not a format handled here.
  GRHEAP_ERR_UNSUPPORTED, ///< The format is known and the feature is not
                          ///< implemented.
  GRHEAP_ERR_LIMIT,       ///< A stated cap was exceeded.
  GRHEAP_ERR_CORRUPT,     ///< The bytes are not a valid encoding of this
                          ///< format.
  GRHEAP_ERR_OOM,         ///< The allocator returned NULL.
  GRHEAP_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GRHEAP_ERR_INTERNAL,    ///< The library's own invariant failed.
  GRHEAP_RESULT_COUNT
} GRHEAP_Result;

/**
 * @brief Static description of a result code.
 *
 * The string is never NULL, never allocated, and never contains caller data.
 *
 * @param result The result code, including values outside the enum.
 * @return A static string.
 */
GRHEAP_API const char * grheap_result_string(GRHEAP_Result result);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * @return A static string, never NULL.  "0.0.0", "0.0.0-dev" when BRANCH
 *   was overridden, and with "-debug" appended for a BUILD=debug build
 *   ("0.0.0-debug", "0.0.0-dev-debug").
 */
GRHEAP_API const char * grheap_version_string(void);

/**
 * @brief This build's version, packed as ::GRHEAP_MAKE_VERSION packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GRHEAP_API unsigned grheap_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_CORE_H */
