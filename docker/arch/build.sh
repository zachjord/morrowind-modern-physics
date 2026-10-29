#!/usr/bin/env bash
# Gets the source (cloned, or the checkout mounted at /src), builds it with ninja + ccache + mold, optionally runs the
# unit tests, and puts a runnable copy in /out. Settings come from the environment (see compose.yaml).
set -euo pipefail

SOURCE="${SOURCE:-clone}"                 # clone: fetch REPO_URL/BRANCH into /src; local: build /src as it is
REPO_URL="${REPO_URL:-https://github.com/zachjord/morrowind-modern-physics.git}"
BRANCH="${BRANCH:-physics}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
JOBS="${JOBS:-$(nproc)}"
BUILD_TESTS="${BUILD_TESTS:-ON}"
RUN_TESTS="${RUN_TESTS:-1}"
BUILD_LAUNCHER="${BUILD_LAUNCHER:-ON}"
BUILD_OPENCS="${BUILD_OPENCS:-OFF}"
EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS:-}"
HOST_UID="${HOST_UID:-1000}"
HOST_GID="${HOST_GID:-1000}"
BUILD_DIR=/build
OUT_DIR=/out

log() { printf '\n==> %s\n' "$*"; }

if [[ "$SOURCE" == "clone" ]]; then
    if [[ -d /src/.git ]]; then
        log "Updating /src to $REPO_URL $BRANCH"
        git -C /src remote set-url origin "$REPO_URL"
        git -C /src fetch --depth 50 origin "$BRANCH"
        git -C /src checkout -B "$BRANCH" FETCH_HEAD
        git -C /src reset --hard FETCH_HEAD
        git -C /src clean -fdx
    else
        log "Cloning $REPO_URL $BRANCH"
        git clone --depth 50 --branch "$BRANCH" "$REPO_URL" /src
    fi
elif [[ ! -f /src/CMakeLists.txt ]]; then
    echo "SOURCE=local, but /src has no CMakeLists.txt: mount the checkout there (see compose.yaml)." >&2
    exit 1
fi
# The checkout may belong to someone else (a mounted one does).
git config --global --add safe.directory /src
REVISION="$(git -C /src rev-parse --short HEAD 2>/dev/null || echo unknown)"

log "Configuring ($BUILD_TYPE, revision $REVISION)"
# shellcheck disable=SC2086 # EXTRA_CMAKE_ARGS is meant to split into arguments
cmake -S /src -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_LINKER_TYPE=MOLD \
    -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON \
    -DOPENMW_USE_SYSTEM_GOOGLETEST=ON \
    -DBUILD_LAUNCHER="$BUILD_LAUNCHER" \
    -DBUILD_WIZARD="$BUILD_LAUNCHER" \
    -DBUILD_OPENCS="$BUILD_OPENCS" \
    -DBUILD_OPENMW_TESTS="$BUILD_TESTS" \
    -DBUILD_COMPONENTS_TESTS="$BUILD_TESTS" \
    -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_NIFTEST=OFF -DBUILD_ESSIMPORTER=OFF \
    -DBUILD_NAVMESHTOOL=OFF -DBUILD_BULLETOBJECTTOOL=OFF \
    $EXTRA_CMAKE_ARGS

ccache --zero-stats >/dev/null
log "Building with $JOBS jobs"
cmake --build "$BUILD_DIR" -j "$JOBS"
log "Compiler cache"
ccache --show-stats

mkdir -p "$OUT_DIR"
TEST_STATUS=0
if [[ "$BUILD_TESTS" == "ON" && "$RUN_TESTS" == "1" ]]; then
    log "Unit tests (logs in out/tests-$REVISION)"
    mkdir -p "$OUT_DIR/tests-$REVISION"
    for tests in openmw-tests components-tests; do
        if [[ -x "$BUILD_DIR/$tests" ]]; then
            (cd "$BUILD_DIR" && "./$tests" --gtest_output="xml:$OUT_DIR/tests-$REVISION/$tests.xml") \
                2>&1 | tee "$OUT_DIR/tests-$REVISION/$tests.log" || TEST_STATUS=1
        fi
    done
fi

# A copy to run: the programs, and what they look for next to them (resources, settings defaults, openmw.cfg).
TARGET="$OUT_DIR/openmw-$REVISION"
log "Copying the game to $TARGET"
rm -rf "$TARGET"
mkdir -p "$TARGET"
for file in openmw openmw-launcher openmw-wizard openmw-iniimporter openmw-cs \
    openmw.cfg defaults.bin defaults-cs.bin gamecontrollerdb.txt; do
    if [[ -e "$BUILD_DIR/$file" ]]; then
        cp -a "$BUILD_DIR/$file" "$TARGET/"
    fi
done
cp -a "$BUILD_DIR/resources" "$TARGET/"
ln -sfn "openmw-$REVISION" "$OUT_DIR/openmw-latest"
chown -R "$HOST_UID:$HOST_GID" "$OUT_DIR"

log "Done: out/openmw-$REVISION (also out/openmw-latest). Run ./openmw-launcher or ./openmw from there."
if [[ "$TEST_STATUS" != 0 ]]; then
    echo "Some unit tests FAILED: see out/tests-$REVISION/*.log" >&2
    exit 2
fi
