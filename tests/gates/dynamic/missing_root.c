/*
 * A planted-defect fixture for tools/check-dynamic-gates.sh.
 *
 * It holds an object in a plain C variable across an allocation. That is the
 * mistake GC torture exists to catch: the object is in no root, so a
 * collection at the allocation frees it, and the freed cell is poisoned.
 *
 *   built plain                 the root is registered; the read sees the data
 *   built -DPLANT_MISSING_ROOT  the root is omitted
 *
 * What the gate requires of the planted build: it exits non-zero under
 * GRHEAP_TORTURE=1 and says "poisoned read"; it exits zero without torture,
 * which is the proof that torture, and not the program, is what finds it. The
 * plain build exits zero under torture.
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdint.h>
#include <stdio.h>

#define MAGIC UINT64_C(0x1122334455667788)
#define POISON UINT64_C(0xDDDDDDDDDDDDDDDD)

static const GRHEAP_Type blob_type = {"blob", 64, NULL, NULL, NULL};

#ifndef PLANT_MISSING_ROOT
/* A registered root slot: the way a C variable keeps an object. */
static void * root_slot;
#endif

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "fixture failed: %s (line %d)\n", #condition, __LINE__); \
      return 3;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
  /* No options: the environment (GRHEAP_TORTURE) decides. */
  CHECK(grheap_heap_create(context, NULL, &heap) == GRHEAP_OK);

  /* A survivor of the victim's size class, so that the victim's cell stays
   * inside a mapped block when it is freed (ASan could never see it there). */
  void * keeper;
  GRHEAP_Handle * handle;
  CHECK(grheap_alloc(heap, &blob_type, &keeper) == GRHEAP_OK);
  CHECK(grheap_handle_create(heap, keeper, &handle) == GRHEAP_OK);

  volatile uint64_t * victim;
  void * raw;
  CHECK(grheap_alloc(heap, &blob_type, &raw) == GRHEAP_OK);
  victim = raw;
  victim[0] = MAGIC;

#ifndef PLANT_MISSING_ROOT
  root_slot = raw;
  CHECK(grheap_root_add(heap, &root_slot) == GRHEAP_OK);
#endif

  /* A GC point, of another size class so the victim's cell is not reused. */
  void * other;
  CHECK(grheap_alloc_sized(heap, &blob_type, 400, &other) == GRHEAP_OK);

  uint64_t seen = victim[0];
  if (seen == POISON) {
    fprintf(stderr, "poisoned read: the object was freed under a live pointer\n");
    return 1;
  }
  if (seen != MAGIC) {
    fprintf(stderr, "stale read: %llx (not the poison pattern)\n",
        (unsigned long long)seen);
    return 2;
  }
  printf("missing_root: the object survived\n");

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
