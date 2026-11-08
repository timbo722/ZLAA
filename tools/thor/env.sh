# env.sh: sourced by the scripts here. What differs between machines comes from local.sh next to it (not in git;
# copy local.sh.example): LAA_ADB (default adb), LAA_ADB_SERIAL (the device, when several are connected) and
# LAA_HOST_DIR (default ~/laar-build/thor-files: env/, transfer/, game/, apk/ for files on their way to or from the
# device). On WSL with the Windows adb.exe (the only one that sees USB devices) LAA_HOST_DIR must be on a Windows drive
# (/mnt/c/...): adb.exe is then given Windows paths.
H=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ -f "$H/local.sh" ] && . "$H/local.sh"
thor_adb() { "${LAA_ADB:-adb}" ${LAA_ADB_SERIAL:+-s "$LAA_ADB_SERIAL"} "$@"; }
W=thor_adb
A=/storage/emulated/0/Android/data/io.github.zlaa/files      # the app's folder on the device
X=${LAA_HOST_DIR:-$HOME/laar-build/thor-files}; mkdir -p "$X"/env "$X"/transfer
case ${LAA_ADB:-adb} in *.exe) XW=$(wslpath -w "$X");; *) XW=;; esac
xa() { if [ -n "$XW" ]; then printf '%s\\%s' "$XW" "${1//\//\\}"; else printf '%s/%s' "$X" "$1"; fi; }   # $X/<path> as adb sees it
