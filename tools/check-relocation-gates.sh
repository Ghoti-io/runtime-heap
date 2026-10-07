#!/bin/sh
#
# Prove that the relocation torture catches what it exists to catch, and that
# its own tests catch a defect in it. Run by `make check-relocation-gates` in a
# build made with RELOCATE=yes.
#
# Two kinds of planted defect, each seen to fail:
#
#   Fixtures (tests/gates/relocate) are small C programs, each built twice, a
#   control and a build with one engine-side defect planted, and run under and
#   without GRHEAP_RELOCATE=1:
#
#     visited_not_updated.c  a slot reported through a copy. The planted build,
#                            under relocation, exits non-zero and says "poisoned
#                            read"; without it, exits zero (relocation is what
#                            finds it); the control exits zero under it.
#     stale_untraced.c       a field `trace` leaves out: the same three claims.
#
#   Library plants (GRHEAP_PLANT_RELOC_*, in src/heap): the library itself is
#   built, one defect at a time, with a table or slot kind left un-updated, and
#   the one unit test named for it (tests/unit/test_relocate.cpp) must fail on its
#   own assertions, or die while it is the test that runs.
#   The control build must pass the same tests, so that a test which fails for
#   any reason does not count. A plant that does not reach the build (a
#   misspelt macro) passes the test and so fails this gate.
#
# It fails when the fixture directory is empty or lacks a required program,
# when a program cannot be built, and when a planted build is not seen to fail.
#
# Usage: check-relocation-gates.sh
#   Run from the project root by `make check-relocation-gates`, which supplies
#   in the environment: MAKE, CC, CFLAGS, INCLUDE, LIBS (the link line) and
#   LD_LIBRARY_PATH, and passes PREFIX to the sub-makes through MAKEFLAGS.
#
#   For a build made for another machine, GRHEAP_GATE_RUNNER is the command that
#   runs one of its programs (for arm64 under qemu-user, `qemu-aarch64 -L
#   /usr/aarch64-linux-gnu`): every fixture and every test program the gate runs
#   is run through it, and the gate's own claims (a planted defect fails, its
#   control passes) are checked on that machine's code. Unset, programs run
#   directly, as before.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
DIR="$ROOT/tests/gates/relocate"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac

# The runner of programs built for another machine, or nothing.
RUNNER="${GRHEAP_GATE_RUNNER:-}"

work="$(mktemp -d)"
cleanup() {
  rm -rf "$work" "$ROOT"/build/plant-*
}
trap cleanup EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-relocation-gates: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

if [ -z "${CC:-}" ] || [ -z "${LIBS:-}" ] || [ -z "${MAKE:-}" ]; then
  printf 'check-relocation-gates: MAKE, CC and LIBS must be set (run it through make check-relocation-gates)\n' >&2
  exit 1
fi
for need in visited_not_updated stale_untraced; do
  if [ ! -f "$DIR/$need.c" ]; then
    printf 'check-relocation-gates: %s/%s.c is missing; this gate is measuring nothing\n' \
      "$DIR" "$need" >&2
    exit 1
  fi
done

# build <name> <control|planted> [define]  ->  $work/<name>-<which>
build() {
  name="$1"; which="$2"; define="${3:-}"
  out="$work/$name-$which$EXE"
  # shellcheck disable=SC2086
  if ! $CC ${CFLAGS:-} ${INCLUDE:-} $define -o "$out" "$DIR/$name.c" $LIBS \
      >"$work/$name-$which.log" 2>&1; then
    fail "could not build $DIR/$name.c ($which):
$(cat "$work/$name-$which.log")"
    return 1
  fi
  return 0
}

# run <binary> <RELOCATE>   (an empty value means unset). Sets $rc and $out.
run() {
  bin="$1$EXE"; reloc="$2"
  # shellcheck disable=SC2086
  out="$( (env -u GRHEAP_TORTURE -u GRHEAP_VERIFY -u GRHEAP_RELOCATE \
      ${reloc:+GRHEAP_RELOCATE=$reloc} $RUNNER "$bin") 2>&1)"
  rc=$?
}

expect() {
  label="$1"; want="$2"; needle="$3"
  checks=$((checks + 1))
  if [ "$want" = zero ] && [ "$rc" -ne 0 ]; then
    fail "$label should exit 0 but exited $rc:
$out"
  elif [ "$want" = nonzero ] && [ "$rc" -eq 0 ]; then
    fail "$label should fail but exited 0:
$out"
  elif [ -n "$needle" ] && ! printf '%s' "$out" | grep -Eq -- "$needle"; then
    fail "$label gave the right status but without '$needle':
$out"
  else
    printf '  ok   %s\n' "$label"
  fi
}

printf 'visited_not_updated (a slot reported through a copy)\n'
if build visited_not_updated control && build visited_not_updated planted -DPLANT_VISITED_NOT_UPDATED; then
  run "$work/visited_not_updated-planted" 1
  expect 'planted visited-not-updated, relocation on: fails, naming the poisoned read' nonzero 'poisoned read'
  run "$work/visited_not_updated-planted" ""
  expect 'planted visited-not-updated, relocation off: passes (relocation is what catches it)' zero ''
  run "$work/visited_not_updated-control" 1
  expect 'control, relocation on: passes' zero ''
  run "$work/visited_not_updated-control" ""
  expect 'control, relocation off: passes' zero ''
fi

printf 'stale_untraced (a field trace leaves out)\n'
if build stale_untraced control && build stale_untraced planted -DPLANT_UNTRACED_FIELD; then
  run "$work/stale_untraced-planted" 1
  expect 'planted untraced field, relocation on: fails, naming the poisoned read' nonzero 'poisoned read'
  run "$work/stale_untraced-planted" ""
  expect 'planted untraced field, relocation off: passes (relocation is what catches it)' zero ''
  run "$work/stale_untraced-control" 1
  expect 'control, relocation on: passes' zero ''
fi

# lib_plant <macro> <test>: the library built with <macro>, and the one test
# that is named for it, which must fail ON ITS OWN: the output must name that
# test as FAILED, or the program must have died (a signal) while that test was
# the one running. A failure of any other test, or a build that failed, does not
# count, and the filter runs that test alone so nothing else can answer for it.
lib_plant() {
  macro="$1"; test_name="$2"
  tree="./build/plant-$macro"
  checks=$((checks + 1))
  if ! (cd "$ROOT" && $MAKE --no-print-directory RELOCATE=yes BUILD_DIR="$tree" \
      EXTRA_CFLAGS="-D$macro" "$tree/apps/testRelocate$EXE") >"$work/$macro.log" 2>&1; then
    fail "could not build the library with $macro:
$(tail -20 "$work/$macro.log")"
    return
  fi
  # shellcheck disable=SC2086
  out="$( (cd "$ROOT" && env -u GRHEAP_TORTURE -u GRHEAP_VERIFY -u GRHEAP_RELOCATE \
      $RUNNER "$tree/apps/testRelocate$EXE" --gtest_filter="RelocateTest.$test_name") 2>&1)"
  rc=$?
  last_run="$(printf '%s\n' "$out" | grep '^\[ RUN ' | tail -1)"
  if [ "$rc" -eq 0 ]; then
    fail "the library built with $macro passed $test_name: the defect was not seen"
  elif printf '%s\n' "$out" | grep -q "^\[  FAILED  \] RelocateTest\.$test_name"; then
    printf '  ok   %s: %s fails its own assertions\n' "$macro" "$test_name"
  elif [ "$rc" -ge 128 ] && printf '%s' "$last_run" | grep -q "RelocateTest\.$test_name"; then
    printf '  ok   %s: %s dies (signal %d) while it is the test that runs\n' "$macro" "$test_name" "$((rc - 128))"
  else
    fail "the library built with $macro failed, but not in $test_name (exit $rc, last test started: $last_run)"
  fi
}

printf 'the control library passes the tests that follow\n'
checks=$((checks + 1))
control="./build/plant-control"
if (cd "$ROOT" && $MAKE --no-print-directory RELOCATE=yes BUILD_DIR="$control" \
    "$control/apps/testRelocate$EXE") >"$work/control.log" 2>&1; then
  # shellcheck disable=SC2086
  out="$( (cd "$ROOT" && env -u GRHEAP_TORTURE -u GRHEAP_VERIFY -u GRHEAP_RELOCATE \
      $RUNNER "$control/apps/testRelocate$EXE" --gtest_brief=1) 2>&1)"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    fail "the control library fails its own relocation tests:
$out"
  else
    printf '  ok   the unplanted library passes every relocation test\n'
  fi
else
  fail "could not build the control library:
$(tail -20 "$work/control.log")"
fi

printf 'library defects, each planted alone\n'
lib_plant GRHEAP_PLANT_RELOC_ROOT_STALE ARegisteredRootSlotNamesTheNewAddress
lib_plant GRHEAP_PLANT_RELOC_HANDLE_STALE AHandleNamesTheNewAddress
lib_plant GRHEAP_PLANT_RELOC_SOURCE_STALE ARootSourceSlotNamesTheNewAddress
lib_plant GRHEAP_PLANT_RELOC_ENCODE_STALE ACodecWordSlotKeepsItsTagAndNamesTheNewAddress
lib_plant GRHEAP_PLANT_RELOC_COPY_SKIP AMovedObjectsContentsAreCopiedToItsNewCell
lib_plant GRHEAP_PLANT_RELOC_WEAK_STALE TheWeakRegistryNamesTheNewAddressOfACell
lib_plant GRHEAP_PLANT_RELOC_WEAKSLOT_STALE AWeakSlotAndAnEphemeronsSlotsNameTheNewAddresses
lib_plant GRHEAP_PLANT_RELOC_IDS_STALE AnIdResolvesToTheNewAddressOfItsObject
lib_plant GRHEAP_PLANT_RELOC_INDEX_STALE TheIdIndexFindsAnObjectAtItsNewAddress
lib_plant GRHEAP_PLANT_RELOC_SHADOW_STALE TheBarrierVerifyShadowFollowsAMovedObjectAndItsSlots
lib_plant GRHEAP_PLANT_RELOC_MOVE_PINNED APinnedObjectAndAConservativelyHeldObjectKeepTheirAddressAndOthersMove

if [ "$failures" -ne 0 ]; then
  printf 'check-relocation-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-relocation-gates: all %d checks behaved: each defect, planted in an engine or in the library, is seen to fail, and passes unseen where relocation is off\n' \
  "$checks"
