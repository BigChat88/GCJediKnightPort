# Headless Dolphin for automated boot tests of the GameCube build.
#
#   docker build -t openjkdf2-dolphin packaging/gamecube/test
#   docker run --rm --shm-size=1g -v "<dir with dol+iso>:/work" openjkdf2-dolphin /work/openjkdf2.dol /work/openjkdf2.iso 30
# Default trixie (Dolphin 2503: boots a DOL directly, used for the libogc core).
# For the libogc2 core build it from sid (Dolphin 2609; 2503 hangs libogc2 builds
# at boot, it doesn't mirror the EXI registers libogc2 polls; 2609's nogui only
# boots disc images, see dev.sh):
#   docker build --build-arg DEBIAN=sid -t openjkdf2-dolphin-sid packaging/gamecube/test
ARG DEBIAN=trixie
FROM debian:${DEBIAN}

RUN apt-get update \
 && apt-get install -y --no-install-recommends dolphin-emu coreutils procps xvfb xauth libgl1-mesa-dri \
 && rm -rf /var/lib/apt/lists/*

COPY run_test.sh /usr/local/bin/run_test.sh
RUN chmod +x /usr/local/bin/run_test.sh
ENTRYPOINT ["/usr/local/bin/run_test.sh"]
