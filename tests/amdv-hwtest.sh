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
if [ "$(id -u)" -ne 0 ]; then note "must be root to load kexts; re-run with sudo"; exit 1; fi
[ -d "$AMDV_KEXT" ] || { note "AMDV.kext not found at $AMDV_KEXT (build it first with make)"; exit 1; }
if [ -z "$LILU_KEXT" ] || [ ! -d "$LILU_KEXT" ]; then
  note "Lilu.kext path missing/invalid; pass --lilu /path/to/Lilu.kext"; exit 1
fi

# Timestamp so we only show log lines from this run.
SINCE=$(date '+%Y-%m-%d %H:%M:%S')

load_one() {
  local k="$1"
  note "kmutil load -p $k"
  if ! kmutil load -p "$k" 2>&1 | sed 's/^/    /'; then
    note "load failed for $k (check SIP / csrutil and codesigning)"
    return 1
  fi
}

load_one "$LILU_KEXT" || exit 1
load_one "$AMDV_KEXT" || exit 1
sleep 1

hr "AMDV log"
log show --style compact --start "$SINCE" \
    --predicate 'eventMessage CONTAINS "AMDV"' 2>/dev/null | sed 's/^/  /' \
  || note "no log entries (try: log stream --predicate 'eventMessage CONTAINS \"AMDV\"')"

hr "Hypervisor support (after)"
if HV=$(sysctl -n kern.hv_support 2>/dev/null); then
  note "kern.hv_support = $HV"
else
  note "kern.hv_support = <absent>"
fi

hr "Done"
note "Key lines to report back: the 'SVM present: rev ... features' line and"
note "whether AMDV logged 'routed <symbol>' or 'could not locate a VMX gate symbol'."
