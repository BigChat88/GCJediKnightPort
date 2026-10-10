#!/bin/bash
# run_test.sh <dol> [iso] [seconds]
# Boots the DOL in dolphin-emu-nogui (headless, software renderer), with the ISO
# inserted as the disc, for a fixed time. Prints the OSReport log; frame dumps
# (PNG) land in /work/frames when /work is mounted.
DOL="$1"; ISO="$2"; SECS="${3:-30}"
U=/tmp/dolphin-user
mkdir -p $U/Config
# Keep memory cards across runs when /work is mounted (save persistence tests).
if [ -d /work ]; then
    mkdir -p /work/.dolphin-gc
    ln -sfn /work/.dolphin-gc $U/GC
fi
cat > $U/Config/Dolphin.ini <<INI
[Core]
DefaultISO = $ISO
CPUThread = False
GFXBackend = Software Renderer
[DSP]
Backend = No Audio Output
[Interface]
UsePanicHandlers = False
[Movie]
DumpFrames = True
[Analytics]
PermissionAsked = True
INI
cat > $U/Config/GFX.ini <<INI
[Settings]
DumpFramesAsImages = True
INI
cat > $U/Config/Logger.ini <<INI
[Logs]
OSREPORT = True
OSREPORT_HLE = True
BOOT = True
[Options]
Verbosity = 4
WriteToFile = True
WriteToConsole = True
INI
Xvfb :99 -screen 0 1024x768x24 > /dev/null 2>&1 &
export DISPLAY=:99
sleep 1
# SIGINT lets Dolphin shut down cleanly and flush its log.
# DOLPHIN_ARGS: extra options, e.g. "-C Dolphin.Core.FastDiscSpeed=True"
timeout -s INT -k 15 "$SECS" /usr/games/dolphin-emu-nogui -u $U -p x11 $DOLPHIN_ARGS -e "$DOL" > /tmp/stdout.log 2>&1
echo "=== dolphin exit: $?"
grep -av "^ALSA\|DolReader" /tmp/stdout.log | cut -c1-300
if [ -d /work ]; then
    rm -rf /work/frames; mkdir -p /work/frames
    cp $U/Dump/Frames/*.png /work/frames/ 2>/dev/null
    # With -C Dolphin.DSP.DumpAudio=True the DSP output lands here too.
    cp $U/Dump/Audio/*.wav /work/frames/ 2>/dev/null
    echo "=== frames: $(ls /work/frames | wc -l)"
fi
