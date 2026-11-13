#include "native_state_shortcuts.h"
#include <string.h>
unsigned NativeShortcut_Update(NativeShortcutState *S,uint16_t Raw,uint32_t Now,bool Enabled,bool *Consume) {
    /* Menu+Select saves, Menu+Start loads (ModRetro's Menu+Up/Down are color
     * temperature and Menu+Left/Right brightness). Both act on release. */
    const uint16_t Menu=0x100,Save=0x02,Load=0x01,Keys=Raw&0x1ff;
    bool Closed=(Raw&0x200)!=0;unsigned Event=0;
    if(!Enabled){memset(S,0,sizeof(*S));*Consume=false;return 0;}
    if(S->Tracking && (uint32_t)(Now-S->Last)>200) { S->Tracking=false;S->Fired=false; }
    S->Last=Now;
    if(S->Swallow && !Keys) {
        if(S->Fired)Event=S->Chord==Save?1:2;
        memset(S,0,sizeof(*S));*Consume=true;return Event;
    }
    if(!S->Tracking && !S->Swallow && Closed && (Keys==(Menu|Save) || Keys==(Menu|Load))) {
        S->Tracking=S->Swallow=true;S->Chord=Keys&~Menu;S->Start=Now;S->Fired=true;
    }
    if(S->Swallow && (Keys & ~(Menu|S->Chord)))S->Fired=false;
    if(S->Tracking) {
        if(Keys!=(Menu|S->Chord)) {
            // A partial release keeps the chord; another button cancels it.
            if(Keys && Keys!=Menu && Keys!=S->Chord)S->Fired=false;
            S->Tracking=false;
        }
    }
    *Consume=S->Swallow || (Keys&Menu)!=0;
    return Event;
}
