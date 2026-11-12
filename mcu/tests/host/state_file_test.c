#include "state_file.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* Standard CRC-32 check value, also chained in two parts. */
    const uint8_t Check[] = "123456789";
    assert(StateFile_Crc32(0, Check, 9) == 0xCBF43926u);
    assert(StateFile_Crc32(StateFile_Crc32(0, Check, 4), Check + 4, 5) == 0xCBF43926u);

    assert(StateFile_PayloadLength(0) == 50472);
    assert(StateFile_PayloadLength(32768) == 83240); /* matches the board simulation */
    assert(StateFile_PayloadLength(128 * 1024) <= kStateFileSlotSize);

    StateFileGame_t Game = {.RomSize = 2u << 20, .RamSize = 32768, .GlobalChecksum = 0x1234, .CartType = 0x10};
    memcpy(Game.Title, "POKEMON_CRYSTAL", 15);
    uint8_t Header[kStateFileHeaderSize];
    StateFile_EncodeHeader(&Game, StateFile_PayloadLength(Game.RamSize), 0xdeadbeef, Header);
    uint32_t Length = 0, Crc = 0;
    assert(StateFile_DecodeHeader(Header, &Game, &Length, &Crc) == kStateFile_Ok);
    assert(Length == 83240 && Crc == 0xdeadbeef);

    /* Every identity field and the header checksum are enforced. */
    StateFileGame_t Other = Game;
    Other.GlobalChecksum ^= 1;
    assert(StateFile_DecodeHeader(Header, &Other, &Length, &Crc) == kStateFile_WrongGame);
    Other = Game; Other.Title[0] = 'X';
    assert(StateFile_DecodeHeader(Header, &Other, &Length, &Crc) == kStateFile_WrongGame);
    Other = Game; Other.RamSize = 8192;
    assert(StateFile_DecodeHeader(Header, &Other, &Length, &Crc) == kStateFile_WrongGame);
    Other = Game; Other.CartType = 0x13;
    assert(StateFile_DecodeHeader(Header, &Other, &Length, &Crc) == kStateFile_WrongGame);
    for (unsigned Byte = 0; Byte < kStateFileHeaderSize; ++Byte)
    {
        uint8_t Damaged[kStateFileHeaderSize];
        memcpy(Damaged, Header, sizeof(Damaged));
        Damaged[Byte] ^= 0x40;
        assert(StateFile_DecodeHeader(Damaged, &Game, &Length, &Crc) != kStateFile_Ok);
    }
    /* A consistent header with a wrong length is still refused. */
    StateFile_EncodeHeader(&Game, 83240 - 8, 0, Header);
    assert(StateFile_DecodeHeader(Header, &Game, &Length, &Crc) == kStateFile_BadLength);

    uint8_t Payload[8] = {0, 0, 0, 0, 0xce, 0xb0, 0, 0};
    assert(StateFile_PayloadSignatureOk(Payload, sizeof(Payload)));
    Payload[5] = 0xcc;
    assert(!StateFile_PayloadSignatureOk(Payload, sizeof(Payload)));
    assert(!StateFile_PayloadSignatureOk(Payload, 4));
    puts("PASS state file format: CRC, header fields, identity, damage and length checks");
    return 0;
}
