#!/usr/bin/env bash
# read-crumb.sh — dump the boot_crumb_data snapshot from flash.
#
# Prerequisites:
#   - Device must be in BOOTSEL mode (either via a crash+watchdog or by
#     holding BOOTSEL while plugging in).  The firmware calls
#     boot_crumb_dump_to_bootsel() to get here; BOOTSEL from power-up does
#     NOT preserve crumbs (SRAM is wiped by the bootrom).
#
# Usage:
#   scripts/read-crumb.sh [path/to/deskhop.elf]
#
# If no ELF is supplied it looks for build-prod/deskhop.elf relative to
# the repo root (= directory containing this script's parent).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$REPO_ROOT/build-prod/deskhop.elf}"

if [[ ! -f "$ELF" ]]; then
  echo "error: ELF not found at $ELF" >&2
  echo "  usage: $0 [path/to/deskhop.elf]" >&2
  exit 1
fi

# Resolve the flash crumb address from the ELF symbol table so this script
# stays correct even if the linker script changes.
NM=${NM:-arm-none-eabi-nm}
if ! command -v "$NM" &>/dev/null; then
  # Try the Homebrew cask path used by this project.
  NM=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin/arm-none-eabi-nm
fi

ADDR_HEX=$("$NM" "$ELF" | awk '$3 == "_boot_crumb_flash" { print $1; exit }')
if [[ -z "$ADDR_HEX" ]]; then
  echo "error: _boot_crumb_flash symbol not found in $ELF" >&2
  exit 1
fi

ADDR="0x${ADDR_HEX}"
# 16 slots × 4 bytes = 64 bytes
END=$(printf "0x%X" $(( 0x${ADDR_HEX} + 64 )))

TMP=$(mktemp /tmp/crumb_XXXXXX.bin)
trap 'rm -f "$TMP"' EXIT

echo "Saving flash crumb [$ADDR .. $END) from BOOTSEL device …"
picotool save -r "$ADDR" "$END" "$TMP"

# ── pretty-print ────────────────────────────────────────────────────────────
PHASES=(
  [0x01]="ENTER_MAIN"            [0x02]="ENTER_INITIAL_SETUP"
  [0x03]="AFTER_SET_SYS_CLOCK"  [0x04]="AFTER_LOAD_CONFIG"
  [0x05]="AFTER_LED_INIT"       [0x06]="AFTER_CONFIG_MODE_CHECK"
  [0x07]="AFTER_BOARD_AUTOPROBE"[0x08]="AFTER_SERIAL_INIT"
  [0x09]="AFTER_QUEUES"         [0x0A]="BEFORE_CORE1_LAUNCH"
  [0x0B]="AFTER_CORE1_LAUNCH"   [0x0C]="BEFORE_TUD_INIT"
  [0x0D]="AFTER_TUD_INIT"       [0x0E]="BEFORE_TUH_INIT"
  [0x0F]="AFTER_TUH_INIT"       [0x10]="AFTER_DMA"
  [0x11]="BEFORE_CYW43_INIT"    [0x12]="AFTER_CYW43_INIT"
  [0x13]="BEFORE_BT_HID_INIT"   [0x14]="BT_HID_L2CAP_DONE"
  [0x15]="BT_HID_HOST_INIT_DONE"[0x16]="BT_HID_FW_PRELOAD_DONE"
  [0x17]="AFTER_BT_HID_INIT"    [0x18]="BEFORE_WATCHDOG_ENABLE"
  [0x19]="AFTER_WATCHDOG_ENABLE"[0x1A]="BEFORE_SET_ACTIVE"
  [0x1B]="AFTER_SET_ACTIVE"     [0x1C]="MAIN_LOOP_FIRST_ITER"
  [0x1D]="MAIN_LOOP_RUNNING"
)

read_u32_le() {
  # read 4 little-endian bytes at byte offset $1 from $TMP
  local off=$1
  local b0 b1 b2 b3
  b0=$(dd if="$TMP" bs=1 skip=$off count=1 2>/dev/null | xxd -p)
  b1=$(dd if="$TMP" bs=1 skip=$((off+1)) count=1 2>/dev/null | xxd -p)
  b2=$(dd if="$TMP" bs=1 skip=$((off+2)) count=1 2>/dev/null | xxd -p)
  b3=$(dd if="$TMP" bs=1 skip=$((off+3)) count=1 2>/dev/null | xxd -p)
  printf "0x%08X" $(( (0x${b3}<<24) | (0x${b2}<<16) | (0x${b1}<<8) | 0x${b0} ))
}

echo ""
echo "═══ boot_crumb dump ══════════════════════════════════════════════"
NAMES=( MAGIC PHASE DETAIL HEARTBEAT CORE0_TASK SLOT5 SLOT6 CORE1_TASK
        TUD_LIFECYCLE UART_TX UART_RX QUEUE_DROPS STATE_SNAPSHOT
        RELAY_BRANCH TX_DMA LINK_DIAG )
for i in $(seq 0 15); do
  val=$(read_u32_le $((i*4)))
  name="${NAMES[$i]:-SLOT$i}"
  extra=""
  if [[ $i -eq 0 ]]; then
    [[ "$val" == "0xC0FFEE00" ]] && extra=" (ARMED)"
    [[ "$val" == "0xDEADBEEF" ]] && extra=" (CRASH CAPTURED)"
  fi
  if [[ $i -eq 1 ]]; then
    pnum=$(( 16#${val:2} ))
    pname="${PHASES[$pnum]:-unknown}"
    extra=" ($pname)"
  fi
  if [[ $i -eq 2 ]]; then
    raw=$(( 16#${val:2} ))
    hi16=$(printf "%04X" $(( raw >> 16 )))
    lo8=$(printf "%02X"  $(( raw & 0xFF )))
    lo16=$(printf "%04X" $(( raw & 0xFFFF )))
    case "$hi16" in
      BB00)
        if [[ $(( raw & 0xFF00 )) -eq $((0xFF00)) ]]; then
          extra=" BT radio init FAILED — hci error 0x${lo8}"
        else
          extra=" BT radio up, inquiry starting"
        fi ;;
      BB01) extra=" BT inquiry result CoD=0x${lo16}" ;;
      BB02)
        if [[ $(( raw & 0xFF00 )) -eq $((0xFF00)) ]]; then
          extra=" BT connect() FAILED — status 0x${lo8}"
        else
          extra=" BT connect attempt"
        fi ;;
      BB03)
        if [[ $(( raw & 0xFF00 )) -eq $((0xFF00)) ]]; then
          extra=" BT connection FAILED — status 0x${lo8}"
        else
          extra=" BT connection opened"
        fi ;;
      BB04) extra=" BT PIN pairing request" ;;
      BB05) extra=" BT SSP confirmation request" ;;
      C943)
        case "$lo16" in
          0001) extra=" about to call cyw43_arch_init" ;;
          0002) extra=" cyw43_arch_init returned (success)" ;;
          00??) extra=" cyw43_arch_init FAILED — rc=0x${lo8}" ;;
          *)    extra=" cyw43_arch_init phase 0x${lo16}" ;;
        esac ;;
      # ── cyw43_btbus_init diagnostics (cybt_shared_bus.c) ──────────────
      BC01) extra=" ↳ entered cyw43_btbus_init" ;;
      BC02) extra=" ↳ cybt_sharedbus_driver_init done" ;;
      BC03) extra=" ↳ fw_download_prepare (alloc) done" ;;
      BC04) extra=" ↳ cybt_fw_download starting  ← HANG if stuck here" ;;
      BC05) extra=" ↳ cybt_fw_download success" ;;
      BC06) extra=" ↳ cybt_wait_bt_ready — iteration 0x${lo8} of 300  ← HANG if stuck here" ;;
      BC07) extra=" ↳ cybt_wait_bt_ready SUCCESS" ;;
      BC08) extra=" ↳ cybt_init_buffer SUCCESS" ;;
      BC09) extra=" ↳ cybt_wait_bt_awake — iteration 0x${lo8} of 300" ;;
      BC0A) extra=" ↳ cybt_wait_bt_awake SUCCESS" ;;
      BC0B) extra=" ↳ cyw43_btbus_init COMPLETE — returning success" ;;
      # ── cyw43_ensure_bt_up diagnostics (cyw43_ctrl.c) ─────────────────
      BD01) extra=" ↳ entered cyw43_ensure_bt_up  ← HANG if stuck here" ;;
      BD02) extra=" ↳ cyw43_ensure_up returned — about to check bt_loaded" ;;
      BD03) extra=" ↳ bt_loaded already true (second call) — skipped btbus_init" ;;
      BD04) extra=" ↳ about to call cyw43_btbus_init" ;;
      BCFF)
        case "$lo16" in
          0001) extra=" ↳ FAIL: fw_download_prepare alloc failed" ;;
          0004) extra=" ↳ FAIL: cybt_fw_download failed" ;;
          0006) extra=" ↳ FAIL: cybt_wait_bt_ready TIMED OUT (all 300 polls)" ;;
          0008) extra=" ↳ FAIL: cybt_init_buffer failed" ;;
          0009) extra=" ↳ FAIL: cybt_wait_bt_awake timed out" ;;
          *)    extra=" ↳ FAIL: btbus error 0x${lo16}" ;;
        esac ;;
      # ── hci_power_control diagnostics (hci.c) ────────────────────────
      BE00)
        case "$lo16" in
          0001) extra=" hci_power_control: about to call (from bt_hid_host_init)" ;;
          0002) extra=" hci_power_control: returned successfully" ;;
          *)    extra=" hci_power_control caller phase 0x${lo16}" ;;
        esac ;;
      BE01) extra=" ↳ entered hci_power_control" ;;
      BE02) extra=" ↳ after btstack_run_loop_remove_timer" ;;
      BE03) extra=" ↳ entered hci_power_control_on" ;;
      BE04) extra=" ↳ after control->on check" ;;
      BE05) extra=" ↳ after chipset->init check" ;;
      BE06) extra=" ↳ after transport->init check" ;;
      BE07) extra=" ↳ about to call transport->open()  ← BD_STEP should fire next" ;;
    esac
  fi
  printf "  [%2d] %-18s %s%s\n" "$i" "$name" "$val" "$extra"
done
echo "══════════════════════════════════════════════════════════════════"
