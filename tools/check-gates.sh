#!/bin/sh
#
# Prove that each layering gate fails on the defect it exists to catch.
#
# A gate that has never been seen to fail may be measuring nothing. So this
# runs the real scripts - not copies, not mocks - against tests/gates:
#
#   control/   a tree that is correct; the gate must pass it, so that a gate
#              which rejects everything cannot pass this self-test
#   planted-*  a tree with one defect; the gate must exit non-zero AND name
#              the offending file, line or edge, so that a gate which fails
#              for the wrong reason (a typo, a missing tool) does not count
#   (empty)    an empty directory; the gate must fail rather than report
#              success over a population of zero
#
# The link-line check cannot use a committed fixture, because what it reads is
# a built shared object's NEEDED list. So it builds real ones in a
# temporary directory, against stub libraries: one that links a forbidden
# library, one that links the stubs of the two allowed dependencies (cutil and
# runtime-core), and one that links a BRANCH-suffixed cutil.
#
# The dynamic gates (torture, barrier-verify) have their own script,
# check-dynamic-gates.sh; this file checks that it too fails on an empty
# population and on a program it cannot build. Run through make, which supplies
# the build (CC, CFLAGS, INCLUDE, LIBS) that script compiles with.
#
# Usage: make check-gates   (or check-gates.sh with that environment set)

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
FIX="$ROOT/tests/gates"
CC="${CC:-cc}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-gates: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

# expect_pass <label> <command...>
expect_pass() {
  label="$1"; shift
  checks=$((checks + 1))
  if out="$("$@" 2>&1)"; then
    printf '  ok   %s passes\n' "$label"
  else
    fail "$label should pass but failed:
$out"
  fi
}

# expect_fail <label> <needle> <command...>
# The needle is what the failure message must name.
expect_fail() {
  label="$1"; needle="$2"; shift 2
  checks=$((checks + 1))
  out="$("$@" 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$label should fail but exited 0:
$out"
  elif ! printf '%s' "$out" | grep -qF -- "$needle"; then
    fail "$label failed, but without naming '$needle':
$out"
  else
    printf '  ok   %s fails, naming %s\n' "$label" "$needle"
  fi
}

mkdir "$work/empty"

printf 'check-labels\n'
L="$HERE/check-labels.sh"
expect_pass 'labels/control' "$L" "$FIX/labels/control"
expect_fail 'labels/planted-missing' 'nolabel.h' "$L" "$FIX/labels/planted-missing"
expect_fail 'labels/planted-bogus' '"experimental"' "$L" "$FIX/labels/planted-bogus"
expect_fail 'labels/planted-prose-only' 'top.h has no @stability' \
  "$L" "$FIX/labels/planted-prose-only"
expect_fail 'labels/planted-free' 'top.h is labelled free' \
  "$L" "$FIX/labels/planted-free"
expect_fail 'labels/planted-two-labels' 'more than one @stability' \
  "$L" "$FIX/labels/planted-two-labels"
expect_fail 'labels/empty' 'measuring nothing' "$L" "$work/empty"

printf 'check-edges --includes\n'
E="$HERE/check-edges.sh"
expect_pass 'edges/control' "$E" --includes "$FIX/edges/control"
expect_fail 'edges/planted-a-stack-src' 'runtime-core/a/stack.h' \
  "$E" --includes "$FIX/edges/planted-a-stack-src"
expect_fail 'edges/planted-a-frame-header' 'runtime-core/a/frame.h' \
  "$E" --includes "$FIX/edges/planted-a-frame-header"
expect_fail 'edges/planted-a-quoted' 'runtime-core/a/unwind.h' \
  "$E" --includes "$FIX/edges/planted-a-quoted"
expect_fail 'edges/planted-umbrella-src' 'runtime-core/runtime-core.h' \
  "$E" --includes "$FIX/edges/planted-umbrella-src"
expect_fail 'edges/planted-umbrella-header' 'runtime-core/runtime-core.h' \
  "$E" --includes "$FIX/edges/planted-umbrella-header"
expect_fail 'edges/planted-jit' 'runtime-heap -> runtime-jit' \
  "$E" --includes "$FIX/edges/planted-jit"
expect_fail 'edges/planted-debug-header' 'runtime-heap -> runtime-debug' \
  "$E" --includes "$FIX/edges/planted-debug-header"
expect_fail 'edges/planted-ctang' 'runtime-heap -> tang' \
  "$E" --includes "$FIX/edges/planted-ctang"
expect_fail 'edges/planted-engine' 'runtime-heap -> lang-tang' \
  "$E" --includes "$FIX/edges/planted-engine"
expect_fail 'edges/empty (includes)' 'measuring nothing' \
  "$E" --includes "$work/empty"

printf 'check-edges --links\n'
# TODO(windows): the .dll arm has not been run; objdump -p is the reader there.
case "$(uname -s)" in
  MINGW* | MSYS*) SHEXT=dll; SHFLAGS="-shared" ;;
  Darwin) SHEXT=dylib; SHFLAGS="-dynamiclib" ;;
  *) SHEXT=so; SHFLAGS="-shared -fPIC" ;;
esac
stubs="$work/stubs"
mkdir -p "$stubs" "$work/planted" "$work/control" "$work/dev"
printf 'int stub_jit(void) { return 1; }\n' > "$work/jit.c"
printf 'int stub_cutil(void) { return 2; }\n' > "$work/cutil.c"
printf 'int stub_core(void) { return 3; }\n' > "$work/core.c"
printf 'int stub_jit(void);\nint planted(void) { return stub_jit(); }\n' \
  > "$work/planted.c"
printf 'int stub_cutil(void);\nint stub_core(void);\nint control(void) { return stub_cutil() + stub_core(); }\n' \
  > "$work/control.c"
printf 'int stub_cutil(void);\nint control(void) { return stub_cutil(); }\n' \
  > "$work/dev.c"

built=1
# shellcheck disable=SC2086
{
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-jit-0.$SHEXT" "$work/jit.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-0.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-core-0.$SHEXT" "$work/core.c" &&
  $CC $SHFLAGS -o "$work/planted/libplanted.$SHEXT" "$work/planted.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-jit-0.$SHEXT &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-dev.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$work/dev/libdev.$SHEXT" "$work/dev.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-dev.$SHEXT &&
  $CC $SHFLAGS -o "$work/control/libcontrol.$SHEXT" "$work/control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-0.$SHEXT \
    -l:libghoti.io-runtime-core-0.$SHEXT
} >"$work/build.log" 2>&1 || built=0
if [ "$built" -eq 0 ]; then
  fail "could not build the link-line fixtures:
$(cat "$work/build.log")"
else
  expect_pass 'links/control (cutil and runtime-core)' "$E" --links "$work/control"
  expect_pass 'links/cutil with a BRANCH suffix (-dev)' "$E" --links "$work/dev"
  expect_fail 'links/planted-jit' 'runtime-heap -> runtime-jit' \
    "$E" --links "$work/planted"
  expect_fail 'links/planted-jit names the object' 'libplanted' \
    "$E" --links "$work/planted"
fi
expect_fail 'links/empty' 'measuring nothing' "$E" --links "$work/empty"

printf 'check-dynamic-gates\n'
# The dynamic gate builds programs, so it needs the build's compiler and link
# line (make supplies them). Without them this self-test could not tell a gate
# that fails for the right reason from one that fails for a missing tool.
D="$HERE/check-dynamic-gates.sh"
if [ -z "${LIBS:-}" ]; then
  fail "LIBS is not set: run this through 'make check-gates', which supplies the build the dynamic gates compile with"
else
  expect_fail 'dynamic/empty' 'measuring nothing' "$D" "$work/empty"
  expect_fail 'dynamic/unbuildable' 'could not build' "$D" "$FIX/dynamic-unbuildable"
fi

if [ "$failures" -ne 0 ]; then
  printf 'check-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-gates: all %d checks behaved: each gate fails on its planted defect, passes its control, and fails on an empty population\n' \
  "$checks"
