#include "native_state_shortcuts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static NativeShortcutState S;
static bool C;
static unsigned Key(unsigned K,unsigned T) {return NativeShortcut_Update(&S,K,T,true,&C);}
int main(void) {
 assert(!Key(0x310,0) && C);assert(!Key(0x210,40) && C);assert(Key(0x200,80)==1 && C);assert(!Key(0x200,120));
 assert(!Key(0x310,160));assert(!Key(0x300,200));assert(Key(0x200,240)==1);
 assert(!Key(0x380,280));assert(!Key(0x200,320)); /* Short tap cannot load. */
 for(unsigned t=400;t<=1400;t+=40)assert(!Key(0x380,t));
 assert(!Key(0x280,1440) && C);assert(Key(0x200,1480)==2);
 assert(!Key(0x380,1520));assert(!Key(0x380,2600));assert(!Key(0x200,2640)); /* Stale telemetry. */
 assert(!Key(0x310,2700));assert(!Key(0x318,2740));assert(!Key(0x200,2780));
 assert(!Key(0x110,2820));assert(!Key(0,2860)); /* Open menu. */
 assert(!Key(0x310,2900));assert(!Key(0x210,2940));assert(!Key(0x218,2980));assert(!Key(0x200,3020));
 memset(&S,0,sizeof S);
 for(unsigned t=0;t<=1000;t+=40)assert(!Key(0x380,0xfffffe00u+t));
 assert(Key(0x200,0xfffffe00u+1040)==2); /* Tick wrap. */
 assert(!NativeShortcut_Update(&S,0x310,0,false,&C) && !C);
 puts("PASS shortcut release orders, hold duration, stale input, conflicts, open menu, wrap and disabled defaults");
}
