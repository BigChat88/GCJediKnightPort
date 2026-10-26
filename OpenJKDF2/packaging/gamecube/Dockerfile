# Build environment for the OpenJKDF2 GameCube port, on libogc2 (Extrems' fork of
# libogc: SD cards through libdvm with exFAT, more network adapters, many fixes).
#
#   docker build -t openjkdf2-gamecube packaging/gamecube
#   docker run --rm -v "$PWD:/src" -w /src openjkdf2-gamecube ./build_gamecube.sh
#
# The base image has devkitPPC, libogc2 (the default OGC platform library for
# CMake there) and its portlibs; the engine's globals.h/globals.c are generated
# with cogapp, which needs a venv.
FROM ghcr.io/extremscorner/libogc2:latest

RUN apt-get update \
 && apt-get install -y --no-install-recommends python3-venv python3-pip git \
 && rm -rf /var/lib/apt/lists/*
