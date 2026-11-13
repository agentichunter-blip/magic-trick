// Run the actual infinite SaveTask for one iteration on a separate host thread.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
static void NativeUnmountHook(void);
static void NativeSaveBlockHook(void);
#define NATIVE_TEST_TASK_CLOCK
#define NATIVE_TEST_UNMOUNT_HOOK
#define NATIVE_TEST_SAVE_BLOCK_HOOK
#define NATIVE_TEST_ALLOW_BACKGROUND_SAVE
#define NATIVE_TEST_RAM_SIZE 32768
#define main native_fixture_main
#include "native_state_test.c"
#undef main
static pthread_mutex_t Gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t Changed = PTHREAD_COND_INITIALIZER;
static bool AtCleanup, ReleaseCleanup;
static bool BlockDuringTransfer;
static _Thread_local bool IsAutosave;
static _Thread_local unsigned SaveIterations;
static pthread_t Worker;
static TickType_t Clock;
static unsigned Waits, ReleaseAfter;
static bool CheckCompetition;
static void block_autosave(void) {
 if (!IsAutosave) return;
 pthread_mutex_lock(&Gate);AtCleanup=true;pthread_cond_broadcast(&Changed);
 while(!ReleaseCleanup)pthread_cond_wait(&Changed,&Gate);
 pthread_mutex_unlock(&Gate);
}
static void NativeUnmountHook(void) { if(!BlockDuringTransfer) block_autosave(); }
static void NativeSaveBlockHook(void) { if(BlockDuringTransfer && Block==0) block_autosave(); }
static void release_save(void) {
 pthread_mutex_lock(&Gate);ReleaseCleanup=true;pthread_cond_broadcast(&Changed);pthread_mutex_unlock(&Gate);
 assert(!pthread_join(Worker,NULL));
}
void vTaskDelay(TickType_t t) {
 if(IsAutosave){if(SaveIterations++)pthread_exit(NULL);return;}
 Clock+=t;++Waits;
 assert(NativeRequested && Running && AutosaveRunning);
 assert(!LoadCommands); // No restore, mount or bulk transfer may overlap cleanup.
 assert(Mounted && Bulk==BlockDuringTransfer);
 if(CheckCompetition) {
  assert(!SetRunning(true));assert(!BeginAutosave());assert(!SetLoadRequested(true));
  assert(VirtualCart_StateCapture(true)==ESP_ERR_INVALID_STATE);
  assert(VirtualCart_Stop()==ESP_ERR_INVALID_STATE);
  CheckCompetition=false;
 }
 if(ReleaseAfter && Waits==ReleaseAfter) {
  release_save();
  assert(!Running && NativeRequested && !AutosaveRunning);
  // Exercise the idle handoff window, not just rejection while Running is set.
  assert(!BeginAutosave());assert(!SetRunning(true));assert(!SetLoadRequested(true));
  assert(VirtualCart_StateCapture(true)==ESP_ERR_INVALID_STATE);
 }
}
TickType_t xTaskGetTickCount(void){return Clock;}
static void *autosave(void *unused){(void)unused;IsAutosave=true;SaveTask(NULL);return NULL;}
static void overlap(bool load, bool timeout, TickType_t start, bool transfer) {
 BlockDuringTransfer=transfer;
 char dir[]="/tmp/native-autosave-XXXXXX";assert(mkdtemp(dir));
 Active=true;ActiveInfo.RamSize=sizeof(Ram);ActiveInfo.Mapper.Kind=kCartMapper_Mbc5;ActiveInfo.Mapper.HasBattery=true;
 StateUncertain=false;NativeRequested=false;Valid=false;
 AtCleanup=ReleaseCleanup=false;Waits=LoadCommands=0;Clock=start;
 ReleaseAfter=timeout?0:5;CheckCompetition=true;Dirty=true;
 snprintf(ActiveSavePath,sizeof(ActiveSavePath),"%s/game.sav",dir);
 char canonical[kPathSize];strcpy(canonical,ActiveSavePath);
 memset(Ram,0x11,sizeof(Ram));assert(VirtualCart_StateCapture(false)==ESP_OK);
 memset(Ram,0x77,sizeof(Ram));Dirty=true;assert(!pthread_create(&Worker,NULL,autosave,NULL));
 pthread_mutex_lock(&Gate);while(!AtCleanup)pthread_cond_wait(&Changed,&Gate);pthread_mutex_unlock(&Gate);
 assert(Running && AutosaveRunning && Mounted && Bulk==transfer);verify(canonical,transfer?0x11:0x77);
 esp_err_t result=VirtualCart_StateCapture(load);
 if(timeout) {
  assert(result==ESP_ERR_TIMEOUT && (TickType_t)(Clock-start)>=kNativeAutosaveWait_ms);
  assert(!NativeRequested && !StateUncertain && Running && !LoadCommands);
  assert(OldState[0]==0x11);verify(canonical,transfer?0x11:0x77);
  release_save();assert(!Running && !AutosaveRunning && !Mounted && !Bulk);
  assert(VirtualCart_StateCapture(load)==ESP_OK); // Timeout did not poison the slot or owner.
 } else assert(result==ESP_OK && Waits==5);
 assert(!Running && !AutosaveRunning && !NativeRequested && !Mounted && !Bulk && !StateUncertain);
 verify(canonical,0x77);
 if(load) {
  assert(LoadCommands==1 && Ram[0]==0x11 && Ram[sizeof(Ram)-1]==0x11);
  assert(!strcmp(canonical,ActiveSavePath));
 } else assert(!LoadCommands && OldState[0]==0x77);
 char backup[kPathSize];snprintf(backup,sizeof(backup),"%s.before-load-0001.sav",canonical);
 assert(VirtualCart_Stop()==ESP_OK);
 // After a load the restored save RAM is in the main file and the newer save
 // it replaced is in the backup.
 if(load){verify(canonical,0x11);verify(backup,0x77);} else verify(canonical,0x77);
 DIR *d=opendir(dir);assert(d);struct dirent *e;
 while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")){
  char path[512];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));
 }
 closedir(d);assert(!rmdir(dir));
 printf("PASS real autosave overlap: %s %s clock=%u transfer=%u\n",load?"load":"capture",timeout?"timeout/retry":"wait/restore",start,transfer);
}
int main(void) {
 overlap(true,false,0,false);
 overlap(true,false,0,true);
 overlap(true,false,UINT32_MAX-25,false); // Tick wrap while waiting.
 overlap(false,false,0,false);
 overlap(true,true,0,false);
 overlap(true,true,0,true);
 unsigned before=Waits;
 assert(SetRunning(true));assert(VirtualCart_StateCapture(true)==ESP_ERR_INVALID_STATE);assert(SetRunning(false));
 assert(SetLoadRequested(true));assert(VirtualCart_StateCapture(true)==ESP_ERR_INVALID_STATE);assert(SetLoadRequested(false));
 StateUncertain=true;assert(VirtualCart_StateCapture(true)==ESP_ERR_INVALID_STATE);StateUncertain=false;
 assert(Waits==before && !NativeRequested && !Running);
 puts("PASS autosave cleanup wait, exclusive handoff, newer battery preservation, timeout retry, tick wrap and conflicting-operation rejection");
}
