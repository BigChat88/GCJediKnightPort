# Headless Dolphin for automated boot tests of the GameCube build.
#
#   docker build -t openjkdf2-dolphin packaging/gamecube/test
#   docker run --rm --shm-size=1g -v "<dir with dol+iso>:/work" openjkdf2-dolphin /work/openjkdf2.dol /work/openjkdf2.iso 30
FROM debian:trixie

RUN apt-get update \
 && apt-get install -y --no-install-recommends dolphin-emu coreutils procps xvfb xauth libgl1-mesa-dri \
 && rm -rf /var/lib/apt/lists/*

COPY run_test.sh /usr/local/bin/run_test.sh
RUN chmod +x /usr/local/bin/run_test.sh
ENTRYPOINT ["/usr/local/bin/run_test.sh"]
