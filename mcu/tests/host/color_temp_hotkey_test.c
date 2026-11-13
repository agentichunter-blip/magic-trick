#include <assert.h>
#include <stdio.h>
#include "color_temp_hotkey.h"

int main(void)
{
    ColorTempHotkey_t S = {0};
    /* Menu closed (0x200) and Menu held (0x100): Up warmer, Down cooler, once per press. */
    assert(ColorTempHotkey_Update(&S, 0x200) == 0);
    assert(ColorTempHotkey_Update(&S, 0x300) == 0);
    assert(ColorTempHotkey_Update(&S, 0x310) == 1);
    assert(ColorTempHotkey_Update(&S, 0x310) == 0);           /* held: no repeat */
    assert(ColorTempHotkey_Update(&S, 0x300) == 0);
    assert(ColorTempHotkey_Update(&S, 0x310) == 1);           /* pressed again */
    assert(ColorTempHotkey_Update(&S, 0x300) == 0);
    assert(ColorTempHotkey_Update(&S, 0x380) == -1);
    assert(ColorTempHotkey_Update(&S, 0x300) == 0);
    /* Both together: nothing. */
    assert(ColorTempHotkey_Update(&S, 0x390) == 0);
    assert(ColorTempHotkey_Update(&S, 0x200) == 0);
    /* Without Menu held (normal play) Up/Down belong to the game. */
    assert(ColorTempHotkey_Update(&S, 0x210) == 0);
    assert(ColorTempHotkey_Update(&S, 0x280) == 0);
    /* Menu open (bit 9 clear): the menu uses the buttons. */
    assert(ColorTempHotkey_Update(&S, 0x100) == 0);
    assert(ColorTempHotkey_Update(&S, 0x110) == 0);
    /* Up already held when Menu goes down: no step until pressed again. */
    assert(ColorTempHotkey_Update(&S, 0x210) == 0);
    assert(ColorTempHotkey_Update(&S, 0x310) == 0);
    puts("PASS color temperature hotkey: Menu+Up warmer, Menu+Down cooler, edges only, menu closed only");
    return 0;
}
