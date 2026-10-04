/*
 * A planted-defect fixture for tools/check-dynamic-gates.sh.
 *
 * It writes a pointer into a traced slot of a heap object. The only sanctioned
 * way is grheap_store; a write that bypasses it is invisible today (this mode
 * has no barrier) and fatal to a mode that has one. Barrier-verify is what
 * finds it.
 *
 *   built plain                   the pointer is stored with grheap_store
 *   built -DPLANT_MISSING_STORE   the pointer is written directly
 *
 * What the gate requires of the planted build: under GRHEAP_VERIFY=1 it dies
 * with a "barrier-verify" report; under GRHEAP_VERIFY=count it prints at least
 * one violation and exits non-zero; with verify off it exits zero. The plain
 * build reports zero violations and exits zero in every mode.
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdint.h>
#include <stdio.h>

typedef struct Pair {
  void * first;
  void * second;
} Pair;

static void pair_trace(GRHEAP_Tracer * tracer, void * payload) {
  Pair * pair = payload;
  grheap_trace_slot(tracer, &pair->first);
  grheap_trace_slot(tracer, &pair->second);
}

static const GRHEAP_Type pair_type = {
    "pair", sizeof(Pair), pair_trace, NULL, NULL, NULL, NULL};

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
  /* No options: the environment (GRHEAP_VERIFY) decides. */
  CHECK(grheap_heap_create(context, NULL, &heap) == GRHEAP_OK);

  void * a;
  void * b;
  GRHEAP_Handle * ha;
  GRHEAP_Handle * hb;
  CHECK(grheap_alloc(heap, &pair_type, &a) == GRHEAP_OK);
  CHECK(grheap_handle_create(heap, a, &ha) == GRHEAP_OK);
  CHECK(grheap_alloc(heap, &pair_type, &b) == GRHEAP_OK);
  CHECK(grheap_handle_create(heap, b, &hb) == GRHEAP_OK);

  Pair * holder = a;
#ifdef PLANT_MISSING_STORE
  holder->first = b; /* the bypass */
#else
  CHECK(grheap_store(heap, holder, &holder->first, b) == GRHEAP_OK);
#endif

  uint64_t violations = 0;
  CHECK(grheap_verify(heap, &violations) == GRHEAP_OK);
  printf("violations: %llu\n", (unsigned long long)violations);

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return violations == 0 ? 0 : 1;
}
