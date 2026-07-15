#!/bin/bash
#
# amdv-hwtest.sh - low-risk AMD-V / AMDV-plugin probes for a Big Sur box.
#
# Safe by default: the read-only probes (CPUID, sysctl, kernel symbols) touch
# nothing. Passing --load additionally loads Lilu + AMDV and captures the
# plugin's log. It never enables guest launch (that is a compile-time flag),
# so no VMRUN is executed.
#
# The CPUID probe is delegated to tests/svmcheck (built by `make tests`), so
# the C lives in exactly one place: tests/svmcheck.c.
#
# Usage:
#   ./tests/amdv-hwtest.sh                         # read-only probes only
#   sudo ./tests/amdv-hwtest.sh --load \
#        --lilu /path/to/Lilu.kext [--amdv ./build/AMDV.kext]
#
# Feed the "SVM feature flags" line and the "VMX gate" result back to guide
# what to build next.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$TESTS_DIR/.." && pwd)"
SVMCHECK="$TESTS_DIR/svmcheck"
AMDV_KEXT="$REPO_DIR/build/AMDV.kext"
LILU_KEXT=""
DO_LOAD=0
KERNEL="/System/Library/Kernels/kernel"

while [ $# -gt 0 ]; do
  case "$1" in
    --load)  DO_LOAD=1 ;;
    --amdv)  AMDV_KEXT="$2"; shift ;;
    --lilu)  LILU_KEXT="$2"; shift ;;
    -h|--help) grep '^#' "$0" | cut -c3-; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

hr()   { printf '\n=== %s ===\n' "$1"; }
note() { printf '  %s\n' "$*"; }

# --------------------------------------------------------------------------
hr "Host"
note "$(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
note "machdep.cpu.brand: $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
note "machdep.cpu.vendor: $(sysctl -n machdep.cpu.vendor 2>/dev/null)"

# --------------------------------------------------------------------------
hr "SVM capability (CPUID 0x8000000A)"
# Delegates to tests/svmcheck rather than carrying its own copy of the probe.
# svmcheck is built for x86_64, so it only executes on the AMD target (or an
# Apple Silicon host with Rosetta 2).
if [ ! -x "$SVMCHECK" ]; then
  note "svmcheck not built; building via make -C $TESTS_DIR"
  make -C "$TESTS_DIR" >/dev/null 2>&1 || note "build failed (try: make tests)"
fi
if [ -x "$SVMCHECK" ]; then
  "$SVMCHECK" 2>&1 | sed 's/^/  /'
  rc=${PIPESTATUS[0]}
  [ "$rc" -ne 0 ] && note "(svmcheck exited $rc)"
else
  note "svmcheck unavailable; build it with: make tests"
fi

# --------------------------------------------------------------------------
hr "Hypervisor support (before)"
if HV=$(sysctl -n kern.hv_support 2>/dev/null); then
  note "kern.hv_support = $HV"
else
  note "kern.hv_support = <absent>  (expected on stock AMD; the gate hook aims to change this)"
fi

# --------------------------------------------------------------------------
hr "Kernel VMX-gate symbols"
if [ -r "$KERNEL" ]; then
  MATCHES=$(nm -arch x86_64 "$KERNEL" 2>/dev/null | grep -iE 'vmx_is_available|vmx_hv_support|hv_support|hv_get_support' || true)
  if [ -n "$MATCHES" ]; then
    echo "$MATCHES" | sed 's/^/  /'
    note "^ compare these against kVmxAvailSymbols[] in src/kern_hv_amd.cpp"
  else
    note "no matching symbols in on-disk kernel (may be stripped; try the KDK's kernel.development)"
  fi
else
  note "kernel not readable at $KERNEL"
fi

# --------------------------------------------------------------------------
if [ "$DO_LOAD" -eq 0 ]; then
  hr "Done (read-only)"
  note "Re-run with:  sudo $0 --load --lilu /path/to/Lilu.kext"
  exit 0
fi

hr "Loading kexts"
note "WARNING: AMDV is a Lilu plugin, and Lilu closes its plugin-registration"
note "window during early boot (finaliseRequests() at kPEEnableScreen). A plugin"
note "loaded post-boot with kmutil gets Error::TooLate from requestAccess() and"
note "does nothing at all - it will load, start, and be inert."
note ""
note "This path is therefore only useful for checking that the kext VALIDATES and"
note "LOADS. To actually exercise the plugin it must be injected at boot:"
note "  1. cp -R build/AMDV.kext /Volumes/EFI/EFI/OC/Kexts/"
note "  2. add it to config.plist -> Kernel -> Add, ordered AFTER Lilu"
note "  3. reboot, then re-run this script without --load and read the log"
note ""
if [ "$(id -u)" -ne 0 ]; then note "must be root to load kexts; re-run with sudo"; exit 1; fi

# Resolve a kext path. If the given path is absent, fall back to the usual
# places and say what was tried - "path missing/invalid" alone is useless.
# Sets RESOLVED on success.
RESOLVED=""
resolve_kext() {
  local label="$1" want="$2" name c
  RESOLVED=""
  name="$label.kext"
  [ -n "$want" ] && case "$want" in *.kext) name="$(basename "$want")" ;; esac

  if [ -n "$want" ] && [ -d "$want" ]; then RESOLVED="$want"; return 0; fi

  if [ -n "$want" ]; then
    note "$label: '$want' does not exist (or is not a directory)"
  else
    note "$label: no path given, searching known locations"
  fi

  for c in "/Volumes/EFI/EFI/OC/Kexts/$name" \
           "/Library/Extensions/$name" \
           "$REPO_DIR/build/$name"; do
    if [ -d "$c" ]; then note "  -> found $c"; RESOLVED="$c"; return 0; fi
  done

  note "  searched: /Volumes/EFI/EFI/OC/Kexts/$name"
  note "            /Library/Extensions/$name"
  note "            $REPO_DIR/build/$name"
  if [ ! -d /Volumes/EFI ]; then
    note "  note: the EFI system partition is NOT mounted at /Volumes/EFI."
    note "        OpenCore kexts live on the ESP; mount it with:"
    note "          sudo diskutil mount EFI"
    note "        (a bare /EFI/... path only works if the ESP is mounted there)"
  fi
  return 1
}

# kmutil refuses any bundle not owned root:wheel. Freshly built kexts are owned
# by the building user, and kexts on the ESP sit on FAT32 which has no
# ownership at all - so load from a root-owned staging copy instead of
# chown'ing build/ (which would then need sudo to rebuild or clean).
STAGE="$(mktemp -d /tmp/amdv-stage.XXXXXX)"
chown root:wheel "$STAGE" 2>/dev/null
chmod 755 "$STAGE"

stage_kext() {
  local src="$1" dst
  dst="$STAGE/$(basename "$src")"
  /bin/rm -rf "$dst"
  /bin/cp -R "$src" "$dst"      || return 1
  chown -R root:wheel "$dst"    || return 1
  chmod -R 755 "$dst"           || return 1
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

resolve_kext Lilu "$LILU_KEXT" || exit 1
LILU_KEXT="$RESOLVED"
resolve_kext AMDV "$AMDV_KEXT" || { note "build it first with: make"; exit 1; }
AMDV_KEXT="$RESOLVED"

# Timestamp so we only show log lines from this run.
SINCE=$(date '+%Y-%m-%d %H:%M:%S')

load_one "$LILU_KEXT" || exit 1
load_one "$AMDV_KEXT" || exit 1
sleep 1

hr "Loaded in kernel (kextstat)"
# The definitive check: is the kext actually resident, and is Lilu there too?
# A plugin can validate and load yet still be inert if Lilu never registered it.
if kextstat 2>/dev/null | grep -iE "vit9696|hackintosh|lilu|amdv" | sed 's/^/  /'; then
  :
else
  note "neither Lilu nor AMDV appear in kextstat - nothing is resident"
fi

hr "AMDV log"
# Match Lilu too: its messages explain why a plugin was rejected, and our own
# SYSLOGs are prefixed with the product name by Lilu's logging macros.
log show --style compact --start "$SINCE" \
    --predicate 'eventMessage CONTAINS "AMDV" OR eventMessage CONTAINS "Lilu"' \
    2>/dev/null | sed 's/^/  /' \
  || note "no log entries"
note ""
note "If the only lines above are from kernelmanagerd, the plugin loaded but never"
note "ran: expected when injecting post-boot (see the warning above). Lilu's own"
note "logging needs the -liludbgall boot-arg to be verbose."

hr "Hypervisor support (after)"
if HV=$(sysctl -n kern.hv_support 2>/dev/null); then
  note "kern.hv_support = $HV"
else
  note "kern.hv_support = <absent>"
fi

hr "Done"
note "Key lines to report back: the 'SVM present: rev ... features' line and"
note "whether AMDV logged 'routed <symbol>' or 'could not locate a VMX gate symbol'."
