/* .nes header parsing and the info block the NES core reads (main/nes_rom.h). */
#include <assert.h>
#include "nes_rom.h"

static void Header(uint8_t h[16], uint8_t prg, uint8_t chr, uint8_t f6, uint8_t f7)
{
    memset(h, 0, 16); memcpy(h, "NES\x1a", 4); h[4] = prg; h[5] = chr; h[6] = f6; h[7] = f7;
}

int main(void)
{
    uint8_t h[16], info[16]; NesRomInfo_t i;
    /* iNES 1.0 NROM-256, vertical mirroring: 8 KiB PRG RAM assumed (byte 12 = 7). */
    Header(h, 2, 1, 0x01, 0x00);
    assert(NesRom_Parse(h, 16 + 32768 + 8192, &i) == ESP_OK);
    assert(i.Mapper == 0 && i.PrgBytes == 32768 && i.ChrBytes == 8192 && i.DataOffset == 16);
    NesRom_InfoBlock(&i, info);
    assert(!memcmp(info, "NES1", 4) && info[4] == 0 && info[7] == 0x01 && info[8] == 32 && info[10] == 8);
    assert(info[12] == 0x07 && info[13] == 0);
    /* Short file. */
    assert(NesRom_Parse(h, 16 + 32768, &i) == ESP_ERR_INVALID_SIZE);
    /* MMC3 with battery, trainer and CHR RAM. */
    Header(h, 8, 0, 0x46, 0x00);
    assert(NesRom_Parse(h, 16 + 512 + 131072, &i) == ESP_OK);
    assert(i.Mapper == 4 && i.Battery && i.DataOffset == 16 + 512 && i.Flags == (0x04 | 0x08));
    /* DiskDude! tail: upper mapper nibble ignored, mapper 1 (not 65). */
    Header(h, 8, 0, 0x10, 0x40); memcpy(h + 7, "DiskDude!", 9);
    assert(NesRom_Parse(h, -1, &i) == ESP_OK && i.Mapper == 1);
    /* NES 2.0: mapper 69 with 8 KiB PRG NVRAM (0x70); RAM byte passed through. */
    Header(h, 16, 32, 0x52, 0x48); h[10] = 0x70;
    assert(NesRom_Parse(h, -1, &i) == ESP_OK && i.Mapper == 69 && (i.Flags & 0x10));
    NesRom_InfoBlock(&i, info);
    assert(info[4] == 69 && info[12] == 0x70);
    /* Unsupported mapper (5, MMC5) and bad magic. */
    Header(h, 16, 16, 0x50, 0x00);
    assert(NesRom_Parse(h, -1, &i) == ESP_ERR_NOT_SUPPORTED);
    h[0] = 'X';
    assert(NesRom_Parse(h, -1, &i) == ESP_ERR_INVALID_RESPONSE);
    return 0;
}
