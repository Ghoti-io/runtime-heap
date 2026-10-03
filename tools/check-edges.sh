#!/bin/sh
#
# Fail on a dependency edge the spine forbids, in either place it can hide.
#
# AD-2: runtime-heap depends on cutil and runtime-core and nothing else. It
# never reaches codegen (runtime-jit), the debugger (runtime-debug), an engine
# (lang-*), or ctang. It also never reaches runtime-core's A: "gc includes no A
# header" (frame.h, stack.h, unwind.h and the rest of runtime-core/a/), and it
# takes its roots through B's root-source types alone. runtime-core's umbrella
# header includes every A header, so an include of the umbrella is the same
# edge by another name and is refused too.
#
# The rule is checked twice because the two checks see different things: a
# manifest and a clean #include list can both be right while the shared object
# still links a forbidden library, and an #include can be forbidden while the
# link line is clean. A and B ship in one runtime-core shared object (AD-3), so
# the link line cannot tell them apart; the #include rule is what holds the
# line there.
#
# The check is an allowlist, not a list of known-bad names: any
# <ghoti.io/X/...> include or libghoti.io-X NEEDED entry whose X is not cutil,
# runtime-core or runtime-heap is an edge. A new forbidden library then needs
# no edit here, and a typo in a deny list cannot let one through. The one rule
# inside the allowlist is the A rule above.
#
# Usage:
#   check-edges.sh --includes <root>   scan <root>/src and <root>/include
#   check-edges.sh --links <dir>       read the NEEDED list of every shared
#                                      object under <dir>
#
# Each mode fails on an empty population.

set -eu

mode="${1:?usage: check-edges.sh --includes <root> | --links <dir>}"
target="${2:?usage: check-edges.sh --includes <root> | --links <dir>}"

ALLOWED='cutil|runtime-core|runtime-heap'
status=0

case "$mode" in
  --includes)
    files=""
    for d in "$target/src" "$target/include"; do
      if [ -d "$d" ]; then
        found="$(find "$d" -type f \( -name '*.c' -o -name '*.h' \
          -o -name '*.cpp' \) | sort)"
        files="$files
$found"
      fi
    done
    files="$(printf '%s\n' "$files" | sed '/^$/d')"
    if [ -z "$files" ]; then
      printf 'check-edges: no sources under %s/src or %s/include; this gate is measuring nothing\n' \
        "$target" "$target" >&2
      exit 1
    fi
    count=0
    for f in $files; do
      count=$((count + 1))
      hits="$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]ghoti\.io/[^/>"]+/' "$f" || true)"
      [ -n "$hits" ] || continue
      while IFS= read -r line; do
        n="${line%%:*}"
        lib="$(printf '%s\n' "$line" \
          | sed -E 's/.*[<"]ghoti\.io\/([^\/>"]+)\/.*/\1/')"
        if ! printf '%s\n' "$lib" | grep -qE "^($ALLOWED)\$"; then
          printf 'check-edges: forbidden edge runtime-heap -> %s: %s:%s: %s\n' \
            "$lib" "$f" "$n" "${line#*:}" >&2
          status=1
          continue
        fi
        if [ "$lib" = runtime-core ]; then
          # What follows runtime-core/ in the include path: a/stack.h, or
          # runtime-core.h (the umbrella). b/ and the top-level basics are the
          # interface this library is allowed to use.
          rest="$(printf '%s\n' "${line#*:}" \
            | sed -E 's/.*[<"]ghoti\.io\/runtime-core\/([^>"]*)[>"].*/\1/')"
          case "$rest" in
            a/* | runtime-core.h)
              printf 'check-edges: forbidden edge runtime-heap -> runtime-core/%s (AD-2: gc includes no A header, nor the umbrella that includes them): %s:%s: %s\n' \
                "$rest" "$f" "$n" "${line#*:}" >&2
              status=1
              ;;
          esac
        fi
      done <<HITS
$hits
HITS
    done
    if [ "$status" -ne 0 ]; then
      exit 1
    fi
    printf 'check-edges: %d source files, every #include of another Ghoti library is cutil, runtime-core (B only) or this library\n' \
      "$count"
    ;;

  --links)
    objects="$(find "$target" -type f \( -name '*.so' -o -name '*.so.*' \
      -o -name '*.dll' -o -name '*.dylib' \) 2>/dev/null | sort)"
    if [ -z "$objects" ]; then
      printf 'check-edges: no shared objects under %s; this gate is measuring nothing\n' \
        "$target" >&2
      exit 1
    fi
    count=0
    for so in $objects; do
      count=$((count + 1))
      if [ "$(uname -s)" = Darwin ] && command -v otool >/dev/null 2>&1; then
        needed="$(otool -L "$so" | sed '1d' | awk '{print $1}' \
          | sed 's|.*/||')"
      elif command -v readelf >/dev/null 2>&1 \
        && readelf -d "$so" >/dev/null 2>&1; then
        needed="$(readelf -d "$so" \
          | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')"
      elif command -v objdump >/dev/null 2>&1; then
        # objdump -p reads both ELF (NEEDED) and PE (DLL Name:), which is what
        # a Windows build has.
        needed="$(objdump -p "$so" \
          | awk '$1 == "NEEDED" {print $2} /DLL Name:/ {print $3}')"
      else
        printf 'check-edges: neither readelf nor objdump is available; cannot read %s\n' \
          "$so" >&2
        exit 1
      fi
      if [ -z "$needed" ]; then
        # A real shared object links at least libc. An empty answer is a
        # reader that read nothing, which would pass any planted link.
        printf 'check-edges: read no dependencies from %s; the reader is not seeing the link line\n' \
          "$so" >&2
        exit 1
      fi
      for dep in $needed; do
        case "$dep" in
          libghoti.io-*)
            lib="${dep#libghoti.io-}"
            # libghoti.io-runtime-heap-0.so.0 -> runtime-heap.
            lib="$(printf '%s\n' "$lib" \
              | sed -E 's/(-[0-9]+)?(-debug)?(\.so.*|\.dll.*|\.dylib.*)$//')"
            # Exactly an allowed name, or one followed by a BRANCH suffix
            # (cutil-dev, cutil-0-debug); never a different library that
            # merely starts with one.
            if ! printf '%s\n' "$lib" | grep -qE "^($ALLOWED)(-.*)?\$"; then
              printf 'check-edges: forbidden edge runtime-heap -> %s: %s has NEEDED %s\n' \
                "$lib" "$so" "$dep" >&2
              status=1
            fi
            ;;
        esac
      done
    done
    if [ "$status" -ne 0 ]; then
      exit 1
    fi
    printf 'check-edges: %d shared objects, every Ghoti library they link is cutil, runtime-core or this library\n' \
      "$count"
    ;;

  *)
    printf 'usage: check-edges.sh --includes <root> | --links <dir>\n' >&2
    exit 2
    ;;
esac
