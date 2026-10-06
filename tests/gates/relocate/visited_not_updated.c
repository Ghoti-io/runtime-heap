/*
 * A planted-defect fixture for tools/check-relocation-gates.sh.
 *
 * An object holds a reference to another in a slot that its `trace` function
 * reports, and the target is kept alive by a root. Under relocation torture the
 * target moves at the next collection, and the slot must move with it.
 *
 *   built plain                          trace reports the slot itself
 *   built -DPLANT_VISITED_NOT_UPDATED    trace reports a copy of the slot: the
 *                                        collector is shown the reference and
 *                                        keeps its target, but the slot it can
 *                                        write is not the slot the object holds
 *
 * What the gate requires of the planted build: it exits non-zero under
 * GRHEAP_RELOCATE=1 and says "poisoned read"; it exits zero without relocation,
 * which is the proof that relocation, and not the program, is what finds it.
 * The plain build exits zero under relocation.
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdint.h>
#include <stdio.h>

#define MAGIC UINT64_C(0x1122334455667788)
#define POISON UINT64_C(0xDDDDDDDDDDDDDDDD)

typedef struct Holder {
  void * slot;
  uint64_t pad;
} Holder;

static void holder_trace(GRHEAP_Tracer * tracer, void * payload) {
  Holder * h = payload;
#ifdef PLANT_VISITED_NOT_UPDATED
  void * copy = h->slot;
  grheap_trace_slot(tracer, &copy);
#else
  grheap_trace_slot(tracer, &h->slot);
#endif
}

static const GRHEAP_Type holder_type =
    GRHEAP_TYPE_INIT("holder", sizeof(Holder), holder_trace, NULL, NULL, NULL, NULL);
static const GRHEAP_Type blob_type =
    GRHEAP_TYPE_INIT("blob", 32, NULL, NULL, NULL, NULL, NULL);

static void * target_root;
static void * holder_root;

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
  /* No options: the environment (GRHEAP_RELOCATE) decides. */
  CHECK(grheap_heap_create(context, NULL, &heap) == GRHEAP_OK);

  /* A pinned survivor of the cells' size class, so that the block stays mapped
   * after the objects leave it and a stale read is a read of poison. */
  void * keeper;
  CHECK(grheap_alloc(heap, &blob_type, &keeper) == GRHEAP_OK);
  CHECK(grheap_pin(heap, keeper) == GRHEAP_OK);

  void * holder_raw;
  CHECK(grheap_alloc(heap, &holder_type, &holder_raw) == GRHEAP_OK);
  holder_root = holder_raw;
  CHECK(grheap_root_add(heap, &holder_root) == GRHEAP_OK);
  void * target_raw;
  CHECK(grheap_alloc_sized(heap, &blob_type, 32, &target_raw) == GRHEAP_OK);
  ((volatile uint64_t *)target_raw)[0] = MAGIC;
  target_root = target_raw;
  CHECK(grheap_root_add(heap, &target_root) == GRHEAP_OK);
  ((Holder *)holder_root)->slot = target_root;

  CHECK(grheap_collect(heap) == GRHEAP_OK);

  /* Read the target through the holder's own slot. */
  volatile uint64_t * seen_at = ((Holder *)holder_root)->slot;
  uint64_t seen = seen_at[0];
  if (seen == POISON) {
    fprintf(stderr, "poisoned read: a slot was visited and not updated\n");
    return 1;
  }
  if (seen != MAGIC) {
    fprintf(stderr, "stale read: %llx (not the poison pattern)\n",
        (unsigned long long)seen);
    return 2;
  }
  printf("visited_not_updated: the slot followed its object\n");

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
