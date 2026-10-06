/*
 * A planted-defect fixture for tools/check-relocation-gates.sh.
 *
 * An engine object keeps a reference to another heap object in a field. The
 * target is held alive by a root of its own.
 *
 *   built plain                      the field is `slot`, which `trace` reports
 *   built -DPLANT_UNTRACED_FIELD     the reference is kept in `hidden`, which
 *                                    `trace` leaves out: the collector never
 *                                    sees it, so it is never rewritten
 *
 * What the gate requires of the planted build: it exits non-zero under
 * GRHEAP_RELOCATE=1 and says "poisoned read"; it exits zero without relocation
 * (an unmoving collector cannot tell the field is missing from `trace`); the
 * plain build exits zero under relocation.
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdint.h>
#include <stdio.h>

#define MAGIC UINT64_C(0x1122334455667788)
#define POISON UINT64_C(0xDDDDDDDDDDDDDDDD)

typedef struct Engine {
  void * slot;
  void * hidden;
} Engine;

static void engine_trace(GRHEAP_Tracer * tracer, void * payload) {
  grheap_trace_slot(tracer, &((Engine *)payload)->slot);
}

static const GRHEAP_Type engine_type =
    GRHEAP_TYPE_INIT("engine", sizeof(Engine), engine_trace, NULL, NULL, NULL, NULL);
static const GRHEAP_Type blob_type =
    GRHEAP_TYPE_INIT("blob", 32, NULL, NULL, NULL, NULL, NULL);

static void * target_root;
static void * engine_root;

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
  CHECK(grheap_heap_create(context, NULL, &heap) == GRHEAP_OK);

  void * keeper;
  CHECK(grheap_alloc(heap, &blob_type, &keeper) == GRHEAP_OK);
  CHECK(grheap_pin(heap, keeper) == GRHEAP_OK);

  void * engine_raw;
  CHECK(grheap_alloc(heap, &engine_type, &engine_raw) == GRHEAP_OK);
  engine_root = engine_raw;
  CHECK(grheap_root_add(heap, &engine_root) == GRHEAP_OK);
  void * target_raw;
  CHECK(grheap_alloc_sized(heap, &blob_type, 32, &target_raw) == GRHEAP_OK);
  ((volatile uint64_t *)target_raw)[0] = MAGIC;
  target_root = target_raw;
  CHECK(grheap_root_add(heap, &target_root) == GRHEAP_OK);
#ifdef PLANT_UNTRACED_FIELD
  ((Engine *)engine_root)->hidden = target_root;
#else
  ((Engine *)engine_root)->slot = target_root;
#endif

  CHECK(grheap_collect(heap) == GRHEAP_OK);

#ifdef PLANT_UNTRACED_FIELD
  volatile uint64_t * seen_at = ((Engine *)engine_root)->hidden;
#else
  volatile uint64_t * seen_at = ((Engine *)engine_root)->slot;
#endif
  uint64_t seen = seen_at[0];
  if (seen == POISON) {
    fprintf(stderr, "poisoned read: a field `trace` leaves out went stale\n");
    return 1;
  }
  if (seen != MAGIC) {
    fprintf(stderr, "stale read: %llx (not the poison pattern)\n",
        (unsigned long long)seen);
    return 2;
  }
  printf("stale_untraced: the field followed its object\n");

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
