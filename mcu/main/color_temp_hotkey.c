#include "color_temp_hotkey.h"

int ColorTempHotkey_Update(ColorTempHotkey_t *State, uint16_t Raw)
{
    enum { Up = 0x010, Down = 0x080, MenuHeld = 0x100, MenuClosed = 0x200 };
    const uint16_t Pressed = Raw & ~State->Last;
    State->Last = Raw;
    if ((Raw & (MenuHeld | MenuClosed)) != (MenuHeld | MenuClosed)) return 0;
    if ((Pressed & Up) && !(Raw & Down)) return 1;
    if ((Pressed & Down) && !(Raw & Up)) return -1;
    return 0;
}
