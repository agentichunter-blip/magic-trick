#include <assert.h>
#include <stdio.h>
#include "button.h"
#include "mutex.h"
MutexResult_t Mutex_Take(MutexKey_t Key) { assert(Key==kMutexKey_Buttons); return kMutexResult_Ok; }
MutexResult_t Mutex_Give(MutexKey_t Key) { assert(Key==kMutexKey_Buttons); return kMutexResult_Ok; }
int main(void) {
    Button_ResetAll();
    for(unsigned I=0;I<kNumButtons;++I) {
        Button_Update(0); Button_Update(1u<<I);
        /* Menu telemetry flags are levels, not menu navigation events. */
        assert(Button_GetState((Button_t)I)==(I<8?kButtonState_Pressed:kButtonState_None));
        assert(Button_GetState((Button_t)I)==kButtonState_None);
        Button_Update(1u<<I); assert(Button_GetState((Button_t)I)==kButtonState_None);
        uint16_t Held; Button_GetSnapshot(&Held); assert(Held==(1u<<I));
    }
    Button_Update(0); Button_Update((1u<<kButton_Select)|(1u<<kButton_A));
    assert(Button_GetState(kButton_Select)==kButtonState_Pressed);
    assert(Button_GetState(kButton_A)==kButtonState_Pressed);
    Button_ResetAll(); assert(Button_GetHeldBits()==0 && Button_GetState(kButton_Select)==kButtonState_None);
    assert(Button_GetState((Button_t)999)==kButtonState_None);
    puts("PASS: real button receive path delivers Select, consumes edges once, preserves held levels and chords");
}
