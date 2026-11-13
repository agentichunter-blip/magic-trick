#include "native_state_shortcuts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static NativeShortcutState S;
static bool C;
static unsigned Key(unsigned K,unsigned T) {return NativeShortcut_Update(&S,K,T,true,&C);}
int main(void) {
 assert(!Key(0x302,0) && C);assert(!Key(0x202,40) && C);assert(Key(0x200,80)==1 && C);assert(!Key(0x200,120));
 assert(!Key(0x302,160));assert(!Key(0x300,200));assert(Key(0x200,240)==1);
 assert(!Key(0x301,280) && C);assert(Key(0x200,320)==2); /* Menu+Start loads on release, no hold. */
 assert(!Key(0x301,400));assert(!Key(0x300,440));assert(Key(0x200,480)==2); /* Start released first. */
 assert(!Key(0x301,520));assert(!Key(0x201,560) && C);assert(Key(0x200,600)==2); /* Menu released first. */
 assert(!Key(0x301,1520));assert(!Key(0x301,2600));assert(!Key(0x200,2640)); /* Stale telemetry. */
 assert(!Key(0x302,2700));assert(!Key(0x30a,2740));assert(!Key(0x200,2780));
 assert(!Key(0x110,2820));assert(!Key(0,2860)); /* Open menu. */
 assert(!Key(0x302,2900));assert(!Key(0x202,2940));assert(!Key(0x20a,2980));assert(!Key(0x200,3020));
 memset(&S,0,sizeof S);
 for(unsigned t=0;t<=1000;t+=40)assert(!Key(0x301,0xfffffe00u+t));
 assert(Key(0x200,0xfffffe00u+1040)==2); /* Tick wrap. */
 assert(!NativeShortcut_Update(&S,0x302,0,false,&C) && !C);
 puts("PASS shortcut release orders, instant load, stale input, conflicts, open menu, wrap and disabled defaults");
}
