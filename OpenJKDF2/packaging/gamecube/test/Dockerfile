# Headless Dolphin for automated boot tests of the GameCube build.
#
#   docker build -t openjkdf2-dolphin packaging/gamecube/test
#   docker run --rm --shm-size=1g -v "<dir with the iso>:/work" openjkdf2-dolphin /work/openjkdf2.iso /work/openjkdf2.iso 30
# Debian sid: Dolphin 2609. (Trixie's 2503 hangs libogc2 programs at boot -- it
# doesn't mirror the EXI registers libogc2 polls -- and 2609's nogui only boots
# disc images, so the DOL is packed into an image first; see dev.sh.)
ARG DEBIAN=sid
FROM debian:${DEBIAN}

RUN apt-get update \
 && apt-get install -y --no-install-recommends dolphin-emu coreutils procps xvfb xauth libgl1-mesa-dri \
 && rm -rf /var/lib/apt/lists/*

COPY run_test.sh /usr/local/bin/run_test.sh
RUN chmod +x /usr/local/bin/run_test.sh
ENTRYPOINT ["/usr/local/bin/run_test.sh"]
