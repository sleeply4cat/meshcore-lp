/* nRF52840 application behind Adafruit UF2/DFU bootloader + S140 v6.1.1 SoftDevice.
 * SoftDevice occupies 0x1000..0x26000 (never enabled by us, only makes the bootloader
 * place the application at a fixed address). 0xED000..0xF4000 is our config storage
 * (same place Adafruit InternalFS uses), bootloader starts at 0xF4000.
 * RAM is fully ours: SoftDevice is never enabled and VTOR points at our own table. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x26000,    LENGTH = 0xED000 - 0x26000
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x3BF00
  RETAIN (rw) : ORIGIN = 0x2003BF00, LENGTH = 0x100   /* below the bootloader stack, survives DFU */
}

__storage_start = 0xED000;
__storage_end   = 0xF4000;

SECTIONS
{
  .retained (NOLOAD) : { KEEP(*(.retained*)) } > RETAIN
}

INCLUDE "nrf_common.ld"
