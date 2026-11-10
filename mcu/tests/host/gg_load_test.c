#define main gb_regression_main
#include "virtual_load_test.c"
#undef main
int main(int argc,char **argv) {
 char base[]="/tmp/gg-loader-XXXXXX";int fd=mkstemp(base);assert(fd>=0);close(fd);
 char path[128];snprintf(path,sizeof(path),"%s.gg",base);assert(!rename(base,path));
 FILE *rom=fopen(path,"wb");assert(rom);
 uint8_t data[32768];for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i^(i>>8));
 memcpy(data+0x7ff0,"TMR SEGA",8);data[0x7fff]=0x60;
 assert(fwrite(data,1,sizeof(data),rom)==sizeof(data));assert(!fclose(rom));
 const char *input=argc>1?argv[1]:path;
 ExpectedRom=fopen(input,"rb");assert(ExpectedRom);
 assert(GGRom_Inspect(ExpectedRom,&ExpectedRomSize)==ESP_OK);
 char resolved[kPathSize];assert(ResolvePath("test.GG",resolved)==ESP_OK);
 /* Legacy FPGA rejection occurs before reset, writes or cheat processing. */
 FailCheats=false;
 assert(RunLoad(input)==ESP_ERR_NOT_SUPPORTED && !Writes && !Prepares && !Starts);
 GGCore=true;Writes=0;
 FailCheats=true;assert(RunLoad(input)==ESP_ERR_INVALID_CRC && !Prepares && !Writes);FailCheats=false;
 assert(RunLoad(input)==ESP_OK && Active && ActiveInfo.GameGear);
 assert(Writes==4096 && Starts==1 && Remembered==1 && RecentRemembered==1);
 assert(ActiveInfo.RamSize==0 && ActiveInfo.Mapper.Kind==kCartMapper_RomOnly);
 bool supported=true,valid=true,uncertain=true;uint32_t session=0;
 assert(VirtualCart_NativeInfo(&supported,&valid,&uncertain,&session)==ESP_OK);
 assert(!supported && !valid && !uncertain);
 ActiveInfo.Mapper.Kind=kCartMapper_Mbc1; /* Core identity must override mapper aliases. */
 assert(VirtualCart_NativeInfo(&supported,&valid,&uncertain,&session)==ESP_OK && !supported);
 ActiveInfo.Mapper.Kind=kCartMapper_RomOnly;
 SetActive(false);ClearActiveSave();Started=false;
 Writes=0;GGFailWrite=7;unsigned starts=Starts;
 assert(RunLoad(input)==ESP_FAIL && Writes==7 && Starts==starts && !Active && !Started);
 GGFailWrite=0;GGNoFrame=true;Writes=0;
 assert(RunLoad(input)==ESP_ERR_TIMEOUT && !Active && !Started);
 GGNoFrame=false;GGReset=true;Writes=0;
 assert(RunLoad(input)==ESP_ERR_TIMEOUT && !Active && !Started);
 GGReset=false;GGShortReply=true;Writes=0;unsigned prepares=Prepares;
 assert(RunLoad(input)==ESP_ERR_INVALID_RESPONSE && !Writes && Prepares==prepares);
 GGShortReply=false;
 /* A GB ROM is rejected by this GG image before writes/preparation. */
 memcpy(data+0x104,NintendoLogo,sizeof(NintendoLogo));memset(data+0x134,0,0x1c);
 for(unsigned i=0x134;i<=0x14c;i++) data[0x14d]=(uint8_t)(data[0x14d]-data[i]-1);
 rom=fopen(base,"wb");assert(rom && fwrite(data,1,sizeof(data),rom)==sizeof(data));fclose(rom);
 FailCheats=false;assert(RunLoad(base)==ESP_ERR_NOT_SUPPORTED && !Writes && Prepares==prepares);
 /* Reject SMS, bad signature, and a copier-header/non-power-of-two image. */
 rom=fopen(path,"r+b");assert(rom);fseek(rom,0x7fff,SEEK_SET);fputc(0x40,rom);fflush(rom);
 uint32_t size;assert(GGRom_Inspect(rom,&size)==ESP_ERR_NOT_SUPPORTED);
 fseek(rom,0x7ff0,SEEK_SET);fputc(0,rom);fflush(rom);
 assert(GGRom_Inspect(rom,&size)==ESP_ERR_INVALID_RESPONSE);
 fseek(rom,0,SEEK_END);fputc(0,rom);fflush(rom);assert(GGRom_Inspect(rom,&size)==ESP_ERR_INVALID_SIZE);
 fclose(rom);
 /* Cover all accepted sizes, each GG region and the three header locations. */
 for(uint32_t extent=8192;extent<=4u*1024*1024;extent*=2) {
  for(unsigned region=5;region<=7;region++) {
   rom=tmpfile();assert(rom);
   assert(!fseek(rom,extent-1,SEEK_SET));assert(fputc(0,rom)!=EOF);
   long header=extent>=32768?0x7ff0:extent>=16384?0x3ff0:0x1ff0;
   assert(!fseek(rom,header,SEEK_SET));assert(fwrite("TMR SEGA",1,8,rom)==8);
   assert(!fseek(rom,header+15,SEEK_SET));assert(fputc(region<<4,rom)!=EOF);fflush(rom);
   assert(GGRom_Inspect(rom,&size)==ESP_OK && size==extent && ftell(rom)==0);
   fclose(rom);
  }
 }
 rom=tmpfile();assert(rom);assert(!fseek(rom,8u*1024*1024-1,SEEK_SET));fputc(0,rom);fflush(rom);
 assert(GGRom_Inspect(rom,&size)==ESP_ERR_INVALID_SIZE);fclose(rom);
 fclose(ExpectedRom);unlink(path);unlink(base);
 assert(!Mounted && !Bulk && !SleepLocks);
 puts("PASS: GG source-byte mirroring, identity, fresh-frame wait, timeout/reset, short response, upload failure, unsupported headers and cleanup");
}
