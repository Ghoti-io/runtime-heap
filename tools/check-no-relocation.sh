#!/bin/sh
#
# Fail if a library carries relocation code (--absent), or fail if a build made
# with RELOCATE=yes does not (--present).
#
# The relocation torture (relocate.h) is a test-only mode. Its code and its
# symbols must be in no build but one made with the option: the shared library
# that ships and the default static archive of a normal build must hold neither
# a relocation function nor the string that names its switch. The one trace a
# normal build keeps is grheap_relocation_available, the query that says the mode
# is absent, and it is the one name this gate allows.
#
# What is read, from every artifact named (a shared object or a static archive):
#
#   - every symbol name, local ones included, with nm -a: no name contains
#     "reloc" but the query's;
#   - every printable string: none names the switch (GRHEAP_RELOCATE), the
#     source file (relocate.c) or an internal function (grheap_reloc), apart from
#     the query's own name.
#
# --present is the converse and the same reading: the artifacts do contain the
# relocation phase (grheap_reloc_run) and the switch's name. A build made with
# the option that lacks them would run a "relocation arm" that moves nothing.
#
# check-gates.sh runs this against a normal build (it must pass --absent and
# fail --present) and a build made with the option (the reverse), and against
# an empty population, so the gate is seen to fail.
#
# Usage: check-no-relocation.sh --absent|--present <artifact>...

set -u

MODE="${1:-}"
case "$MODE" in
  --absent | --present) shift ;;
  *)
    printf 'usage: check-no-relocation.sh --absent|--present <artifact>...\n' >&2
    exit 2
    ;;
esac

if [ "$#" -eq 0 ]; then
  printf 'check-no-relocation: no artifact named; this gate is measuring nothing\n' >&2
  exit 1
fi

# The name of the query, which both modes ignore: the whole name, with the
# library's version token in front of it or not, and nothing else around it. A
# longer name that merely contains it (grheap_relocation_available_helper) is
# relocation code like any other.
ALLOWED='(^|[^A-Za-z0-9_])(ghotiio_[A-Za-z0-9_]*_)?grheap_relocation_available([^A-Za-z0-9_]|$)'

status=0
for f in "$@"; do
  if [ ! -s "$f" ]; then
    printf 'check-no-relocation: %s does not exist or is empty; this gate is measuring nothing\n' \
      "$f" >&2
    status=1
    continue
  fi
  # A population check: the artifact must be readable as an object at all, and
  # must name the library's own symbols, or "no relocation symbol" is vacuous.
  syms="$(nm -a "$f" 2>/dev/null | awk '{print $NF}')"
  if ! printf '%s\n' "$syms" | grep -q 'grheap_'; then
    printf 'check-no-relocation: %s holds no grheap_ symbol at all; this gate is measuring nothing\n' \
      "$f" >&2
    status=1
    continue
  fi
  names="$(printf '%s\n' "$syms" | grep -i 'reloc' | grep -vE "$ALLOWED" || true)"
  strs="$(strings -a "$f" | grep -E 'GRHEAP_RELOC|relocate\.c|grheap_reloc|GRHEAP_PLANT_RELOC' \
    | grep -vE "$ALLOWED" || true)"
  case "$MODE" in
    --absent)
      if [ -n "$names" ] || [ -n "$strs" ]; then
        printf 'check-no-relocation: %s contains relocation code, which only a build made with RELOCATE=yes may:\n' \
          "$f" >&2
        [ -n "$names" ] && printf '  symbol: %s\n' $names >&2
        [ -n "$strs" ] && printf '%s\n' "$strs" | sed 's/^/  string: /' >&2
        status=1
      fi
      ;;
    --present)
      if ! printf '%s\n' "$names" | grep -q 'grheap_reloc_run' ||
          ! printf '%s\n' "$strs" | grep -q 'GRHEAP_RELOCATE'; then
        printf 'check-no-relocation: %s lacks the relocation phase (grheap_reloc_run) or the GRHEAP_RELOCATE switch, so a build made with RELOCATE=yes would move nothing\n' \
          "$f" >&2
        status=1
      fi
      ;;
  esac
done

if [ "$status" -ne 0 ]; then
  exit 1
fi
case "$MODE" in
  --absent) printf 'check-no-relocation: %d artifacts, none with relocation code or a relocation symbol\n' "$#" ;;
  --present) printf 'check-relocation-present: %d artifacts, each with the relocation phase\n' "$#" ;;
esac
