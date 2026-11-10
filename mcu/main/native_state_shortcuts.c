#include "native_state_shortcuts.h"
#include <string.h>
unsigned NativeShortcut_Update(NativeShortcutState *S,uint16_t Raw,uint32_t Now,bool Enabled,bool *Consume) {
    const uint16_t Menu=0x100,Up=0x10,Down=0x80,Keys=Raw&0x1ff;
    bool Closed=(Raw&0x200)!=0;unsigned Event=0;
    if(!Enabled){memset(S,0,sizeof(*S));*Consume=false;return 0;}
    if(S->Tracking && (uint32_t)(Now-S->Last)>200) { S->Tracking=false;S->Fired=false; }
    S->Last=Now;
    if(S->Swallow && !Keys) {
        if(S->Fired)Event=S->Chord==Up?1:2;
        memset(S,0,sizeof(*S));*Consume=true;return Event;
    }
    if(!S->Tracking && !S->Swallow && Closed && (Keys==(Menu|Up) || Keys==(Menu|Down))) {
        S->Tracking=S->Swallow=true;S->Chord=Keys&~Menu;S->Start=Now;S->Fired=S->Chord==Up;
    }
    if(S->Swallow && (Keys & ~(Menu|S->Chord)))S->Fired=false;
    if(S->Tracking) {
        if(Keys!=(Menu|S->Chord)) {
            // A partial release preserves an already accepted chord, never an unfinished hold.
            if(Keys && Keys!=Menu && Keys!=S->Chord)S->Fired=false;
            S->Tracking=false;
        } else if(S->Chord==Down && (uint32_t)(Now-S->Start)>=1000)S->Fired=true;
    }
    *Consume=S->Swallow || (Keys&Menu)!=0;
    return Event;
}
