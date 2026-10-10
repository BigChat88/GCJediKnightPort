#!/bin/sh
# build.sh -- GameCube disc image build (Linux/macOS). See build.cmd / README.md.
#   ./build.sh                 # everything found in gamedata/
#   ./build.sh --only mots     # options go straight to tools/pack_iso.py
set -e
cd "$(dirname "$0")"
PY="${PYTHON:-python3}"
"$PY" -m pip install --quiet --disable-pip-version-check -r requirements.txt || \
    echo "warning: could not install numpy/Pillow (pip); install them yourself if the build fails"
exec "$PY" tools/pack_iso.py "$@"
