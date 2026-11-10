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
 char branch[kPathSize];strcpy(branch,ActiveSavePath);assert(strcmp(branch,canonical));
 // Normal Stop/autosave path must persist the restored timeline to the branch.
 assert(VirtualCart_Stop()==ESP_OK);verify_save(branch,old,size);
 verify_save(canonical,newer,size);
 // Inputs must remain unchanged as well as the canonical firmware output.
 verify_save(argv[1],old,size);verify_save(argv[2],newer,size);
 unlink(branch);unlink(canonical);assert(!rmdir(dir));
 puts("PASS game-save fixtures: restored branch exact, newer canonical exact, input files unchanged");
 return 0;
}
