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
# The relocation gates are checked here too. check-no-relocation.sh reads the
# artifacts of two builds the Makefile makes (PLAIN_ARTIFACTS, a normal build,
# and RELOC_ARTIFACTS, one made with RELOCATE=yes): the normal build must pass
# its absence check and fail its presence check, and the option build the
# reverse, each library alone as well as both, and an empty population fails.
# check-symbols' leaving relocate.h out of a normal build is shown against a
# fixture header.
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

expect_fail 'edges/planted-relative' 'runtime-heap -> runtime-jit' \
  "$E" --includes "$FIX/edges/planted-relative"
expect_fail 'edges/planted-example' 'runtime-heap -> runtime-jit' \
  "$E" --includes "$FIX/edges/planted-example"

printf 'check-edges --links\n'
# The .dll arm (objdump -p is the reader there) has run under wine, cross-built
# (tools/xwin in the workspace); it has not run on a Windows machine.
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

# A name that merely begins with an allowed one is another library, not that
# library with a branch suffix.
extra="$work/extra"
mkdir -p "$extra" "$work/extra-stubs"
printf 'int stub_extra(void) { return 4; }\n' > "$work/extra-stub.c"
printf 'int stub_extra(void);\nint extended(void) { return stub_extra(); }\n' > "$work/extended.c"
# shellcheck disable=SC2086
if $CC $SHFLAGS -o "$work/extra-stubs/libghoti.io-cutil-extra-0.$SHEXT" "$work/extra-stub.c" &&
  $CC $SHFLAGS -o "$extra/libextended.$SHEXT" "$work/extended.c" \
    -L"$work/extra-stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-extra-0.$SHEXT \
    >"$work/extra-build.log" 2>&1; then
  expect_fail 'links/planted-name-extending-an-allowed-one' 'runtime-heap -> cutil-extra' \
    "$E" --links "$extra"
else
  fail "could not build the extended-name fixture"
fi

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

printf 'check-stamps (fixtures)\n'
S="$HERE/check-stamps.py"
expect_pass 'stamps/control' python3 "$S" "$FIX/stamps/control.mk"
expect_fail 'stamps/planted-no-stamp' 'names no flag stamp' \
  python3 "$S" "$FIX/stamps/planted-no-stamp.mk"
expect_fail 'stamps/planted-unrecorded-flag' 'EXTRA_CFLAGS' \
  python3 "$S" "$FIX/stamps/planted-unrecorded-flag.mk"
expect_fail 'stamps/planted-link-unrecorded' 'LINK_EXTRA' \
  python3 "$S" "$FIX/stamps/planted-link-unrecorded.mk"
expect_fail 'stamps/planted-no-printf' 'does not printf' \
  python3 "$S" "$FIX/stamps/planted-no-printf.mk"
expect_fail 'stamps/planted-no-stamps' 'no flag stamps at all' \
  python3 "$S" "$FIX/stamps/planted-no-stamps.mk"
expect_fail 'stamps/planted-no-compile' 'no compile rules' \
  python3 "$S" "$FIX/stamps/planted-no-compile.mk"
expect_fail 'stamps/planted-no-link' 'no link lines' \
  python3 "$S" "$FIX/stamps/planted-no-link.mk"

# check-symbols reads a built shared object's dynamic symbol table, which is
# nm -D and so Linux only (the Makefile skips it elsewhere too).
if [ "$(uname -s)" = Linux ]; then
  printf 'check-symbols\n'
  Y="$HERE/check-symbols.sh"
  tok=ghotiio_runtime_heap_0
  sym="$work/sym"
  mkdir -p "$sym"
  printf 'int %s_grheap_ctx_make(void) { return 1; }\n' "$tok" > "$sym/good.c"
  printf 'int grheap_leaked(void) { return 1; }\n' > "$sym/bad.c"
  printf 'int %s_grheap_ctx_make(void) { return 1; }\nint %s_grheap_missing(void);\nint %s_grheap_user(void) { return %s_grheap_missing(); }\n' \
    "$tok" "$tok" "$tok" "$tok" > "$sym/split.c"
  printf 'int %s_grheap_other(void) { return 1; }\n' "$tok" > "$sym/other.c"
  printf 'static int hidden(void) { return 1; }\n' > "$sym/empty.c"
  built=1
  {
    $CC -shared -fPIC -o "$sym/good.so" "$sym/good.c" &&
    $CC -shared -fPIC -o "$sym/bad.so" "$sym/bad.c" &&
    $CC -shared -fPIC -o "$sym/split.so" "$sym/split.c" &&
    $CC -shared -fPIC -o "$sym/other.so" "$sym/other.c" &&
    $CC -shared -fPIC -o "$sym/empty.so" "$sym/empty.c" &&
    printf 'int %s_grheap_ctx_make(void) { return 1; }\nint %s_grheap_state_digest(void) { return 1; }\n' "$tok" "$tok" > "$sym/testonly.c" &&
    $CC -shared -fPIC -o "$sym/testonly.so" "$sym/testonly.c"
  } >"$sym/build.log" 2>&1 || built=0
  if [ "$built" -eq 0 ]; then
    fail "could not build the symbol fixtures:
$(cat "$sym/build.log")"
  else
    expect_pass 'symbols/control' "$Y" "$sym/good.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-unnamespaced-export' 'grheap_leaked' \
      "$Y" "$sym/bad.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-split-symbol' 'split symbol' \
      "$Y" "$sym/split.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-api-not-exported' 'Declared GRHEAP_API functions' \
      "$Y" "$sym/other.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-no-api' 'grheap_ctx_make' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-no-api"
    expect_fail 'symbols/planted-no-macros' 'ctx_internal.h' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-no-macros"
    expect_fail 'symbols/planted-bad-guard' 'MY_OWN_GUARD_H' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-bad-guard"
    expect_fail 'symbols/planted-dup-guard' 'sharing an include guard' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-dup-guard"
    expect_fail 'symbols/exports-nothing' 'measuring nothing' \
      "$Y" "$sym/empty.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/no-library' 'measuring nothing' \
      "$Y" "$sym/absent.so" "$tok" "$FIX/symbols/control"
    expect_pass 'symbols/test-only list, clean library' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/control" grheap_state_digest
    expect_fail 'symbols/planted-test-only-code-shipped' 'grheap_state_digest' \
      "$Y" "$sym/testonly.so" "$tok" "$FIX/symbols/control" grheap_state_digest
  fi
fi

printf 'check-no-relocation\n'
N="$HERE/check-no-relocation.sh"
if [ "$(uname -s)" != Linux ]; then
  printf '  (skipped: the gate reads nm output, Linux only)\n'
elif [ -z "${PLAIN_ARTIFACTS:-}" ] || [ -z "${RELOC_ARTIFACTS:-}" ]; then
  fail "PLAIN_ARTIFACTS and RELOC_ARTIFACTS are not set: run this through 'make check-gates', which builds both"
else
  plain_so="${PLAIN_ARTIFACTS%% *}"; plain_a="${PLAIN_ARTIFACTS##* }"
  reloc_so="${RELOC_ARTIFACTS%% *}"; reloc_a="${RELOC_ARTIFACTS##* }"
  expect_pass 'relocation/control: a normal build (shared and static)' "$N" --absent $PLAIN_ARTIFACTS
  expect_fail 'relocation/planted: a build with the option, shared and static' 'grheap_reloc_run' \
    "$N" --absent $RELOC_ARTIFACTS
  expect_fail 'relocation/planted: the option build'"'"'s shared library alone' 'contains relocation code' \
    "$N" --absent "$reloc_so"
  expect_fail 'relocation/planted: the option build'"'"'s static archive alone' 'contains relocation code' \
    "$N" --absent "$reloc_a"
  expect_pass 'relocation/control: a normal build'"'"'s shared library alone' "$N" --absent "$plain_so"
  expect_pass 'relocation/control: a normal build'"'"'s static archive alone' "$N" --absent "$plain_a"
  expect_pass 'relocation/present: a build with the option' "$N" --present $RELOC_ARTIFACTS
  expect_fail 'relocation/planted: a normal build where the option was wanted' 'lacks the relocation phase' \
    "$N" --present $PLAIN_ARTIFACTS
  expect_fail 'relocation/empty population' 'measuring nothing' "$N" --absent
  expect_fail 'relocation/missing artifact' 'measuring nothing' "$N" --absent "$work/absent.so"
  # An object with no grheap symbol says nothing about relocation.
  printf 'int unrelated(void) { return 1; }\n' > "$work/unrelated.c"
  if $CC -shared -fPIC -o "$work/unrelated.so" "$work/unrelated.c" 2>/dev/null; then
    expect_fail 'relocation/an object with no grheap symbol' 'measuring nothing' "$N" --absent "$work/unrelated.so"
  else
    fail "could not build the unrelated shared object"
  fi
  # A library whose only relocation name is the query passes; any other
  # name that contains "reloc" does not.
  printf 'int ghotiio_runtime_heap_0_grheap_relocation_available(void) { return 0; }\nint ghotiio_runtime_heap_0_grheap_x(void) { return 1; }\n' > "$work/query.c"
  printf 'int ghotiio_runtime_heap_0_grheap_relocation_available(void) { return 0; }\nint ghotiio_runtime_heap_0_grheap_x(void) { return 1; }\nint ghotiio_runtime_heap_0_grheap_reloc_sneaked(void) { return 2; }\n' > "$work/sneak.c"
  if $CC -shared -fPIC -o "$work/query.so" "$work/query.c" 2>/dev/null &&
      $CC -shared -fPIC -o "$work/sneak.so" "$work/sneak.c" 2>/dev/null; then
    expect_pass 'relocation/control: only the query' "$N" --absent "$work/query.so"
    expect_fail 'relocation/planted: one stray relocation name beside the query' 'grheap_reloc_sneaked' \
      "$N" --absent "$work/sneak.so"
  else
    fail "could not build the query fixtures"
  fi
  # check-symbols leaves relocate.h out of a normal build only.
  Y="$HERE/check-symbols.sh"
  mkdir -p "$work/relroot/include/ghoti.io/runtime-heap"
  cp -r "$FIX/symbols/control/include" "$FIX/symbols/control/src" "$work/relroot/"
  printf '#ifndef GHOTI_IO_GRHEAP_RELOCATE_H\n#define GHOTI_IO_GRHEAP_RELOCATE_H\n#include <ghoti.io/runtime-heap/macros.h>\nGRHEAP_API int grheap_options_set_relocate(void);\n#endif\n' \
    > "$work/relroot/include/ghoti.io/runtime-heap/relocate.h"
  tok=ghotiio_runtime_heap_0
  printf 'int %s_grheap_ctx_make(void) { return 1; }\n' "$tok" > "$work/relgood.c"
  if $CC -shared -fPIC -o "$work/relgood.so" "$work/relgood.c" 2>/dev/null; then
    expect_pass 'symbols/relocate.h in a normal build (not exported, not installed)' \
      "$Y" "$work/relgood.so" "$tok" "$work/relroot" "" no
    expect_fail 'symbols/planted: relocate.h declares a function a RELOCATE=yes build does not export' 'grheap_options_set_relocate' \
      "$Y" "$work/relgood.so" "$tok" "$work/relroot" "" yes
  else
    fail "could not build the relocate.h symbol fixture"
  fi
fi

if [ "$failures" -ne 0 ]; then
  printf 'check-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-gates: all %d checks behaved: each gate fails on its planted defect, passes its control, and fails on an empty population\n' \
  "$checks"
