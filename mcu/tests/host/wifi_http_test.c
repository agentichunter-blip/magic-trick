#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char Root[] = "/tmp/chromagic-http-test-XXXXXX";
#define SDCARD_MOUNT_POINT Root
#include "../../main/wifi_files.c"
static WifiNetworkStatus Network={.Hotspot=true};
static WifiCredentials Submitted;
static bool Forgot;
static WifiScanStatus ScanFixture;
static bool ScanConflict;
esp_err_t WifiNetwork_RequestScan(void) { if(ScanConflict)return ESP_ERR_INVALID_STATE; ScanFixture.Busy=true;return ESP_OK; }
void WifiNetwork_GetScan(WifiScanStatus *Out) { *Out=ScanFixture; }
void WifiNetwork_GetStatus(WifiNetworkStatus *Out) { *Out=Network; }
esp_err_t WifiNetwork_Configure(const WifiCredentials *C) { if(!WifiCredentials_Valid(C))return ESP_ERR_INVALID_ARG; Submitted=*C;return ESP_OK; }
esp_err_t WifiNetwork_Forget(void) { Forgot=true;return ESP_OK; }
static int64_t Now=1000;
static const char *Host = "192.168.4.1", *Origin, *Auth = "test-token";
static char Response[65536], Code[64];
static unsigned char Input[8193];
static size_t Output, Position, InputSize;
static bool Disconnect, Cancel;
static const char *Header(const char *Name) {
    return !strcmp(Name, "Host") ? Host : !strcmp(Name, "Origin") ? Origin : !strcmp(Name,"Content-Type") ? "application/json" : Auth;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *R, const char *N, char *B, size_t S) {
    const char *V = Header(N); if (!V || strlen(V) >= S) return ESP_FAIL; strcpy(B,V); return ESP_OK;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *R, const char *N) { const char *V = Header(N); return V ? strlen(V) : 0; }
esp_err_t httpd_resp_set_status(httpd_req_t *R, const char *V) { snprintf(Code,sizeof(Code),"%s",V); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *R, const char *V) { return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *R, const char *N, const char *V) { return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *R, const char *B, size_t S) {
    assert(Output+S < sizeof(Response)); if(S)memcpy(Response+Output,B,S); Output+=S; Response[Output]=0; return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *R, const char *V) { return httpd_resp_send_chunk(R,V,strlen(V)); }
int httpd_req_recv(httpd_req_t *R, char *B, size_t S) {
    if (Disconnect && Position) return -1;
    size_t N=InputSize-Position; if(N>S)N=S; if(N>1024)N=1024;
    memcpy(B,Input+Position,N); Position+=N;
    if(Cancel)StopRequested=true;
    return N;
}
int64_t esp_timer_get_time(void) { return Now; }
uint32_t esp_rom_crc32_le(uint32_t C,const uint8_t *B,size_t N) { C=~C; for(size_t I=0;I<N;++I) { C^=B[I]; for(unsigned J=0;J<8;++J)C=(C>>1)^(C&1?0xedb88320u:0); }return ~C; }
uint32_t esp_random(void) { static unsigned N; return ++N; }
static void Reset(void) { Output=Position=0; Response[0]=0; strcpy(Code,"200 OK"); Disconnect=Cancel=StopRequested=false; }
static void NotExists(const char *Name) { char P[768]; snprintf(P,sizeof(P),"%s/%s",Root,Name); assert(access(P,F_OK)!=0); }
int main(void) {
    Reset(); Auth=NULL; httpd_req_t ScanReq={.uri="/api/networks"};assert(StartScan(&ScanReq)==ESP_FAIL);Auth="test-token";
    assert(mkdtemp(Root)); strcpy(Token,"test-token"); strcpy(Status.AccessCode,"012345");
    Reset();assert(StartScan(&ScanReq)==ESP_OK && !strcmp(Code,"202 Accepted"));
    Reset();assert(GetScan(&ScanReq)==ESP_OK && strstr(Response,"\"scanning\":true"));
    ScanFixture.Busy=false;ScanFixture.Count=1;strcpy(ScanFixture.Entries[0].SSID,"Home <test>");ScanFixture.Entries[0].RSSI=-40;
    Reset();assert(GetScan(&ScanReq)==ESP_OK && strstr(Response,"Home <test>"));
    ScanConflict=true;Reset();assert(StartScan(&ScanReq)==ESP_FAIL && !strcmp(Code,"409 Conflict"));
    httpd_req_t Login={.uri="/api/session"};
    const char *Bad="{\"code\":\"wrong\"}";
    for(unsigned I=0;I<5;++I) { Reset(); InputSize=strlen(Bad); memcpy(Input,Bad,InputSize); Login.content_len=InputSize; assert(Pair(&Login)==ESP_FAIL && !strcmp(Code,"401 Unauthorized")); }
    Reset(); assert(Pair(&Login)==ESP_FAIL && !strcmp(Code,"429 Too Many Requests"));
    Now+=30000001; Reset();
    const char *Good="{\"code\":\"012345\"}"; InputSize=strlen(Good);memcpy(Input,Good,InputSize);Login.content_len=InputSize;
    assert(Pair(&Login)==ESP_OK && strstr(Response,"test-token"));
    Reset(); Network.Connected=true;strcpy(Network.Address,"192.168.1.22");Host="192.168.1.22"; Origin="http://192.168.1.22";
    assert(GetStatus(&Login)==ESP_OK && strstr(Response,"router_connected\":true") && !strstr(Response,"012345") && !strstr(Response,"test-token"));
    Reset(); Host="192.168.1.22:80"; assert(Authorized(&Login));
    Reset(); Origin="http://192.168.1.23";assert(!Authorized(&Login));
    Host="192.168.4.1";Origin=NULL;Network.Connected=false;
    Reset(); const char *Config="{\"ssid\":\"Home\",\"password\":\"secretpass\"}";
    InputSize=strlen(Config);memcpy(Input,Config,InputSize);Login.content_len=InputSize;
    assert(ConfigureNetwork(&Login)==ESP_OK && !strcmp(Code,"202 Accepted") && !strcmp(Submitted.SSID,"Home") && !strstr(Response,"secretpass"));
    Reset(); const char *Forget="{\"forget\":true}";InputSize=strlen(Forget);memcpy(Input,Forget,InputSize);Login.content_len=InputSize;
    assert(ConfigureNetwork(&Login)==ESP_OK && Forgot);

    for(size_t I=0;I<sizeof(Input);++I)Input[I]=(unsigned char)I;
    httpd_req_t R={.uri="/api/file?path=%2Fgame.gbc",.content_len=sizeof(Input)};
    InputSize=sizeof(Input); Reset(); Auth="wrong";
    assert(Upload(&R)==ESP_FAIL && !strcmp(Code,"401 Unauthorized")); NotExists("game.gbc");
    Auth="test-token"; Reset(); Host="evil.example"; assert(Upload(&R)==ESP_FAIL); NotExists("game.gbc");
    Host="192.168.4.1"; Origin="http://evil.example"; Reset(); assert(Upload(&R)==ESP_FAIL); Origin=NULL;
    Reset(); R.uri="/api/file?path=%2F..%2Fescape"; assert(Upload(&R)==ESP_FAIL && !strcmp(Code,"400 Bad Request"));
    R.uri="/api/file?path=%2Fgame.gbc"; Reset(); Disconnect=true;
    assert(Upload(&R)==ESP_FAIL); NotExists("game.gbc");
    Reset(); Cancel=true; assert(Upload(&R)==ESP_FAIL); NotExists("game.gbc");
    Reset(); assert(Upload(&R)==ESP_OK && !strcmp(Code,"201 Created"));
    Reset(); assert(Upload(&R)==ESP_FAIL && !strcmp(Code,"409 Conflict"));
    Reset(); R.content_len=0; assert(Download(&R)==ESP_OK); assert(Output==sizeof(Input) && !memcmp(Response,Input,sizeof(Input)));
    Reset(); assert(Checksum(&R)==ESP_OK);
    char Expected[9];snprintf(Expected,sizeof(Expected),"%08x",esp_rom_crc32_le(0,Input,sizeof(Input)));assert(strstr(Response,Expected));
    Reset(); R.uri="/api/folder?path=%2FCovers"; assert(MakeDirectory(&R)==ESP_OK);
    Reset(); R.uri="/api/list?path=%2F"; assert(List(&R)==ESP_OK);
    assert(strstr(Response,"game.gbc") && strstr(Response,"Covers") && !strstr(Response,".cmwifi-"));
    /* No partial temp files survive any of the failures above. */
    DIR *D=opendir(Root); struct dirent *E; unsigned Entries=0;
    while((E=readdir(D)))if(strcmp(E->d_name,".")&&strcmp(E->d_name,"..")){assert(!WifiFile_IsInternal(E->d_name));++Entries;}
    closedir(D); assert(Entries==2);
    char P[768]; snprintf(P,sizeof(P),"%s/game.gbc",Root); unlink(P); snprintf(P,sizeof(P),"%s/Covers",Root); rmdir(P); assert(rmdir(Root)==0);
    puts("PASS: HTTP authorization, origin/host checks, traversal, disconnect/cancel, binary upload/download, collision, list, mkdir");
}
