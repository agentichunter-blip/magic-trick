"""Exercise the firmware's actual buffer lifetime functions with failed allocators."""
import os, pathlib, subprocess, tempfile
source = (pathlib.Path(__file__).resolve().parents[2] / 'main/cart_backup.c').read_text()
def function(signature):
    start=source.index(signature+'\n{');end=source.index('{',start);depth=0
    for i in range(end,len(source)):
        if source[i]=='{':depth+=1
        if source[i]=='}':
            depth-=1
            if depth==0:return source[start:i+1]
    raise AssertionError(signature)
prelude=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define CONFIG_CHROMAGIC_WIFI_FILES 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
enum { kFileBufferSize=16384 };
static bool Running;
static int StateLock, Calls, FailAt, Live;
static uint8_t (*RomFileStorage)[kFileBufferSize];
static uint8_t (*SaveFileStorage)[kFileBufferSize];
#define RomFileBuffer (*RomFileStorage)
#define SaveFileBuffer (*SaveFileStorage)
static uint8_t *RomBankBuffers[2];
static void *heap_caps_malloc(size_t n, int caps) {
 assert(n==16384 && caps==3); ++Calls;
 if (Calls==FailAt) return NULL;
 void *p=malloc(n); assert(p); ++Live; return p;
}
static void release(void *p) { if(p){--Live;free(p);} }
#define free release
static void EndRun(void);
'''
tests=r'''
int main(void) {
 assert(BeginRun(false)); assert(Running && Calls==0 && Live==0);
 assert(!BeginRun(true)); assert(Calls==0); EndRun(); assert(!Running);
 for(int fail=1;fail<=2;++fail) {
  Calls=0;FailAt=fail;assert(!BeginRun(true));
  assert(!Running && Live==0 && !RomFileStorage && !SaveFileStorage);
  assert(!RomBankBuffers[0] && !RomBankBuffers[1]);
 }
 Calls=0;FailAt=0;assert(BeginRun(true));assert(Live==2 && Calls==2);
 assert(sizeof(RomFileBuffer)==16384 && sizeof(SaveFileBuffer)==16384);
 for(int i=0;i<16384;++i){RomFileBuffer[i]=(uint8_t)i;SaveFileBuffer[i]=(uint8_t)~i;}
 for(int i=0;i<16384;++i){assert(RomBankBuffers[0][i]==(uint8_t)i);assert(RomBankBuffers[1][i]==(uint8_t)~i);}
 assert(!BeginRun(true));assert(Calls==2);EndRun();assert(!Running && Live==0);
 assert(BeginRun(false));assert(Calls==2 && Live==0);EndRun();
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d);(p/'test.c').write_text(prelude+function('static bool BeginRun(bool NeedsBuffers)')+function('static void EndRun(void)')+tests)
 subprocess.run([os.environ.get('CC','cc'),'-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: allocation failure cleanup, exclusive ownership, full bank sizes, maintenance without allocation')
