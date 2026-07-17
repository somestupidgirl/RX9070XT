#!/bin/bash
#
# rdna4fb-diagnose.sh — collect every RDNA4FB diagnostic in one file.
# Run on the target hackintosh as: sudo bash rdna4fb-diagnose.sh
# Output: rdna4fb-diag-<timestamp>.txt in the current directory.
#
# The script detects which rdna4-* boot-args are active and labels each
# gated section accordingly (so an empty section reads "inactive — add the
# arg" vs "active but no output — check the build"). Append-only: never
# remove a capture; add a line whenever the kext gains a new diagnostic.
#
# Boot-args for the full survey round:
#   rdna4-smuping=1 rdna4-ihdump=1 rdna4-pspdump=1 rdna4-dmubping=1 \
#   rdna4-dmubhist=1 rdna4-modedump=1 rdna4-vbl=1
# (add rdna4-hwcursor=1 / rdna4-dmubcursor=1 for cursor experiments;
#  optionally remove the ATY,bin_image DeviceProperties entry to exercise
#  the on-die IP discovery path — check "Discovery,Source" below)
#

set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo (dmesg needs root)"; exit 1; }

OUT="rdna4fb-diag-$(date +%Y%m%d-%H%M%S).txt"

# Active rdna4-* boot-args (space-padded for whole-token matching).
BOOTARGS=" $(nvram boot-args 2>/dev/null | cut -f2- | tr -s '[:space:]' ' ') "
have_arg() { case "$BOOTARGS" in *" rdna4-$1=1 "*) return 0;; *) return 1;; esac; }

section() { echo; echo "=== $1 ==="; }

# gated <title> <grep-ERE> <boot-arg-name>
# Prints matching dmesg lines; if none, distinguishes "arg inactive" from
# "arg active but produced nothing" (a build/version red flag).
gated() {
	local title="$1" pat="$2" arg="$3" out
	section "$title"
	out="$(dmesg | grep -E "$pat")"
	if [ -n "$out" ]; then
		echo "$out"
	elif have_arg "$arg"; then
		echo "(rdna4-$arg=1 active but no output — check kext build/version)"
	else
		echo "(inactive — add rdna4-$arg=1 to boot-args)"
	fi
}

{
	section "system"
	sw_vers
	sysctl -n machdep.cpu.brand_string
	date

	section "active RDNA4FB boot-args"
	found=""
	for a in off cmap lutbypass 8bpc noedid nosleep modedump hwcursor \
	         curmode curtest dmubping dmubhist dmubcursor smuping ihdump \
	         pspdump vbl; do
		have_arg "$a" && found="$found rdna4-$a=1"
	done
	[ -n "$found" ] && echo "active:$found" || echo "(no rdna4-* boot-args set)"

	section "kext loaded?"
	kextstat | grep -i rdna4 || echo "RDNA4FB NOT LOADED"

	section "boot-args"
	nvram boot-args 2>/dev/null || echo "(nvram boot-args unreadable)"

	section "dmesg: full RDNA4FB log"
	dmesg | grep 'RDNA4FB:' || echo "(no RDNA4FB dmesg lines — buffer wrapped or kext absent)"

	section "dmesg: discovery / variant"
	dmesg | grep -E 'RDNA4FB: (probe|Navi 48|discovery):' || true

	section "dmesg: EDID / I2C / HPD"
	dmesg | grep -E 'RDNA4FB: (edid|i2c|cmd):' || true

	section "dmesg: display power (sleep/wake)"
	dmesg | grep -E 'RDNA4FB: power:' || true

	section "dmesg: HW cursor (incl. vm routing + curtest)"
	dmesg | grep -E 'RDNA4FB: cursor:' || true

	# DMUB: catches both 'dmub:' (ping) and 'dmub-hist:' (GOP command decode).
	gated "DMUB mailbox + GOP command history" 'RDNA4FB: dmub' dmubping

	gated "SMU handshake" 'RDNA4FB: smu:' smuping
	gated "interrupt-delivery survey" 'RDNA4FB: ih:' ihdump
	gated "PSP status" 'RDNA4FB: psp:' pspdump

	section "dmesg: emulated VBL"
	dmesg | grep -E 'RDNA4FB: vbl:' || true

	gated "mode-setting survey" 'RDNA4FB: mode:' modedump

	section "ioreg: framebuffer properties"
	ioreg -l -w0 | grep -E '"(Console|AtomBIOS|Discovery|VRAM|MMIO|EDID|GPU|SMU|PSP),' || true

	section "ioreg: what the OS sees (display identity)"
	ioreg -lw0 | grep -E 'IODisplayEDID|DisplayProductID|DisplayVendorID' || true

	section "WindowServer attached?"
	ioreg -l -w0 | grep -c IOFramebufferUserClient | \
		xargs -I{} echo "{} IOFramebufferUserClient instance(s)"
} > "$OUT" 2>&1

echo "wrote $OUT"
echo
echo "Verdict lines to look for:"
echo "  smu:  'PING OK — TestMessage acked' + PMFW version"
echo "  psp:  'verdict: bootloader READY, sOS ALIVE, ...'"
echo "  ih:   RB cntl/base all-zero = GOP left no interrupt ring (expected)"
echo "  dmub: 'PING OK — rptr advanced' + dmub-hist command decode"
echo "  Discovery,Source = 'on-die TMR' if ATY,bin_image was removed"
