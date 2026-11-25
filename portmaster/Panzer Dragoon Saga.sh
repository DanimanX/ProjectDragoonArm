#!/bin/bash
# Panzer Dragoon Saga — ProjectDragoonArm launcher
# Anbernic RG34XX (Allwinner H700 / Mali-G31) and compatible AArch64 handhelds.
#
# Works with and without PortMaster: on a plain Knulli/Batocera ports folder the
# script resolves its own directory, so $directory (which only PortMaster's
# control.txt defines) is not required.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# --- resolve where we are --------------------------------------------------
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$SELF_DIR/panzerdragoon"
if [ ! -d "$GAMEDIR" ] && [ -n "${directory:-}" ]; then
    GAMEDIR="/$directory/panzerdragoon"
fi

# --- PortMaster (optional) -------------------------------------------------
controlfolder=""
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -n "${directory:-}" ] && [ -d "/$directory/PortMaster" ]; then controlfolder="/$directory/PortMaster"
fi

if [ -f "${controlfolder}/control.txt" ]; then
    # shellcheck disable=SC1090
    . "${controlfolder}/control.txt"
    # shellcheck disable=SC1090
    [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && . "${controlfolder}/mod_${CFW_NAME}.txt"
    get_controls
fi

cd "$GAMEDIR" || exit 1

# Keep every run in log.txt (the engine is chatty and the device has no console).
{
    echo "=== Panzer Dragoon Saga (Azel engine / ARM64 / GLES3) ==="
    echo "Date: $(date)"
    echo "Device: ${DEVICE_NAME:-unknown}  CFW: ${CFW_NAME:-unknown}  Arch: $(uname -m)"
} >> "$GAMEDIR/log.txt"
exec >> "$GAMEDIR/log.txt" 2>&1

# --- data ------------------------------------------------------------------
DATA="$GAMEDIR/data/disc1"
if [ ! -f "$DATA/1ST_READ.PRG" ]; then
    echo "No disc data found in $DATA"
    echo "See PUT-YOUR-DISC-HERE.txt"
    exit 1
fi

# --- libraries -------------------------------------------------------------
# The firmware on this device ships SDL2 only, so the port carries its own
# SDL3 in libs.aarch64. That directory must come first.
export LD_LIBRARY_PATH="$GAMEDIR/libs.aarch64:${LD_LIBRARY_PATH:-}"

# Ports are launched by EmulationStation with no display server. On this CFW the
# bundled SDL3 has only the dummy/offscreen drivers, so it falls back on its own
# and the port takes GLES from libmali's EGL over /dev/fb0 (see
# platform/arm64/renderer_gles.cpp). Do not force SDL_VIDEODRIVER here: on
# handhelds whose firmware SDL3 does have KMSDRM, that path should win.

# --- gamepad mapping -------------------------------------------------------
# Measured on the device by correlating raw evdev with SDL's joystick button
# indices, one press at a time:
#
#   BTN_SOUTH(304)->b0  BTN_EAST(305)->b1  BTN_C(306)->b2  BTN_NORTH(307)->b3
#   BTN_WEST(308)->b4   KEY_309->b5        BTN_TL(310)->b6 BTN_TR(311)->b7
#   BTN_TL2(312)->b8    BTN_SELECT(314)->b9  BTN_START(315)->b10
#   axes a0 ABS_RX, a1 ABS_RY (stick), a2 ABS_RZ (R2 analog), hat h0 (d-pad)
#
# SDL's Linux driver numbers joystick buttons only from the BTN_* range, so the
# pad's stray KEY_ESC/volume codes are not part of the sequence. SDL's
# autodetected mapping already had every button right; what it lacked was the
# stick, because this pad reports RX/RY and the auto-mapping only bound
# rightx/righty. This entry is that mapping plus leftx/lefty.
PD_PAD_MAP="19006c08010000000100000000010000,Anbernic RG34XX Controller,a:b0,b:b1,x:b4,y:b3,back:b9,start:b10,leftshoulder:b6,rightshoulder:b7,lefttrigger:b8,righttrigger:a2,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,platform:Linux,"
export SDL_GAMECONTROLLERCONFIG="$PD_PAD_MAP"

# --- audio: unmute the internal speaker -------------------------------------
# Measured on the RG34XX: Knulli boots the sunxi codec with "SPK Switch" off
# (LINEOUT and the OutputL/R DAC mixers are on). The port can render audio into
# PipeWire perfectly — captured from the sink monitor at peak 18056 / RMS ~3000
# — and the speaker still stays silent because that switch is off.
#
# Enable the playback switches for the duration of the session and put back
# exactly what we found. Everything is best-effort: on a CFW that already has
# this right, nothing changes.
PD_AUDIO_RESTORE=""
pd_audio_on() {
    local card ctl numid name old
    for card in $(sed -n 's/^ *\([0-9][0-9]*\) \[.*/\1/p' /proc/asound/cards 2>/dev/null); do
        for name in "SPK Switch" "LINEOUT Switch"; do
            ctl=$(amixer -c "$card" controls 2>/dev/null \
                  | grep -F "name='$name'" | head -1) || continue
            numid=$(printf '%s\n' "$ctl" | sed -n 's/^numid=\([0-9][0-9]*\).*/\1/p')
            [ -n "$numid" ] || continue
            old=$(amixer -c "$card" cget numid="$numid" 2>/dev/null \
                  | sed -n 's/^ *: values=//p' | head -1)
            PD_AUDIO_RESTORE="$PD_AUDIO_RESTORE $card:$numid:${old:-unknown}"
            [ "$old" = "on" ] || amixer -c "$card" cset numid="$numid" on >/dev/null 2>&1
        done
    done
    [ -n "$PD_AUDIO_RESTORE" ] && echo "audio: switches" "$PD_AUDIO_RESTORE"
}

pd_audio_restore() {
    local entry card numid old
    [ -n "$PD_AUDIO_RESTORE" ] || return 0
    for entry in $PD_AUDIO_RESTORE; do
        card=${entry%%:*}; entry=${entry#*:}
        numid=${entry%%:*}; old=${entry#*:}
        case "$old" in
            on|off) amixer -c "$card" cset numid="$numid" "$old" >/dev/null 2>&1 ;;
        esac
    done
}

# --- performance -----------------------------------------------------------
# The H700 defaults to schedutil and the Mali devfreq only exposes
# simple_ondemand (so writing "performance" to governor silently fails and the
# GPU idles at 240 MHz instead of 696 MHz). Raise min_freq to the highest
# available frequency for the session and restore it on exit.
ORIG_GOV=""
if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$g" 2>/dev/null || true
    done
fi
ORIG_GPU_MIN=""
GPU_DEVFREQ="/sys/class/devfreq/gpu"
if [ -w "$GPU_DEVFREQ/min_freq" ] && [ -r "$GPU_DEVFREQ/available_frequencies" ]; then
    ORIG_GPU_MIN=$(cat "$GPU_DEVFREQ/min_freq" 2>/dev/null || true)
    GPU_MAX=$(tr ' ' '\n' < "$GPU_DEVFREQ/available_frequencies" | awk 'NF && $1+0 > m { m = $1+0 } END { print m }')
    if [ -n "$GPU_MAX" ] && [ "$GPU_MAX" -gt 0 ] 2>/dev/null; then
        echo "$GPU_MAX" > "$GPU_DEVFREQ/min_freq" 2>/dev/null || true
        echo "gpu: min_freq -> $(cat "$GPU_DEVFREQ/cur_freq" 2>/dev/null || echo "$GPU_MAX") Hz"
    fi
else
    for dev in /sys/class/devfreq/*/governor; do
        [ -w "$dev" ] && echo performance > "$dev" 2>/dev/null || true
    done
fi

# FAT/exFAT copies lose the executable bit.
chmod +x "$GAMEDIR/panzerdragoon.aarch64" 2>/dev/null

export PD_FULLSCREEN=1
export PD_WIDTH=720
export PD_HEIGHT=480

# The SCSP emulation runs inside the SDL audio callback (68k + 32-slot mixer per
# sample) while the main thread renders. Its default device buffer is small, and
# underruns are audible as choppy sound on this SoC, so ask ALSA for a bigger one.
export SDL_AUDIO_DEVICE_SAMPLE_FRAMES="${SDL_AUDIO_DEVICE_SAMPLE_FRAMES:-2048}"

if [ -f "${controlfolder}/pm_platform_helper.sh" ]; then
    # shellcheck disable=SC1090
    . "${controlfolder}/pm_platform_helper.sh"
    pm_platform_helper "$GAMEDIR/panzerdragoon.aarch64" >/dev/null 2>&1
fi

pd_audio_on

# --- stop EmulationStation from drawing over the game ----------------------
# On Knulli ES keeps running and holds /dev/fb0 open; the port draws to the
# same framebuffer, so without this the menu is composited on top of the game
# (verified on the device: ES has /dev/fb0 open and repaints over the picture).
# SIGSTOP it for the session and always bring it back, including on failure.
ES_PID="$(pidof emulationstation 2>/dev/null | awk '{print $1}')"
pd_es_resume() {
    if [ -n "${ES_PID:-}" ] && kill -0 "$ES_PID" 2>/dev/null; then
        kill -CONT "$ES_PID" 2>/dev/null || true
    fi
}
# Save what ES is showing before we take the framebuffer over, and put it back
# afterwards. ES repaints only changed regions, so leaving our last game frame
# behind makes it draw a half-broken menu (verified: it restored only one half).
# Restoring the exact bytes keeps its dirty tracking consistent with the screen.
PD_ES_FRAME="$GAMEDIR/.es_frame.bin"
PD_FB_BYTES=$((720 * 960 * 4))
if [ -r /dev/fb0 ]; then
    dd if=/dev/fb0 of="$PD_ES_FRAME" bs=4096 count=$((PD_FB_BYTES / 4096)) 2>/dev/null || rm -f "$PD_ES_FRAME"
fi

if [ -n "$ES_PID" ]; then
    kill -STOP "$ES_PID" 2>/dev/null && echo "ES ($ES_PID) suspended for the session"
fi
pd_perf_restore() {
    if [ -n "${ORIG_GPU_MIN:-}" ] && [ -w "$GPU_DEVFREQ/min_freq" ]; then
        echo "$ORIG_GPU_MIN" > "$GPU_DEVFREQ/min_freq" 2>/dev/null || true
    fi
    if [ -n "${ORIG_GOV:-}" ]; then
        for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            echo "$ORIG_GOV" > "$g" 2>/dev/null || true
        done
    fi
}
pd_cleanup() {
    pd_es_resume
    pd_perf_restore
}
trap pd_cleanup EXIT INT TERM HUP

# --- run -------------------------------------------------------------------
cd "$DATA" || exit 1
"$GAMEDIR/panzerdragoon.aarch64"
RC=$?

# --- teardown --------------------------------------------------------------
# Put ES's frame back before letting it run again.
if [ -s "$PD_ES_FRAME" ] && [ -w /dev/fb0 ]; then
    dd if="$PD_ES_FRAME" of=/dev/fb0 bs=4096 conv=notrunc 2>/dev/null || true
fi
pd_es_resume
pd_audio_restore
pd_perf_restore
if command -v pm_finish >/dev/null 2>&1; then pm_finish >/dev/null 2>&1 || true; fi
exit $RC
