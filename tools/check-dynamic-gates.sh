#!/bin/sh
#
# Prove that the dynamic instruments fail on a planted defect.
#
# GC torture and barrier-verify are instruments, and an instrument that has
# never been seen to find a defect may be measuring nothing. This builds two
# small C programs from tests/gates/dynamic, each twice - a control, and a
# build with one defect planted - runs them under and without the instrument,
# and requires:
#
#   missing_root.c   the planted build, under GRHEAP_TORTURE=1, exits non-zero
#                    and says "poisoned read"; the same build without torture
#                    exits zero (so that torture, and not the program, is what
#                    finds the defect); the control exits zero under torture.
#   missing_store.c  the planted build, under GRHEAP_VERIFY=1, aborts with a
#                    "barrier-verify" report; under GRHEAP_VERIFY=count it
#                    reports at least one violation and exits non-zero; with
#                    verify off it exits zero; the control reports zero
#                    violations and exits zero in every mode.
#
# It fails when the fixture directory holds no programs (a gate that measures
# nothing reports success), when a required program is missing, and when a
# program cannot be built.
#
# Usage: check-dynamic-gates.sh [fixture-dir]
#   Run from the project root by `make check-dynamic-gates`, which supplies the
#   build in the environment:
#     CC CFLAGS INCLUDE   the compiler and its flags
#     LIBS                the link line (the archive, runtime-core, cutil)
#     LD_LIBRARY_PATH     where the shared libraries are, for the run

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
DIR="${1:-$ROOT/tests/gates/dynamic}"

# On Windows the compiler appends .exe to whatever -o names, so the program it
# wrote is not the path that was asked for; name the suffix once and use it
# wherever a built program is written or run.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-dynamic-gates: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

if [ ! -d "$DIR" ]; then
  printf 'check-dynamic-gates: %s does not exist; this gate is measuring nothing\n' \
    "$DIR" >&2
  exit 1
fi
programs="$(find "$DIR" -type f -name '*.c' | sort)"
if [ -z "$programs" ]; then
  printf 'check-dynamic-gates: no programs under %s; this gate is measuring nothing\n' \
    "$DIR" >&2
  exit 1
fi
for need in missing_root missing_store; do
  # A fixture directory that has programs but not the required ones is not
  # the population this gate was written for.
  case "$programs" in
    *"/$need.c"*) ;;
    *)
      if [ "$DIR" = "$ROOT/tests/gates/dynamic" ]; then
        printf 'check-dynamic-gates: %s/%s.c is missing; this gate is measuring nothing\n' \
          "$DIR" "$need" >&2
        exit 1
      fi
      ;;
  esac
done

if [ -z "${CC:-}" ] || [ -z "${LIBS:-}" ]; then
  printf 'check-dynamic-gates: CC and LIBS must be set (run it through make check-dynamic-gates)\n' >&2
  exit 1
fi

# build <name> <control|planted> [define]  ->  $work/<name>-<which>
build() {
  name="$1"; which="$2"; define="${3:-}"
  src="$DIR/$name.c"
  out="$work/$name-$which$EXE"
  # shellcheck disable=SC2086
  if ! $CC ${CFLAGS:-} ${INCLUDE:-} $define -o "$out" "$src" $LIBS \
      >"$work/$name-$which.log" 2>&1; then
    fail "could not build $src ($which):
$(cat "$work/$name-$which.log")"
    return 1
  fi
  return 0
}

# run <binary> <TORTURE> <VERIFY>   (an empty value means unset)
# Sets $rc and $out. The instrument variables are set or removed explicitly, so
# that a make test-torture run exporting them cannot change what this proves.
run() {
  bin="$1$EXE"; torture="$2"; verify="$3"
  # shellcheck disable=SC2086
  out="$( (env -u GRHEAP_TORTURE -u GRHEAP_VERIFY \
      ${torture:+GRHEAP_TORTURE=$torture} ${verify:+GRHEAP_VERIFY=$verify} \
      "$bin") 2>&1)"
  rc=$?
}

# expect <label> <exit: zero|nonzero> <needle or ''> -- after a run
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

printf 'missing_root (GC torture)\n'
if [ -f "$DIR/missing_root.c" ] && build missing_root control && build missing_root planted -DPLANT_MISSING_ROOT; then
  run "$work/missing_root-planted" 1 ""
  expect 'planted missing root, torture on: fails, naming the poisoned read' nonzero 'poisoned read'
  run "$work/missing_root-planted" "" ""
  expect 'planted missing root, torture off: passes (torture is what catches it)' zero ''
  run "$work/missing_root-control" 1 ""
  expect 'control, torture on: passes' zero ''
  run "$work/missing_root-control" "" ""
  expect 'control, torture off: passes' zero ''
fi

printf 'missing_store (barrier-verify)\n'
if [ -f "$DIR/missing_store.c" ] && build missing_store control && build missing_store planted -DPLANT_MISSING_STORE; then
  run "$work/missing_store-planted" "" 1
  expect 'planted missing store, verify abort: dies with a barrier-verify report' nonzero 'barrier-verify'
  run "$work/missing_store-planted" "" count
  expect 'planted missing store, verify count: reports at least one violation' nonzero 'violations: [1-9]'
  run "$work/missing_store-planted" "" ""
  expect 'planted missing store, verify off: passes (verify is what catches it)' zero 'violations: 0'
  run "$work/missing_store-control" "" 1
  expect 'control, verify abort: passes' zero 'violations: 0'
  run "$work/missing_store-control" "" count
  expect 'control, verify count: reports zero' zero 'violations: 0'
  run "$work/missing_store-control" 1 count
  expect 'control, torture and verify: passes' zero 'violations: 0'
fi

# Programs other than the two above (a fixture directory a test points this
# script at) are built, as a control only, so that an unbuildable one fails.
for src in $programs; do
  name="$(basename "$src" .c)"
  case "$name" in
    missing_root | missing_store) ;;
    *)
      checks=$((checks + 1))
      build "$name" control && printf '  ok   %s builds\n' "$name"
      ;;
  esac
done

if [ "$failures" -ne 0 ]; then
  printf 'check-dynamic-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-dynamic-gates: all %d checks behaved: each instrument fails on its planted defect, passes its control, and the defect passes unseen when the instrument is off\n' \
  "$checks"
