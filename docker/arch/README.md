# Building this fork with Docker (Arch Linux)

`compose.yaml` at the root of the repository builds the game in an Arch Linux container. The libraries are Arch's
own, so the result runs on an up-to-date Arch desktop. Other distributions would need the same library versions.

## Build

From the repository root:

```sh
# The physics branch from GitHub: clones it (or updates the clone), builds, runs the unit tests.
docker compose run --rm build

# This checkout as it is, including uncommitted changes (mounted read-only).
docker compose run --rm build-local
```

The first run builds the image (a few minutes) and compiles everything (the slow part). Later runs reuse:

- **ccache** (volume `ccache`, up to 15 GB): unchanged files aren't recompiled, even after `docker compose down`.
- **the build tree** (volumes `build-clone`, `build-local`): ninja only redoes what changed.
- **mold** as the linker, much faster than the default for the big `openmw` binary.

## Output

`out/openmw-<revision>/` (and `out/openmw-latest`, a link to the newest): `openmw`, `openmw-launcher`,
`openmw-wizard`, `openmw-iniimporter`, `resources/`, and the default config. Run `./openmw-launcher` from there the
first time to point it at your Morrowind data (or reuse an existing `~/.config/openmw`).

Unit-test logs and JUnit XML go to `out/tests-<revision>/`. If any test fails, the command exits with status 2
after copying the build, so you can still play it.

## Options

Environment variables, e.g. `JOBS=8 BRANCH=my-branch docker compose run --rm build`:

| Variable | Default | |
|---|---|---|
| `BRANCH` | `physics` | branch to clone (`build` only) |
| `REPO_URL` | this fork on GitHub | |
| `BUILD_TYPE` | `RelWithDebInfo` | or `Release`, `Debug` |
| `JOBS` | all cores | lower it if the machine runs out of memory (about 1.5–2 GB per job at peak) |
| `BUILD_TESTS` / `RUN_TESTS` | `ON` / `1` | `OFF` / `0` to skip them |
| `BUILD_LAUNCHER` | `ON` | the launcher and wizard (Qt) |
| `BUILD_OPENCS` | `OFF` | the Construction Set (slow to build) |
| `EXTRA_CMAKE_ARGS` | | anything else for CMake |
| `HOST_UID` / `HOST_GID` | `1000` | who owns `out/` afterwards: use `HOST_UID=$(id -u) HOST_GID=$(id -g)` |

To start over: `docker compose down -v` (deletes the cache and build volumes) and `docker compose build --no-cache`
(to refresh the Arch packages; worth doing when your desktop's packages have moved on).
