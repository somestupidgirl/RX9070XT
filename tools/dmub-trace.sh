#!/bin/bash
#
# dmub-trace.sh — capture amdgpu's DMUB inbox command stream under Linux.
#
# Ground-truth companion to the macOS-side rdna4-dmubhist decode. The GOP
# firmware on this card speaks a DMUB VBIOS sub-dialect (subtypes 6/10/12/16)
# that does not match mainline amdgpu, so before synthesising mode-set
# commands from macOS we capture what a *working* driver actually sends to
# this exact firmware. Hooks amdgpu's DMUB command-submit function and dumps
# each 64-byte union dmub_rb_cmd (16 dwords) while you trigger an HDMI
# modeset — that stream is the recipe to replay on macOS for the Lenovo.
#
# Run on Debian (amdgpu):  sudo bash dmub-trace.sh
# Needs: bpftrace (apt install bpftrace). GPU assumed at 0000:56:00.0.
#
# HOW TO DRIVE IT: when prompted, physically UNPLUG then REPLUG the HDMI
# cable to the Lenovo (a hotplug forces amdgpu to re-run the full HDMI
# link/pixel-clock bring-up). Replug is the most reliable trigger and needs
# no X/Wayland session. Alternatives if you prefer: `xrandr --output
# <HDMI> --off; xrandr --output <HDMI> --auto` (X only), or DPMS off/on.
#

set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
command -v bpftrace >/dev/null || { echo "install bpftrace: apt install bpftrace"; exit 1; }

OUT="dmub-trace-$(date +%Y%m%d-%H%M%S).txt"

# Pick the first DMUB command-submit symbol present in this kernel. Lowest-
# level queue fns see every command; the DM wrapper is the most likely to be
# non-inlined. In all of these the union dmub_rb_cmd* is the 2nd arg (arg1).
FUNC=""
for f in dmub_srv_fb_cmd_queue dmub_srv_cmd_queue dc_dmub_srv_cmd_run \
         dc_dmub_srv_cmd_list_queue_execute dm_execute_dmub_cmd \
         dc_wake_and_execute_dmub_cmd; do
	if grep -qw "$f" /proc/kallsyms; then FUNC="$f"; break; fi
done
[ -n "$FUNC" ] || { echo "no known DMUB submit symbol in kallsyms — kernel too old/new?"; exit 1; }
echo "hooking amdgpu fn: $FUNC (union dmub_rb_cmd* = arg1)"

# Connector state, for reference.
echo "HDMI connector status:"
for c in /sys/class/drm/card*-HDMI-A-*; do
	[ -e "$c/status" ] && echo "  $(basename "$c"): $(cat "$c/status")"
done

PROG='
kprobe:'"$FUNC"' {
  $p = arg1;
  printf("%llu %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
    nsecs,
    *(uint32*)($p+0),  *(uint32*)($p+4),  *(uint32*)($p+8),  *(uint32*)($p+12),
    *(uint32*)($p+16), *(uint32*)($p+20), *(uint32*)($p+24), *(uint32*)($p+28),
    *(uint32*)($p+32), *(uint32*)($p+36), *(uint32*)($p+40), *(uint32*)($p+44),
    *(uint32*)($p+48), *(uint32*)($p+52), *(uint32*)($p+56), *(uint32*)($p+60));
}'

echo "starting capture -> $OUT"
bpftrace -e "$PROG" > "$OUT" 2>/tmp/dmub-trace.err &
BPID=$!
sleep 2
if ! kill -0 "$BPID" 2>/dev/null; then
	echo "bpftrace failed to start:"; cat /tmp/dmub-trace.err; exit 1
fi

echo
echo ">>> NOW: unplug and replug the HDMI cable to the Lenovo, then press Enter."
read -r _
sleep 1
kill "$BPID" 2>/dev/null; wait "$BPID" 2>/dev/null

echo
echo "=== decoded commands (type/subtype/payload from header dword) ==="
# Header layout (union dmub_rb_cmd.header): type[7:0] subtype[15:8]
# ret[16] multi[17] reg_based[18] payload_bytes[29:24].
awk '
function lbl(t, s) {
  if (t != 128) {
    if (t==0) return "NULL"; if (t==6) return "QUERY_FEATURE_CAPS";
    if (t==68) return "UPDATE_CURSOR_INFO"; return "type"t;
  }
  if (s==0)  return "VBIOS/DIGX_ENCODER_CONTROL";
  if (s==1)  return "VBIOS/DIG1_TRANSMITTER_CONTROL";
  if (s==2)  return "VBIOS/SET_PIXEL_CLOCK";
  if (s==3)  return "VBIOS/ENABLE_DISP_POWER_GATING";
  if (s==15) return "VBIOS/LVTMA_CONTROL";
  if (s==26) return "VBIOS/TRANSMITTER_QUERY_DP_ALT";
  if (s==28) return "VBIOS/DOMAIN_CONTROL";
  if (s==29) return "VBIOS/TRANSMITTER_SET_PHY_FSM";
  return "VBIOS/sub"s;
}
{
  hdr = strtonum("0x" $2);
  t = and(hdr, 255); s = and(rshift(hdr,8),255); pl = and(rshift(hdr,24),63);
  printf "hdr=%s %-30s pl=%2d  %s %s %s %s %s %s %s\n",
    $2, lbl(t,s), pl, $3,$4,$5,$6,$7,$8,$9;
}' "$OUT"

echo
echo "raw 16-dword capture saved to $OUT"
echo "If NO commands appeared: the replug did not trigger a modeset, or the"
echo "hooked fn was inlined — try DPMS/xrandr toggle, or edit FUNC candidates."
