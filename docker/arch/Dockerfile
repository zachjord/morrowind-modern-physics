# Build environment for this fork on Arch Linux (the same libraries as an Arch desktop, so what it builds runs
# there). See compose.yaml at the root of the repository, and docker/arch/README.md.
FROM archlinux:latest

# Toolchain and speed-ups: ninja, ccache (compiler cache, kept in a volume), mold (a much faster linker).
# Libraries: what OpenMW needs from the system (OSG, MyGUI, recastnavigation...), Qt 6 for the launcher and
# installation wizard, gtest for the unit tests. Not Bullet: Arch's is single precision, OpenMW builds its own.
RUN pacman -Syu --noconfirm --needed \
        base-devel git cmake ninja ccache mold \
        openscenegraph mygui recastnavigation \
        sdl2-compat openal ffmpeg boost boost-libs luajit yaml-cpp sqlite icu lz4 unshield tinyxml \
        qt6-base qt6-tools qt6-svg \
        gtest \
    && pacman -Scc --noconfirm

ENV CCACHE_DIR=/ccache \
    CCACHE_MAXSIZE=15G \
    CCACHE_COMPRESS=1 \
    CCACHE_COMPRESSLEVEL=1 \
    CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime \
    CCACHE_BASEDIR=/src

COPY build.sh /usr/local/bin/build-openmw
RUN chmod +x /usr/local/bin/build-openmw

WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-openmw"]
