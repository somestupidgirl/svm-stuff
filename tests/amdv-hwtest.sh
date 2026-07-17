#!/bin/bash
#
# amdv-hwtest.sh - low-risk AMD-V / AMDV-plugin probes for a Big Sur box.
#
# Runs on the TARGET x86_64 Hackintosh, which is not the machine this project
# is built on. It therefore assumes nothing about a source tree: no repo, no
# build/ directory, no make. Copy this script (and the svmcheck binary next to
# it) anywhere on the target and run it.
#
# Kexts are expected on the mounted EFI system partition, where OpenCore keeps
# them:  /Volumes/EFI/EFI/OC/Kexts
# Mount it first if needed:  sudo diskutil mount EFI
#
# Safe by default: the probes (CPUID, sysctl, kernel symbols, kextstat, log)
# are read-only. --load additionally kmutil-loads Lilu + AMDV, which is only
# useful to prove the kext validates - a Lilu plugin loaded post-boot is inert
# (see the warning it prints). Guest launch is a compile-time flag and is never
# enabled here, so no VMRUN is executed.
#
# Every run also writes a transcript to ~/amdv-hwtest-log-<timestamp>.txt.
#
# Usage:
#   ./amdv-hwtest.sh                 # read-only probes (use this after a reboot)
#   sudo ./amdv-hwtest.sh --load     # + kmutil load from the ESP
#   ./amdv-hwtest.sh --amdv /path/to/AMDV.kext --lilu /path/to/Lilu.kext
#
# Report back: the "summary: rev=... NP=... NRIPS=..." line, and whether AMDV
# logged "routed <symbol>" or "could not locate a VMX gate symbol".

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Where OpenCore keeps kexts on the target. This is the canonical location;
# everything else is a fallback.
OC_KEXTS="/Volumes/EFI/EFI/OC/Kexts"

SVMCHECK=""
AMDV_KEXT=""
LILU_KEXT=""
DO_LOAD=0
KERNEL="/System/Library/Kernels/kernel"

while [ $# -gt 0 ]; do
  case "$1" in
    --load)  DO_LOAD=1 ;;
    --amdv)  AMDV_KEXT="${2:-}"; shift ;;
    --lilu)  LILU_KEXT="${2:-}"; shift ;;
    -h|--help) grep '^#' "$0" | cut -c3-; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

# --------------------------------------------------------------------------
# Transcript. Under sudo, $HOME is root's, so resolve the invoking user's home
# and hand them ownership - otherwise the log lands in /var/root and they
# cannot read it.
if [ -n "${SUDO_USER:-}" ]; then
  USER_HOME="$(eval echo "~$SUDO_USER")"
else
  USER_HOME="$HOME"
fi
[ -d "$USER_HOME" ] || USER_HOME="$HOME"
LOGFILE="$USER_HOME/amdv-hwtest-log-$(date '+%Y%m%d-%H%M%S').txt"

hr()   { printf '\n=== %s ===\n' "$1"; }
note() { printf '  %s\n' "$*"; }

# --------------------------------------------------------------------------
# Locate the svmcheck binary. It is cross-compiled for x86_64 elsewhere and
# copied here, so look next to the script and on PATH. Only try to build it if
# a Makefile is actually adjacent (i.e. we are on the dev machine, in-tree).
find_svmcheck() {
  local c
  for c in "$SCRIPT_DIR/svmcheck" "$SCRIPT_DIR/../build/svmcheck"; do
    if [ -x "$c" ]; then SVMCHECK="$c"; return 0; fi
  done
  if command -v svmcheck >/dev/null 2>&1; then
    SVMCHECK="$(command -v svmcheck)"; return 0
  fi
  if [ -f "$SCRIPT_DIR/Makefile" ] && command -v make >/dev/null 2>&1; then
    note "svmcheck not built; building in $SCRIPT_DIR"
    make -C "$SCRIPT_DIR" >/dev/null 2>&1
    [ -x "$SCRIPT_DIR/svmcheck" ] && { SVMCHECK="$SCRIPT_DIR/svmcheck"; return 0; }
  fi
  return 1
}

# --------------------------------------------------------------------------
# Resolve a kext, preferring the ESP. Reports what was tried; a bare
# "missing/invalid" is useless when the ESP simply is not mounted.
RESOLVED=""
resolve_kext() {
  local label="$1" want="$2" name c
  RESOLVED=""
  name="$label.kext"
  [ -n "$want" ] && case "$want" in *.kext) name="$(basename "$want")" ;; esac

  if [ -n "$want" ] && [ -d "$want" ]; then RESOLVED="$want"; return 0; fi
  [ -n "$want" ] && note "$label: '$want' does not exist (or is not a directory)"

  for c in "$OC_KEXTS/$name" "$SCRIPT_DIR/$name" "/Library/Extensions/$name"; do
    if [ -d "$c" ]; then note "$label: using $c"; RESOLVED="$c"; return 0; fi
  done

  note "$label: not found. searched:"
  note "    $OC_KEXTS/$name"
  note "    $SCRIPT_DIR/$name"
  note "    /Library/Extensions/$name"
  if [ ! -d /Volumes/EFI ]; then
    note "  the EFI system partition is NOT mounted at /Volumes/EFI."
    note "  OpenCore kexts live there; mount it with:  sudo diskutil mount EFI"
  fi
  return 1
}

# --------------------------------------------------------------------------
# kmutil refuses any bundle not owned root:wheel. Kexts on the ESP sit on
# FAT32, which carries no ownership at all, so always load from a root-owned
# staging copy rather than trying to chown the original.
STAGE=""
stage_kext() {
  local src="$1" dst
  dst="$STAGE/$(basename "$src")"
  /bin/rm -rf "$dst"
  /bin/cp -R "$src" "$dst"   || return 1
  chown -R root:wheel "$dst" || return 1
  chmod -R 755 "$dst"        || return 1
  printf '%s' "$dst"
}

load_one() {
  local src="$1" k
  k="$(stage_kext "$src")" || { note "staging failed for $src"; return 1; }
  note "kmutil load -p $k"
  note "  (staged root:wheel from $src)"
  if ! kmutil load -p "$k" 2>&1 | sed 's/^/    /'; then
    note "load failed for $src (check SIP / csrutil and codesigning)"
    return 1
  fi
}

# --------------------------------------------------------------------------
# Residency + log: read-only, so they run on EVERY invocation. These are what
# answer "did the boot-injected plugin actually run?".
report_state() {
  hr "Loaded in kernel (kextstat)"
  local ks
  ks=$(kextstat 2>/dev/null | grep -iE "vit9696|hackintosh|lilu|amdv")
  if [ -n "$ks" ]; then
    echo "$ks" | sed 's/^/  /'
  else
    note "neither Lilu nor AMDV are resident."
    note "if you boot-injected via OpenCore, they did not load - check"
    note "config.plist -> Kernel -> Add (AMDV must be listed AFTER Lilu)."
  fi

  hr "AMDV log ($1)"
  # Lilu's SYSLOG format is "%s%10s: @ " (product, module), e.g.
  #   AMDV      amdv: @ SVM present: rev 1, ...
  # Keying on ": @ " avoids matching anything that merely contains "AMDV"
  # (sandbox denials naming a path, etc). process == "kernel" is required:
  # lilu_os_log goes via IOLog so plugin output is kernel[0], and it also
  # excludes `log` itself, which logs its own argv - and our argv contains the
  # very strings being matched.
  local out
  out=$(log show --style compact ${2:+--start "$2"} ${3:+--end "$3"} \
          --predicate 'process == "kernel" AND eventMessage CONTAINS ": @ " AND (eventMessage CONTAINS "AMDV" OR eventMessage CONTAINS "Lilu")' \
          2>/dev/null | grep -v "^Timestamp")
  if [ -n "$out" ]; then
    echo "$out" | sed 's/^/  /'
  else
    note "no AMDV/Lilu kernel log lines."
    note "expected if the plugin never ran (post-boot kmutil load), or if the"
    note "-amdvdbg / -liludbgall boot-args are not set."
  fi
}

# Bound the scan to this boot. kern.boottime prints:
#   { sec = 1783855099, usec = 605228 } Sun Jul 12 ...
# Anchor on "{ sec =": an unanchored ".*sec = " greedily matches the LATER
# "usec = ", giving a 1970 timestamp and a log scan that never finishes.
boot_sec() {
  sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ sec = \([0-9][0-9]*\).*/\1/p'
}
boot_time()       { local s; s=$(boot_sec); [ -n "$s" ] && date -r "$s" '+%Y-%m-%d %H:%M:%S' 2>/dev/null; }
# A Lilu plugin does all its work in early boot; without an end bound `log
# show` walks every chunk since boot (minutes of CPU on a long-uptime box).
boot_window_end() { local s; s=$(boot_sec); [ -n "$s" ] && date -r "$((s + 300))" '+%Y-%m-%d %H:%M:%S' 2>/dev/null; }

deploy_help() {
  note "To exercise the plugin it must be injected at BOOT: Lilu closes plugin"
  note "registration in early boot, so a post-boot kmutil load is inert."
  note "  1. copy AMDV.kext to $OC_KEXTS/"
  note "  2. config.plist -> Kernel -> Add: AMDV.kext, ordered AFTER Lilu.kext"
  note "  3. boot-args: -amdvdbg   (add -liludbgall for Lilu's own logging)"
  note "  4. reboot, then re-run this script (no --load needed)"
}

# ==========================================================================
main() {
  hr "Host"
  note "$(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
  note "machdep.cpu.brand: $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
  note "machdep.cpu.vendor: $(sysctl -n machdep.cpu.vendor 2>/dev/null)"

  hr "SVM capability (CPUID 0x8000000A)"
  if find_svmcheck; then
    "$SVMCHECK" 2>&1 | sed 's/^/  /'
    local rc=${PIPESTATUS[0]}
    [ "$rc" -ne 0 ] && note "(svmcheck exited $rc)"
  else
    note "svmcheck not found next to this script or on PATH."
    note "build it on the dev machine (make tests) and copy it to $SCRIPT_DIR/"
  fi

  hr "Hypervisor support (before)"
  local hv
  if hv=$(sysctl -n kern.hv_support 2>/dev/null); then
    note "kern.hv_support = $hv"
  else
    note "kern.hv_support = <absent>  (expected on stock AMD; the gate hook aims to change this)"
  fi

  hr "Kernel VMX-gate symbols"
  if [ -r "$KERNEL" ]; then
    local matches
    matches=$(nm -arch x86_64 "$KERNEL" 2>/dev/null | grep -iE 'vmx_is_available|vmx_hv_support|hv_support|hv_get_support')
    if [ -n "$matches" ]; then
      echo "$matches" | sed 's/^/  /'
      note "^ compare against kVmxAvailSymbols[] in src/kern_hv_amd.cpp"
    else
      note "no matching symbols in on-disk kernel (may be stripped)"
    fi
  else
    note "kernel not readable at $KERNEL"
  fi

  if [ "$DO_LOAD" -eq 0 ]; then
    report_state "first 5 min of this boot" "$(boot_time)" "$(boot_window_end)"
    hr "Done (read-only)"
    deploy_help
    note ""
    note "--load only verifies the kext validates and loads; it cannot run it."
    return 0
  fi

  hr "Loading kexts"
  note "WARNING: AMDV is a Lilu plugin. Lilu closes its plugin-registration"
  note "window during early boot (finaliseRequests() at kPEEnableScreen), so a"
  note "plugin loaded post-boot gets Error::TooLate from requestAccess() and does"
  note "nothing - it will load, start, and be inert. This path only proves the"
  note "kext VALIDATES and LOADS."
  note ""
  if [ "$(id -u)" -ne 0 ]; then note "must be root to load kexts; re-run with sudo"; return 1; fi

  resolve_kext Lilu "$LILU_KEXT" || return 1
  LILU_KEXT="$RESOLVED"
  resolve_kext AMDV "$AMDV_KEXT" || return 1
  AMDV_KEXT="$RESOLVED"

  STAGE="$(mktemp -d /tmp/amdv-stage.XXXXXX)" || return 1
  chown root:wheel "$STAGE" 2>/dev/null
  chmod 755 "$STAGE"

  local since; since=$(date '+%Y-%m-%d %H:%M:%S')
  load_one "$LILU_KEXT" || return 1
  load_one "$AMDV_KEXT" || return 1
  sleep 1

  report_state "this load" "$since"

  hr "Hypervisor support (after)"
  if hv=$(sysctl -n kern.hv_support 2>/dev/null); then
    note "kern.hv_support = $hv"
  else
    note "kern.hv_support = <absent>"
  fi

  hr "Done"
  note "A post-boot load cannot run the plugin; absent AMDV log lines above are"
  note "expected, not a failure."
  deploy_help
}

main 2>&1 | tee "$LOGFILE"

# Hand the transcript to the invoking user, not root.
[ -n "${SUDO_USER:-}" ] && chown "$SUDO_USER" "$LOGFILE" 2>/dev/null
printf '\nwrote %s\n' "$LOGFILE"
