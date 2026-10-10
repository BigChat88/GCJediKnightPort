# Build environment for the OpenJKDF2 GameCube port.
#
#   docker build -t openjkdf2-gamecube packaging/gamecube
#   docker run --rm -v "$PWD:/src" -w /src openjkdf2-gamecube ./build_gamecube.sh
#
# devkitpro/devkitppc ships devkitPPC, libogc, libfat and the GameCube portlibs;
# the engine's globals.h/globals.c are generated with cogapp, which needs a venv.
FROM devkitpro/devkitppc:latest

RUN apt-get update \
 && apt-get install -y --no-install-recommends python3-venv python3-pip git \
 && rm -rf /var/lib/apt/lists/*
