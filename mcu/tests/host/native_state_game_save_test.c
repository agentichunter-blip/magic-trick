// Exercise production firmware persistence with externally generated game saves.
// The FPGA command transport remains mocked; no hardware is opened.
#define NATIVE_TEST_RAM_SIZE 32768
#define main synthetic_native_state_main
#include "native_state_test.c"
#undef main

static size_t read_save(const char *path,uint8_t *out) {
 FILE *f=fopen(path,"rb");assert(f);
 size_t size=fread(out,1,sizeof(Ram),f);
 assert(feof(f) || fgetc(f)==EOF);assert(!ferror(f));fclose(f);
 assert(size==8192 || size==32768);return size;
}
static void verify_save(const char *path,const uint8_t *expected,size_t size) {
 uint8_t actual[sizeof(Ram)];assert(read_save(path,actual)==size);
 assert(!memcmp(actual,expected,size));
}
int main(int argc,char **argv) {
 assert(argc==3);
 uint8_t old[sizeof(Ram)],newer[sizeof(Ram)];
 size_t size=read_save(argv[1],old);assert(read_save(argv[2],newer)==size);
 assert(memcmp(old,newer,size));
 char dir[]="/tmp/native-game-save-XXXXXX";assert(mkdtemp(dir));
 Active=true;ActiveInfo.RamSize=size;ActiveInfo.Mapper.HasBattery=true;
 ActiveInfo.Mapper.Kind=size==8192?kCartMapper_Mbc1:kCartMapper_Mbc5;
 snprintf(ActiveSavePath,sizeof(ActiveSavePath),"%s/game.sav",dir);
 char canonical[kPathSize];strcpy(canonical,ActiveSavePath);
 memcpy(Ram,old,size);assert(VirtualCart_StateCapture(false)==ESP_OK);
 memcpy(Ram,newer,size);Dirty=true;
 assert(VirtualCart_StateCapture(true)==ESP_OK);
 assert(!memcmp(Ram,old,size));verify_save(canonical,newer,size);
 assert(!strcmp(ActiveSavePath,canonical));
 char backup[kPathSize];snprintf(backup,sizeof(backup),"%s.before-load-0001.sav",canonical);
 verify_save(backup,newer,size);
 // Normal Stop/autosave path persists the restored save RAM to the main file,
 // which the next start reads; the newer save stays in the backup.
 assert(VirtualCart_Stop()==ESP_OK);verify_save(canonical,old,size);
 verify_save(backup,newer,size);
 // Inputs must remain unchanged.
 verify_save(argv[1],old,size);verify_save(argv[2],newer,size);
 unlink(backup);unlink(canonical);assert(!rmdir(dir));
 puts("PASS game-save fixtures: restored save exact in the main file, newer save exact in its backup, input files unchanged");
 return 0;
}
