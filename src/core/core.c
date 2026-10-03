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
 * Result strings.
 *
 * The string table is indexed by the enum. A new result that is not added
 * here falls through to "Unknown error", and the unit test that walks
 * GRHEAP_RESULT_COUNT rejects that.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>

const char * grheap_result_string(GRHEAP_Result result) {
  switch (result) {
    case GRHEAP_OK:
      return "No error";
    case GRHEAP_ERR_IO:
      return "Input/output error";
    case GRHEAP_ERR_FORMAT:
      return "Format not recognised";
    case GRHEAP_ERR_UNSUPPORTED:
      return "Unsupported feature";
    case GRHEAP_ERR_LIMIT:
      return "Limit exceeded";
    case GRHEAP_ERR_CORRUPT:
      return "Corrupt input";
    case GRHEAP_ERR_OOM:
      return "Out of memory";
    case GRHEAP_ERR_INVALID:
      return "Invalid argument";
    case GRHEAP_ERR_INTERNAL:
      return "Internal error";
    case GRHEAP_RESULT_COUNT:
      break;
  }
  return "Unknown error";
}
