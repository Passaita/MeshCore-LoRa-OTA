/* Linker script for RAK3401 with LoRa firmware-pull (fw_ota) staging slot.
 *
 * Fork of nrf52840_s140_v6.ld.  The only change is the FLASH region LENGTH:
 * the application is capped at 0x96000 so the space between there and the
 * LittleFS region (0xED000) can hold one fw_ota staging slot plus a control
 * page.
 *
 *   0x00000 - 0x26000   MBR + SoftDevice S140 v6.1.1        (fixed)
 *   0x26000 - 0x96000   Application            0x70000 = 448 KiB   <-- this file (option C, 2026-09-19)
 *   0x96000 - 0xEC000   fw_ota staging slot    0x56000 = 344 KiB (single slot,
 *                                                since 2026-09-16 - was two A/B
 *                                                slots of 218 KiB each before)
 *   0xEC000 - 0xED000   fw_ota control page    0x01000 =   4 KiB
 *   0xED000 - 0xF4000   InternalFS (LittleFS)  0x07000 =  28 KiB   (fixed by framework)
 *   0xF4000 - 0x100000  Bootloader + MBR params + settings         (fixed)
 *
 * Sizing: the slot holds the LZMA image (about 0.64 of the raw size, planned at 0.75):
 * 340 KiB payload for a 448 KiB app. Must match the bootloader (fw_ota.h) and FwOtaSlots.h.
 *
 * No A/B rollback anymore - a failed post-boot self-test has no second slot
 * to restore. Mitigated by strict pre-flash SHA-256 verification (the slot
 * is never marked VALID / never applied unless it matches) plus OTAFIX's
 * BLE DFU kept in the bootloader as a manual recovery path. See the
 * project's design doc for the full rationale.
 *
 * The slot / control regions are deliberately NOT described to the linker -
 * nothing is placed there at link time.  fw_ota addresses them by absolute
 * address (see helpers/fw_ota/FwOtaSlots.h) and the bootloader owns the apply.
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)     : ORIGIN = 0x26000, LENGTH = 0x96000 - 0x26000

  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM

  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "boards/nrf52_common_fwota.ld"   /* framework copy + the firmware tag section */
