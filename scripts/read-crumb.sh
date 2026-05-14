#!/usr/bin/env bash
#
# scripts/read-crumb.sh — read SRAM boot crumbs from one or more Picos in
# BOOTSEL mode (auto-entered after a watchdog-caught hang, or manual).
#
# The firmware (see src/include/boot_crumb.h, src/boot_crumb.c) writes phase
# tags to `boot_crumb_data[]`, which lives in the NOLOAD .uninitialized_data
# section — so it survives a watchdog-triggered reset (SRAM is preserved)
# but is NOT zeroed by the C startup code. On the next boot, the firmware
# auto-enters BOOTSEL via reset_usb_boot() if it detects a watchdog reboot.
#
# Usage:
#   scripts/read-crumb.sh                    # use build-prod/deskhop.elf
#   scripts/read-crumb.sh path/to/deskhop.elf
#
# Multi-device: if more than one Pico is in BOOTSEL, this script dumps the
# crumbs from EACH, labelled by USB bus:address.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

ELF="${1:-build-prod/deskhop.elf}"
if [[ ! -f "$ELF" ]]; then
    echo "ELF not found at $ELF" >&2
    exit 1
fi

ARM_PATH="${ARM_PATH:-/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin}"
NM="${ARM_PATH}/arm-none-eabi-nm"
[[ -x "$NM" ]] || NM="arm-none-eabi-nm"

echo ">> Locate _boot_crumb_flash in $ELF"
# Read from the flash mirror, NOT the SRAM symbol. The bootrom's
# REBOOT_TYPE_BOOTSEL path wipes all SRAM (including SCRATCH_X) on entry,
# so SRAM-only crumbs are gone by the time picotool reads them. The firmware
# saves a snapshot to a reserved flash sector before entering BOOTSEL.
SYM_LINE=$("$NM" "$ELF" 2>/dev/null | grep ' _boot_crumb_flash$' || true)
if [[ -z "$SYM_LINE" ]]; then
    echo "** Couldn't find _boot_crumb_flash symbol. Did you rebuild after the linker script edit? **" >&2
    exit 1
fi
SYM_ADDR=$(echo "$SYM_LINE" | awk '{print $1}')
ADDR_START="0x${SYM_ADDR}"
ADDR_END=$(printf '0x%08x' $((ADDR_START + 32)))
echo "   _boot_crumb_flash (flash mirror) @ $ADDR_START..$ADDR_END"

echo
echo ">> Enumerate BOOTSEL devices"
INFO_OUTPUT=$(picotool info 2>&1)
echo "$INFO_OUTPUT" | sed 's/^/   /'

# Build (bus,address) pair list. Two formats picotool uses:
#   "RP2350 device at bus N, address M:"      (multi-device)
#   (none — for single device output omits the header)
PAIRS=()
while IFS=$'\t' read -r bus addr; do
    PAIRS+=("$bus:$addr")
done < <(echo "$INFO_OUTPUT" \
    | awk '/^RP[0-9]+ device at bus [0-9]+, address [0-9]+:/ {
              gsub(/[,:]/, " ");
              for (i=1;i<=NF;i++) {
                  if ($i == "bus") bus = $(i+1);
                  if ($i == "address") addr = $(i+1);
              }
              print bus "\t" addr;
           }')

if (( ${#PAIRS[@]} == 0 )); then
    # Single-device case — picotool printed the program info without the
    # multi-device header. Issue the save without filters.
    if echo "$INFO_OUTPUT" | grep -q "No accessible RP-series devices"; then
        echo "** No BOOTSEL device. Hold BOOTSEL while replugging USB. **" >&2
        exit 1
    fi
    PAIRS=("")
fi

decode_magic() {
    case "0x$1" in
        0xc0ffee00) echo "ARMED (firmware armed crumbs but didn't shut down cleanly)" ;;
        0xdeadbeef) echo "CAPTURED (auto-entered BOOTSEL from watchdog-detected crash)" ;;
        0x00000000) echo "ZERO (cold boot or SRAM wiped — no crumb data)" ;;
        *)          echo "UNKNOWN (cold boot — SRAM garbage)" ;;
    esac
}

decode_phase() {
    case "0x$1" in
        0x00000000) echo "(no phase recorded)" ;;
        0x00000001) echo "ENTER_MAIN" ;;
        0x00000002) echo "ENTER_INITIAL_SETUP" ;;
        0x00000003) echo "AFTER_SET_SYS_CLOCK" ;;
        0x00000004) echo "AFTER_LOAD_CONFIG" ;;
        0x00000005) echo "AFTER_LED_INIT" ;;
        0x00000006) echo "AFTER_CONFIG_MODE_CHECK" ;;
        0x00000007) echo "AFTER_BOARD_AUTOPROBE" ;;
        0x00000008) echo "AFTER_SERIAL_INIT" ;;
        0x00000009) echo "AFTER_QUEUES" ;;
        0x0000000a) echo "BEFORE_CORE1_LAUNCH" ;;
        0x0000000b) echo "AFTER_CORE1_LAUNCH" ;;
        0x0000000c) echo "BEFORE_TUD_INIT" ;;
        0x0000000d) echo "AFTER_TUD_INIT" ;;
        0x0000000e) echo "BEFORE_TUH_INIT (pio_usb_host_config)" ;;
        0x0000000f) echo "AFTER_TUH_INIT" ;;
        0x00000010) echo "AFTER_DMA" ;;
        0x00000011) echo "BEFORE_CYW43_INIT" ;;
        0x00000012) echo "AFTER_CYW43_INIT" ;;
        0x00000013) echo "BEFORE_WATCHDOG_ENABLE" ;;
        0x00000014) echo "AFTER_WATCHDOG_ENABLE" ;;
        0x00000015) echo "BEFORE_SET_ACTIVE (set_active_output)" ;;
        0x00000016) echo "AFTER_SET_ACTIVE" ;;
        0x00000017) echo "MAIN_LOOP_FIRST_ITER" ;;
        0x00000018) echo "MAIN_LOOP_RUNNING" ;;
        *)          echo "UNKNOWN (0x$1)" ;;
    esac
}

dump_one() {
    local sel_label="$1"
    shift
    # bash 3.2 + `set -u` chokes on empty-array expansion. Build the array
    # via append-on-arg-count to keep both empty and populated cases happy.
    local sel_args=()
    if (( $# > 0 )); then
        sel_args=("$@")
    fi
    local tmp
    tmp=$(mktemp /tmp/crumb.XXXXXXXX)

    echo
    echo "════════════════════════════════════════════════════════════════"
    echo " Device: $sel_label"
    echo "════════════════════════════════════════════════════════════════"
    # -t bin so picotool doesn't try to infer from the (suffix-less) tmpfile.
    if (( ${#sel_args[@]} == 0 )); then
        echo ">> picotool save -r $ADDR_START $ADDR_END -t bin $tmp"
        if ! picotool save -r "$ADDR_START" "$ADDR_END" -t bin "$tmp"; then
            echo "** picotool save failed for $sel_label **" >&2
            rm -f "$tmp"
            return 1
        fi
    else
        echo ">> picotool save -r $ADDR_START $ADDR_END ${sel_args[*]} -t bin $tmp"
        if ! picotool save -r "$ADDR_START" "$ADDR_END" "${sel_args[@]}" -t bin "$tmp"; then
            echo "** picotool save failed for $sel_label **" >&2
            rm -f "$tmp"
            return 1
        fi
    fi

    echo
    echo ">> 32-byte dump:"
    od -An -tx1 -v -N 32 "$tmp" | sed 's/^/   /'

    local words=()
    while IFS= read -r w; do
        words+=("$w")
    done < <(od -An -tx4 -v -N 32 "$tmp" | tr -s ' ' '\n' | grep -v '^$')

    if (( ${#words[@]} < 8 )); then
        echo "** parse error: got ${#words[@]} words, expected 8 **" >&2
        rm -f "$tmp"
        return 1
    fi

    local hb="${words[3]}"
    local hb_core0=$((0x${hb:0:4}))
    local hb_core1=$((0x${hb:4:4}))

    echo
    printf '   [0] MAGIC      = 0x%s  %s\n' "${words[0]}" "$(decode_magic "${words[0]}")"
    printf '   [1] PHASE      = 0x%s  %s\n' "${words[1]}" "$(decode_phase "${words[1]}")"
    printf '   [2] DETAIL     = 0x%s\n' "${words[2]}"
    printf '   [3] HEARTBEAT  = 0x%s  (core0=%d  core1=%d)\n' "$hb" "$hb_core0" "$hb_core1"
    # slot[4] tags the core0 task currently executing (0xCC00xxxx).
    local s4="${words[4]}"
    local s4_decoded="(unset)"
    case "0x$s4" in
        0xcc000000) s4_decoded="usb_device_task (core0 task[0])" ;;
        0xcc000001) s4_decoded="kick_watchdog_task (core0 task[1])" ;;
        0xcc000002) s4_decoded="process_kbd_queue_task (core0 task[2])" ;;
        0xcc000003) s4_decoded="process_mouse_queue_task (core0 task[3])" ;;
        0xcc000004) s4_decoded="process_hid_queue_task (core0 task[4])" ;;
        0xcc000005) s4_decoded="process_uart_tx_task (core0 task[5])" ;;
        0xcc000006) s4_decoded="cyw43_poll_task (core0 task[6])" ;;
        0xcc00ffff) s4_decoded="(between passes — no task active)" ;;
        0x00000000) s4_decoded="(unset — never reached main loop)" ;;
        *)          s4_decoded="UNKNOWN (0x$s4)" ;;
    esac
    printf '   [4] core0 task = 0x%s  %s\n' "$s4" "$s4_decoded"
    local s5="${words[5]}"
    local s5_decoded="(unset)"
    case "0x$s5" in
        0xaaaaaaaa) s5_decoded="MAIN_ENTERED (firmware reached top of main but did NOT return from boot_crumb_check)" ;;
        0xbbbbbbbb) s5_decoded="MAIN_PAST_CHECK (returned from boot_crumb_check; if PHASE is still 0x01, crash is between check and initial_setup)" ;;
        0x00000000) s5_decoded="ZERO — firmware never wrote tracer (didn'\''t reach main, OR SRAM was wiped after writing)" ;;
        *)          s5_decoded="UNKNOWN" ;;
    esac
    printf '   [5] main tracer= 0x%s  %s\n' "$s5" "$s5_decoded"
    local s6="${words[6]}"
    local s6_decoded="(unset)"
    case "0x$s6" in
        0xc0de0001) s6_decoded="BOOT_CRUMB_CHECK_ENTERED (but did NOT take BOOTSEL path → fresh-arm path ran)" ;;
        0xc0de0002) s6_decoded="BOOTSEL_PATH_TAKEN (auto-BOOTSEL handler fired)" ;;
        0x00000000) s6_decoded="ZERO — boot_crumb_check never ran, OR SRAM was wiped after it ran" ;;
        *)          s6_decoded="UNKNOWN" ;;
    esac
    printf '   [6] check tracer=0x%s  %s\n' "$s6" "$s6_decoded"
    # slot[7] tags the core1 task currently executing (0xCC01xxxx).
    local s7="${words[7]}"
    local s7_decoded="(unset)"
    case "0x$s7" in
        0xcc010000) s7_decoded="usb_host_task (core1 task[0])" ;;
        0xcc010001) s7_decoded="packet_receiver_task (core1 task[1])" ;;
        0xcc010002) s7_decoded="led_blinking_task (core1 task[2])" ;;
        0xcc010003) s7_decoded="screensaver_task (core1 task[3])" ;;
        0xcc010004) s7_decoded="firmware_upgrade_task (core1 task[4])" ;;
        0xcc010005) s7_decoded="heartbeat_output_task (core1 task[5])" ;;
        0xcc01ffff) s7_decoded="(between passes — no task active)" ;;
        0x00000000) s7_decoded="(unset — core1 never ran)" ;;
        *)          s7_decoded="UNKNOWN (0x$s7)" ;;
    esac
    printf '   [7] core1 task = 0x%s  %s\n' "$s7" "$s7_decoded"

    rm -f "$tmp"
}

for pair in "${PAIRS[@]}"; do
    if [[ -z "$pair" ]]; then
        dump_one "(only device)"
    else
        bus="${pair%%:*}"
        addr="${pair##*:}"
        dump_one "bus $bus, address $addr" --bus "$bus" --address "$addr"
    fi
done

echo
echo "Done. ${#PAIRS[@]} device(s) reported."
