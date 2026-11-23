#!/bin/bash
# Panzer Dragoon Saga — ProjectDragoonArm launcher (Anbernic RG34XX / PortMaster)
#
# The firmware supplies libSDL3 and the Mali EGL/GLES drivers; everything else
# ships beside the binary.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
controlfolder="/$directory/PortMaster"
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then controlfolder="/roms/ports/PortMaster"
fi
if [ -f "${controlfolder}/control.txt" ]; then
    # shellcheck disable=SC1090
    . "${controlfolder}/control.txt"
    # shellcheck disable=SC1090
    [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && . "${controlfolder}/mod_${CFW_NAME}.txt"
    get_controls
fi

GAMEDIR="/$directory/panzerdragoon"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# The data tree the engine expects: one flat directory per disc.
DATA="$GAMEDIR/data/disc1"
if [ ! -f "$DATA/1ST_READ.PRG" ]; then
    echo "No disc data found in $DATA"
    echo "See PUT-YOUR-DISC-HERE.txt"
    sleep 5
    exit 1
fi

# SDL3 is a firmware library on most CFWs; if it is missing, the port's own
# libs.aarch64 directory is searched first so users can drop it there.
export LD_LIBRARY_PATH="$GAMEDIR/libs.aarch64:${LD_LIBRARY_PATH:-}"

# Performance: the H700 defaults to schedutil and the Mali devfreq to a low
# clock, which costs 10-20 ms on the first seconds of audio and looks like a
# rendering bug on the first scene.
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$cpu" ] && echo performance > "$cpu" 2>/dev/null
done
for dev in /sys/class/devfreq/*/governor; do
    [ -w "$dev" ] && echo performance > "$dev" 2>/dev/null
done

export PD_FULLSCREEN=1
export PD_WIDTH=720
export PD_HEIGHT=480

if [ -f "${controlfolder}/pm_platform_helper.sh" ]; then
    . "${controlfolder}/pm_platform_helper.sh"
    pm_platform_helper "$GAMEDIR/panzerdragoon.aarch64" >/dev/null 2>&1
fi

# SMB/FAT copies lose the executable bit; restore it silently.
chmod +x "$GAMEDIR/panzerdragoon.aarch64" 2>/dev/null

cd "$DATA"
"$GAMEDIR/panzerdragoon.aarch64"
exit $?
