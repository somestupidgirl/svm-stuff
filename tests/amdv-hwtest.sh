#!/bin/bash
#
# amdv-hwtest.sh - low-risk AMD-V / AMDV-plugin probes for a Big Sur box.
#
# Safe by default: the read-only probes (CPUID, sysctl, kernel symbols) touch
# nothing. Passing --load additionally loads Lilu + AMDV and captures the
# plugin's log. It never enables guest launch (that is a compile-time flag),
# so no VMRUN is executed.
#
# Usage:
#   ./scripts/amdv-hwtest.sh                       # read-only probes only
#   sudo ./scripts/amdv-hwtest.sh --load \
#        --lilu /path/to/Lilu.kext [--amdv ./build/AMDV.kext]
#
# Feed the "SVM feature flags" line and the "VMX gate" result back to guide
# what to build next.

set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/svm.c" <<'EOF'
#include <cpuid.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    unsigned a, b, c, d;
    char vendor[13] = {0};
    __get_cpuid(0, &a, &b, &c, &d);
    memcpy(vendor + 0, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
    printf("  vendor            : %s\n", vendor);
    if (strcmp(vendor, "AuthenticAMD") != 0) { printf("  (not AMD; SVM checks N/A)\n"); return 0; }

    __get_cpuid(0x80000001, &a, &b, &c, &d);
    printf("  SVM supported     : %s (0x80000001 ECX.2)\n", (c & (1u<<2)) ? "yes" : "NO");
    if (!(c & (1u<<2))) return 0;

    __get_cpuid(0x8000000A, &a, &b, &c, &d);
    printf("  SVM revision      : %u\n", a & 0xff);
    printf("  usable ASIDs      : %u\n", b);
    printf("  feature edx       : 0x%08x\n", d);
    printf("    NP (nested pg)  : %d  <- needed for guest memory\n", !!(d & (1u<<0)));
    printf("    NRIPS           : %d  <- if 0, need instruction-decode RIP fallback\n", !!(d & (1u<<3)));
    printf("    SVML (svm lock) : %d\n", !!(d & (1u<<2)));
    printf("    VMCB clean bits : %d\n", !!(d & (1u<<5)));
    printf("    Flush-by-ASID   : %d\n", !!(d & (1u<<6)));
    printf("    Decode assists  : %d\n", !!(d & (1u<<7)));
    return 0;
}
EOF
if cc "$TMP/svm.c" -o "$TMP/svm" 2>"$TMP/cc.err"; then
  "$TMP/svm"
else
  note "compile failed:"; sed 's/^/    /' "$TMP/cc.err"
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
