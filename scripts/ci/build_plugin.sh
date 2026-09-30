#!/usr/bin/env bash
# Build one plugin library. One file, one job.
#
# Usage:  scripts/ci/build_plugin.sh <out.so|out.dll|out.dylib> [make args...]
# Example: scripts/ci/build_plugin.sh nvmidi.so MIDI_BACKEND=dummy
#
# The plugin half is what `make` already does. The reason this wrapper exists
# is the other half: a library with one exported entry point is not a thing a
# Python process can drive on its own. The plugin registers its types into an
# Angelscript engine, and the engine is a separate library the plugin does not
# carry. So a Python test needs both files, and the second one - the engine -
# has no build step anywhere in this repository.
#
# RULE, and it is why this is one file rather than a paragraph in a yml:
# this script builds, and it fetches; it does not install anything. Every path
# it writes to is under the current directory, so two runs on the same machine
# cannot reach into each other. The workflow that calls it owns the cache; the
# script owns what one build means, and it prints what it found rather than
# assuming.
#
# What it does, and in this order, because each step needs the previous one:
#
#   1. make the plugin. The caller passes the make variables, so this file
#      does not know which backend or which target name is wanted.
#   2. find or build libangelscript. The version is not a preference: the
#      vendored header says ANGELSCRIPT_VERSION 23900, and
#      asCreateScriptEngine returns 0 when the caller's major.minor differ
#      from the library's. A 2.38 library against these headers answers a
#      null pointer and "could not create an Angelscript engine", which
#      reads like a bug in the test.
#   3. print where both files ended up, as absolute paths, because the
#      Python test takes them as an argument and a relative path in a log is
#      a path the next step cannot use.

set -euo pipefail

OUT="${1:?usage: build_plugin.sh <library> [make args...]}"
shift || true

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

echo "building the plugin: make $* -> $OUT"
# TARGET is passed as an environment variable rather than as a command-line
# assignment. On Windows the Makefile sets TARGET itself, in a branch, and a
# command-line value overrides the branch - which is how a build silently takes
# the other platform's recipe. The environment is overridden by the branch, so
# the Makefile keeps the last word on names it owns, and the name comes out
# under this script's control everywhere else.
TARGET="$OUT" make "$@" 2>&1 | tail -40

if [ ! -s "$OUT" ]; then
    echo "FAILED: the build wrote no $OUT"
    exit 1
fi
echo "plugin: $ROOT/$OUT ($(wc -c < "$OUT") bytes)"

# --- the engine -------------------------------------------------------------
#
# Built from upstream, not vendored: third_party/angelscript carries the
# headers and the array add-on, which is what the plugin compiles against, and
# not the engine implementation.
#
# The archive is cached by the workflow under a key that includes this file's
# upstream commit, so a hit skips a compile of about a hundred files and a miss
# is a slow build rather than a wrong one.

ENGINE_TAG="${ANGELS_ENV_TAG:-23900}"
ENGINE_DIR="${ANGELS_ENV_DIR:-$ROOT/.ci/angelscript-sdk}"
ENGINE_LIB="$ROOT/.ci/libangelscript.a"

mkdir -p "$ROOT/.ci"

if [ -s "$ENGINE_LIB" ]; then
    echo "engine: reusing $ENGINE_LIB ($(wc -c < "$ENGINE_LIB") bytes)"
    echo "ENGINE_LIB=$ENGINE_LIB"
    exit 0
fi

if [ ! -d "$ENGINE_DIR/sdk/angelscript/source" ]; then
    echo "engine: fetching the Angelscript source (commit pinned in this script's caller)"
    rm -rf "$ENGINE_DIR"
    mkdir -p "$ENGINE_DIR"
    # The workflow exports ANGELS_SRC_DIR when it has a cached checkout. With
    # nothing cached there is no network fetch here on purpose: a build script
    # that downloads is a build script that fails as a network error, and this
    # one has to be readable when it fails. The workflow fetches; this builds.
    if [ -n "${ANGELS_SRC_DIR:-}" ] && [ -d "$ANGELS_SRC_DIR/sdk/angelscript/source" ]; then
        cp -r "$ANGELS_SRC_DIR/." "$ENGINE_DIR/"
    else
        echo "FAILED: no Angelscript source. Set ANGELS_SRC_DIR to a checkout, or"
        echo "        place a built archive at $ENGINE_LIB."
        echo "        The engine is not vendored, so this step cannot be skipped -"
        echo "        a test without it measures the file, not the registration."
        exit 1
    fi
fi

echo "engine: building from $ENGINE_DIR"
# The version macros are not cosmetic and not optional. AsScriptEngine's factory
# compares the caller's major.minor against the library's and returns 0 when
# they differ; without these the library reports its own default, the plugin's
# 23900 header disagrees with it, and the failure arrives as a null engine
# pointer rather than as a version message. They match
# third_party/angelscript/angelscript.h - the number in that file is the one
# this must equal, and it is the reason a stock 2.38 release cannot be used.
COMPILE=(-std=c++17 -O1 -fPIC -w
         -DANGELSCRIPT_VERSION_MAJOR=2 -DANGELSCRIPT_VERSION_MINOR=39
         -DANGELSCRIPT_VERSION_BUILD=0
         -I"$ENGINE_DIR/sdk/angelscript/include")

# Named rather than globbed into one command line. A hundred files on one line
# exceeds the argument limit on some platforms, and a partial compile is a
# partial archive that links and then misbehaves at run time. Every object that
# comes from this loop is accounted for below, and so is every source that did
# not produce one.
BUILT=0
SKIPPED=0
for src in "$ENGINE_DIR"/sdk/angelscript/source/*.cpp; do
    case "$(basename "$src")" in
        # Platform backends for CPUs that cannot be this machine. They
        # #error when compiled off their own target, so they are named here
        # rather than discovered by a failed compile.
        as_callfunc_xenon.cpp|as_callfunc_ppc.cpp|as_callfunc_ppc_64.cpp)
            SKIPPED=$((SKIPPED + 1))
            continue
            ;;
    esac
    "${CXX:-c++}" "${COMPILE[@]}" -c "$src" -o "$(basename "${src%.cpp}").o" || {
        echo "FAILED: $(basename "$src") did not compile"
        exit 1
    }
    BUILT=$((BUILT + 1))
done
echo "engine: compiled $BUILT objects, skipped $SKIPPED platform backend(s)"
# The archive is assembled with ar rather than left as an object list so the
# caller can hand one path to the linker. rm first: ar appends to an existing
# archive, so a second run without this would quietly link the previous build's
# objects together with the new ones.
rm -f "$ENGINE_LIB"
ar rcs "$ENGINE_LIB" ./*.o
rm -f ./*.o

if [ ! -s "$ENGINE_LIB" ]; then
    echo "FAILED: the engine compiled but produced no archive"
    exit 1
fi
echo "engine: $ENGINE_LIB ($(wc -c < "$ENGINE_LIB") bytes)"
echo "ENGINE_LIB=$ENGINE_LIB"
