#include "wifi_files.h"
#include "wifi_files_ui.h"
#include "wifi_file_store.h"
#include "wifi_network.h"
#include "esp_rom_crc.h"
#include <ctype.h>
#include "pc_backup.h"
#include "cart_backup.h"
#include "sd_card.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Off at boot. One HTTP task owns all file handles; one control task owns the
 * SD mount and PC-mode mutex. Stop drains HTTP before unmounting/releasing. */
static portMUX_TYPE Lock = portMUX_INITIALIZER_UNLOCKED;
static WifiFilesStatus Status;
static bool StopRequested;
static int64_t LastActivity;
static char Token[33];
static httpd_handle_t Server;

static unsigned PairFailures;
static int64_t PairBlockedUntil;
extern const unsigned char PageStart[] asm("_binary_wifi_files_html_start");
extern const unsigned char PageEnd[] asm("_binary_wifi_files_html_end");

static void Snapshot(WifiFilesStatus *Out)
{
    portENTER_CRITICAL(&Lock); *Out = Status; portEXIT_CRITICAL(&Lock);
    WifiNetwork_GetStatus(&Out->Network);
}
static void Message(const char *Text)
{
    portENTER_CRITICAL(&Lock);
    snprintf(Status.Message, sizeof(Status.Message), "%s", Text);
    portEXIT_CRITICAL(&Lock);
}
static bool Stopping(void)
{
    portENTER_CRITICAL(&Lock); bool Stop = StopRequested; portEXIT_CRITICAL(&Lock);
    return Stop;
}
static void Touch(void)
{
    portENTER_CRITICAL(&Lock); LastActivity = esp_timer_get_time(); portEXIT_CRITICAL(&Lock);
}
static esp_err_t Json(httpd_req_t *Req, cJSON *Value)
{
    if(!Value) return ESP_FAIL;
    char *Text=cJSON_PrintUnformatted(Value); cJSON_Delete(Value);
    if(!Text) return ESP_FAIL;
    httpd_resp_set_type(Req,"application/json");
    httpd_resp_set_hdr(Req,"Cache-Control","no-store");
    httpd_resp_set_hdr(Req,"X-Content-Type-Options","nosniff");
    esp_err_t R=httpd_resp_sendstr(Req,Text); free(Text); return R;
}
static esp_err_t Error(httpd_req_t *Req, const char *Code, const char *Text)
{
    httpd_resp_set_status(Req, Code);
    httpd_resp_set_hdr(Req, "Connection", "close");
    cJSON *Value=cJSON_CreateObject();
    if(Value) { cJSON_AddStringToObject(Value,"error",Text); (void)Json(Req,Value); }
    return ESP_FAIL; /* Never reinterpret an unread PUT body as another request. */
}
static bool LocalRequest(httpd_req_t *Req)
{
    char Host[48], Origin[64], Expected[64];
    if(Stopping() || httpd_req_get_hdr_value_str(Req,"Host",Host,sizeof(Host)) != ESP_OK) return false;
    char Address[48]; snprintf(Address,sizeof(Address),"%s",Host);
    char *Port=strchr(Address,':');
    if(Port) { if(strcmp(Port,":80")) return false; *Port=0; }
    WifiNetworkStatus Network; WifiNetwork_GetStatus(&Network);
    if(!(Network.Hotspot && !strcmp(Address,"192.168.4.1")) &&
       !(Network.Connected && !strcmp(Address,Network.Address))) return false;
    if(httpd_req_get_hdr_value_len(Req,"Origin"))
    {
        if(httpd_req_get_hdr_value_str(Req,"Origin",Origin,sizeof(Origin)) != ESP_OK) return false;
        snprintf(Expected,sizeof(Expected),"http://%s",Address);
        if(strcmp(Origin,Expected))
        {
            snprintf(Expected,sizeof(Expected),"http://%s:80",Address);
            if(strcmp(Origin,Expected)) return false;
        }
    }
    return true;
}
static bool Authorized(httpd_req_t *Req)
{
    char Supplied[40];
    if(!LocalRequest(Req) || httpd_req_get_hdr_value_str(Req,"X-ChroMagic-Token",Supplied,sizeof(Supplied)) != ESP_OK ||
       !Token[0] || strcmp(Token,Supplied)) return false;
    Touch();
    httpd_resp_set_hdr(Req,"Cache-Control","no-store");
    httpd_resp_set_hdr(Req,"X-Content-Type-Options","nosniff");
    return true;
}
static cJSON *ReadJson(httpd_req_t *Req)
{
    char Type[64], Body[384];
    if(!Req->content_len || Req->content_len >= sizeof(Body) ||
       httpd_req_get_hdr_value_str(Req,"Content-Type",Type,sizeof(Type)) != ESP_OK ||
       (strcmp(Type,"application/json") && strcmp(Type,"application/json; charset=utf-8"))) return NULL;
    size_t Read=0;
    while(Read < Req->content_len && !Stopping())
    {
        int N=httpd_req_recv(Req,Body+Read,Req->content_len-Read);
        if(N<=0) return NULL;
        Read+=N;
    }
    if(Read != Req->content_len || memchr(Body,0,Read)) return NULL;
    Body[Read]=0;
    if(strstr(Body,"\\u0000")) { memset(Body,0,sizeof(Body)); return NULL; }
    cJSON *Value=cJSON_ParseWithLengthOpts(Body,Read+1,NULL,true);
    memset(Body,0,sizeof(Body));
    if(!cJSON_IsObject(Value)) { cJSON_Delete(Value); return NULL; }
    return Value;
}
static esp_err_t Pair(httpd_req_t *Req)
{
    if(!LocalRequest(Req)) return Error(Req,"403 Forbidden","Use the address shown on the Chromatic.");
    int64_t Now=esp_timer_get_time();
    if(Now < PairBlockedUntil) return Error(Req,"429 Too Many Requests","Wait 30 seconds before pairing again.");
    cJSON *Body=ReadJson(Req);
    cJSON *Code=cJSON_GetObjectItemCaseSensitive(Body,"code");
    char Supplied[13]={0};
    if(cJSON_IsString(Code) && strlen(Code->valuestring)==6)
        for(unsigned I=0;I<6;++I) Supplied[I]=Code->valuestring[I];
    cJSON_Delete(Body);
    WifiFilesStatus Current; Snapshot(&Current);
    if(!Supplied[0] || strcmp(Supplied,Current.AccessCode))
    {
        if(++PairFailures >= 5) { PairFailures=0; PairBlockedUntil=Now+30000000; }
        return Error(Req,"401 Unauthorized","Press A on the Chromatic for its six-digit WEB CODE. The Wi-Fi password is separate.");
    }
    PairFailures=0; Touch();
    cJSON *Reply=cJSON_CreateObject();
    if(Reply) { cJSON_AddStringToObject(Reply,"token",Token); cJSON_AddNumberToObject(Reply,"api_version",1); }
    return Json(Req,Reply);
}
static esp_err_t GetStatus(httpd_req_t *Req)
{
    if(!Authorized(Req)) return Error(Req,"401 Unauthorized","Pair with the Chromatic first.");
    WifiFilesStatus Current; Snapshot(&Current);
    WifiNetworkStatus *N=&Current.Network;
    cJSON *Reply=cJSON_CreateObject();
    if(!Reply) return ESP_FAIL;
    cJSON_AddNumberToObject(Reply,"api_version",1);
    cJSON_AddStringToObject(Reply,"mode",N->Connected ? "router" : N->Hotspot ? "hotspot" : "connecting");
    cJSON_AddBoolToObject(Reply,"router_connected",N->Connected);
    cJSON_AddBoolToObject(Reply,"network_saved",N->Configured);
    cJSON_AddStringToObject(Reply,"router_ssid",N->SSID);
    cJSON_AddStringToObject(Reply,"router_ip",N->Connected ? N->Address : "");
    cJSON_AddBoolToObject(Reply,"hotspot_active",N->Hotspot);
    cJSON_AddStringToObject(Reply,"hotspot_ssid",N->HotspotSSID);
    cJSON_AddStringToObject(Reply,"hotspot_ip",N->Hotspot ? "192.168.4.1" : "");
    cJSON_AddStringToObject(Reply,"message",N->Message);
    cJSON_AddNumberToObject(Reply,"max_upload_bytes",128*1024*1024);
    cJSON_AddBoolToObject(Reply,"overwrite_supported",false);
    cJSON_AddNumberToObject(Reply,"idle_timeout_seconds",600);
    return Json(Req,Reply);
}
static esp_err_t StartScan(httpd_req_t *Req)
{
    if(!Authorized(Req)) return Error(Req,"401 Unauthorized","Pair with the Chromatic first.");
    if(WifiNetwork_RequestScan()!=ESP_OK) return Error(Req,"409 Conflict","A scan or network change is already in progress.");
    httpd_resp_set_status(Req,"202 Accepted"); httpd_resp_set_type(Req,"application/json");
    return httpd_resp_sendstr(Req,"{\"scanning\":true}");
}
static esp_err_t GetScan(httpd_req_t *Req)
{
    if(!Authorized(Req)) return Error(Req,"401 Unauthorized","Pair with the Chromatic first.");
    WifiScanStatus Scan; WifiNetwork_GetScan(&Scan);
    cJSON *Reply=cJSON_CreateObject(); if(!Reply) return ESP_FAIL;
    cJSON_AddBoolToObject(Reply,"scanning",Scan.Busy);
    cJSON_AddStringToObject(Reply,"error",Scan.Error);
    cJSON *List=cJSON_AddArrayToObject(Reply,"networks");
    for(unsigned I=0;List && I<Scan.Count;++I)
    {
        WifiScanEntry *E=&Scan.Entries[I]; cJSON *N=cJSON_CreateObject(); if(!N) break;
        cJSON_AddStringToObject(N,"ssid",E->SSID); cJSON_AddNumberToObject(N,"rssi",E->RSSI);
        cJSON_AddBoolToObject(N,"secure",E->Secure); cJSON_AddBoolToObject(N,"supported",E->Supported);
        cJSON_AddItemToArray(List,N);
    }
    return Json(Req,Reply);
}
static esp_err_t ConfigureNetwork(httpd_req_t *Req)
{
    if(!Authorized(Req)) return Error(Req,"401 Unauthorized","Pair with the Chromatic first.");
    cJSON *Body=ReadJson(Req);
    if(!Body) return Error(Req,"400 Bad Request","Expected a JSON network configuration.");
    esp_err_t R;
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(Body,"forget"))) R=WifiNetwork_Forget();
    else
    {
        cJSON *SSID=cJSON_GetObjectItemCaseSensitive(Body,"ssid"), *Password=cJSON_GetObjectItemCaseSensitive(Body,"password");
        WifiCredentials Credentials={0};
        bool Valid=cJSON_IsString(SSID) && cJSON_IsString(Password) && strlen(SSID->valuestring)<=32 && strlen(Password->valuestring)<=64;
        if(Valid) { strcpy(Credentials.SSID,SSID->valuestring); strcpy(Credentials.Password,Password->valuestring); }
        R=Valid ? WifiNetwork_Configure(&Credentials) : ESP_ERR_INVALID_ARG;
        memset(&Credentials,0,sizeof(Credentials));
        if(cJSON_IsString(Password)) memset(Password->valuestring,0,strlen(Password->valuestring));
    }
    cJSON_Delete(Body);
    if(R == ESP_ERR_INVALID_STATE) return Error(Req,"409 Conflict","Wait for the network scan to finish, then try again.");
    if(R != ESP_OK) return Error(Req,R==ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error",
        R==ESP_ERR_INVALID_ARG ? "Use a 1-32 byte SSID and a valid WPA2 password (or empty for an open network)." : "Could not save Wi-Fi settings.");
    httpd_resp_set_status(Req,"202 Accepted");
    httpd_resp_set_type(Req,"application/json");
    return httpd_resp_sendstr(Req,"{\"ok\":true,\"message\":\"Settings saved; connection change queued.\"}");
}
static bool Path(httpd_req_t *Req, char *Out)
{
    /* The only query parameter is path. Never decode twice. */
    const char *Query = strchr(Req->uri, '?');
    char Relative[WIFI_FILE_PATH_SIZE];
    return Query && strncmp(Query + 1, "path=", 5) == 0 && !strchr(Query + 6, '&') &&
        WifiFile_DecodePath(Query + 6, Relative, sizeof(Relative)) &&
        WifiFile_Join(SDCARD_MOUNT_POINT, Relative, Out, WIFI_FILE_PATH_SIZE);
}
static esp_err_t Home(httpd_req_t *Req)
{
    if(!LocalRequest(Req)) return Error(Req,"403 Forbidden","Use the address shown on the Chromatic.");
    Touch();
    httpd_resp_set_type(Req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(Req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(Req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(Req, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' blob: data:; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
    /* The public page contains no credentials or access tokens. */
    return httpd_resp_send(Req,(const char *)PageStart,PageEnd-PageStart-1);
}

static esp_err_t List(httpd_req_t *Req)
{
    if (!Authorized(Req)) return Error(Req, "401 Unauthorized", "Pair with the Chromatic first.");
    char Folder[WIFI_FILE_PATH_SIZE], Child[WIFI_FILE_PATH_SIZE];
    if (!Path(Req, Folder)) return Error(Req, "400 Bad Request", "Invalid folder path.");
    DIR *Dir = opendir(Folder);
    if (!Dir) return Error(Req, "404 Not Found", "Folder unavailable.");
    httpd_resp_set_type(Req, "application/x-ndjson");
    struct dirent *Entry;
    esp_err_t Result = ESP_OK;
    while (!Stopping() && (Entry = readdir(Dir)))
    {
        if (!strcmp(Entry->d_name, ".") || !strcmp(Entry->d_name, "..") || WifiFile_IsInternal(Entry->d_name)) continue;
        int N = snprintf(Child, sizeof(Child), "%s/%s", Folder, Entry->d_name);
        struct stat Info;
        if (N < 0 || N >= sizeof(Child) || stat(Child, &Info) != 0) continue;
        cJSON *Row = cJSON_CreateObject();
        if (!Row) { Result = ESP_ERR_NO_MEM; break; }
        bool Ok = cJSON_AddStringToObject(Row, "name", Entry->d_name) &&
            cJSON_AddBoolToObject(Row, "directory", S_ISDIR(Info.st_mode)) &&
            cJSON_AddNumberToObject(Row, "size", Info.st_size);
        char *JSON = Ok ? cJSON_PrintUnformatted(Row) : NULL;
        cJSON_Delete(Row);
        if (!JSON) { Result = ESP_ERR_NO_MEM; break; }
        Result = httpd_resp_send_chunk(Req, JSON, strlen(JSON)); free(JSON);
        if (Result == ESP_OK) Result = httpd_resp_send_chunk(Req, "\n", 1);
        if (Result != ESP_OK) break;
        Touch();
    }
    closedir(Dir);
    if (Result != ESP_OK || Stopping()) return ESP_FAIL;
    return httpd_resp_send_chunk(Req, NULL, 0);
}
static esp_err_t Download(httpd_req_t *Req)
{
    if (!Authorized(Req)) return Error(Req, "401 Unauthorized", "Pair with the Chromatic first.");
    char Name[WIFI_FILE_PATH_SIZE];
    if (!Path(Req, Name)) return Error(Req, "400 Bad Request", "Invalid file path.");
    struct stat Info;
    if (stat(Name, &Info) != 0 || !S_ISREG(Info.st_mode)) return Error(Req, "404 Not Found", "File unavailable.");
    FILE *File = fopen(Name, "rb");
    if (!File) return Error(Req, "500 Internal Server Error", "Could not open file.");
    char *Buffer = malloc(4096);
    if (!Buffer) { fclose(File); return Error(Req, "503 Service Unavailable", "Not enough memory."); }
    httpd_resp_set_type(Req, "application/octet-stream");
    httpd_resp_set_hdr(Req, "Content-Disposition", "attachment");
    esp_err_t Result = ESP_OK;
    size_t Count, Total = 0;
    while (!Stopping() && (Count = fread(Buffer, 1, 4096, File)))
    {
        Result = httpd_resp_send_chunk(Req, Buffer, Count);
        if (Result != ESP_OK) break;
        Total += Count; Touch();
    }
    if (ferror(File) || Stopping() || Total != Info.st_size) Result = ESP_FAIL;
    if (fclose(File) != 0) Result = ESP_FAIL;
    free(Buffer);
    return Result == ESP_OK ? httpd_resp_send_chunk(Req, NULL, 0) : Result;
}
enum { kUploadStallLimit = 6 };   /* about 30 s without data ends an upload */

static esp_err_t Upload(httpd_req_t *Req)
{
    if (!Authorized(Req)) return Error(Req, "401 Unauthorized", "Pair with the Chromatic first.");
    if (Req->content_len > 128 * 1024 * 1024) return Error(Req, "413 Content Too Large", "Maximum file size is 128 MB.");
    char Name[WIFI_FILE_PATH_SIZE];
    if (!Path(Req, Name)) return Error(Req, "400 Bad Request", "Invalid file path.");
    struct stat Info;
    if (stat(Name, &Info) == 0) return Error(Req, "409 Conflict", "A file or folder with this name already exists. Existing files are kept.");
    WifiFileUpload *Transfer = calloc(1, sizeof(*Transfer));
    char *Buffer = malloc(4096);
    if (!Transfer || !Buffer) { free(Transfer); free(Buffer); return Error(Req, "503 Service Unavailable", "Not enough memory."); }
    bool Ok = WifiFile_Begin(Transfer, Name, Req->content_len, esp_random());
    unsigned Stalls = 0;
    while (Ok && Transfer->Written < Transfer->Expected && !Stopping())
    {
        size_t Left = Transfer->Expected - Transfer->Written;
        int Count = httpd_req_recv(Req, Buffer, Left < 4096 ? Left : 4096);
        /* Each receive waits recv_wait_timeout (5 s). Weak Wi-Fi or a slow SD
         * write can pause a batch that long, so allow a few quiet periods. */
        if (Count == HTTPD_SOCK_ERR_TIMEOUT && ++Stalls < kUploadStallLimit) continue;
        if (Count <= 0) { Ok = false; break; }
        Stalls = 0;
        Ok = WifiFile_Write(Transfer, Buffer, Count); Touch();
    }
    Ok = Ok && !Stopping();
    if (Ok) Ok = WifiFile_Finish(Transfer);
    else WifiFile_Abort(Transfer);
    free(Transfer); free(Buffer);
    if (!Ok) return Error(Req, "500 Internal Server Error", "Upload incomplete. Check SD space and retry; existing files were kept.");
    httpd_resp_set_status(Req, "201 Created");
    httpd_resp_set_type(Req,"application/json");
    return httpd_resp_sendstr(Req,"{\"ok\":true}");
}
static esp_err_t MakeDirectory(httpd_req_t *Req)
{
    if (!Authorized(Req)) return Error(Req, "401 Unauthorized", "Pair with the Chromatic first.");
    char Name[WIFI_FILE_PATH_SIZE];
    if (!Path(Req, Name) || Req->content_len) return Error(Req, "400 Bad Request", "Invalid folder request.");
    if (mkdir(Name, 0775) != 0) return Error(Req, "409 Conflict", "Could not create folder. Check its name, parent and SD space.");
    httpd_resp_set_status(Req, "201 Created");
    httpd_resp_set_type(Req,"application/json");
    return httpd_resp_sendstr(Req,"{\"ok\":true}");
}
static esp_err_t Checksum(httpd_req_t *Req)
{
    if(!Authorized(Req)) return Error(Req,"401 Unauthorized","Pair with the Chromatic first.");
    char Name[WIFI_FILE_PATH_SIZE];
    if(!Path(Req,Name)) return Error(Req,"400 Bad Request","Invalid file path.");
    struct stat Info;
    if(stat(Name,&Info) || !S_ISREG(Info.st_mode)) return Error(Req,"404 Not Found","File unavailable.");
    FILE *File=fopen(Name,"rb"); char *Buffer=malloc(4096);
    if(!File || !Buffer) { if(File)fclose(File); free(Buffer); return Error(Req,"503 Service Unavailable","File or memory unavailable."); }
    uint32_t CRC=0; size_t Total=0, Count;
    while(!Stopping() && (Count=fread(Buffer,1,4096,File)))
    { CRC=esp_rom_crc32_le(CRC,(const uint8_t *)Buffer,Count); Total+=Count; Touch(); }
    bool Ok=!Stopping() && !ferror(File) && Total==Info.st_size;
    if(fclose(File)) Ok=false;
    free(Buffer);
    if(!Ok) return Error(Req,"500 Internal Server Error","Could not checksum the complete file.");
    char Hex[9]; snprintf(Hex,sizeof(Hex),"%08lx",(unsigned long)CRC);
    cJSON *Reply=cJSON_CreateObject();
    if(Reply) { cJSON_AddStringToObject(Reply,"crc32",Hex); cJSON_AddNumberToObject(Reply,"bytes",Total); }
    return Json(Req,Reply);
}
static esp_err_t StartServer(void)
{
    httpd_config_t Config = HTTPD_DEFAULT_CONFIG();
    Config.stack_size = 8192;
    Config.max_open_sockets = 2;
    Config.lru_purge_enable = true;
    Config.recv_wait_timeout = 5; Config.send_wait_timeout = 5;
    Config.max_uri_handlers = 11;
    esp_err_t Result = httpd_start(&Server, &Config);
    if (Result != ESP_OK) return Result;
    const httpd_uri_t Routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=Home},
        {.uri="/api/session", .method=HTTP_POST, .handler=Pair},
        {.uri="/api/status", .method=HTTP_GET, .handler=GetStatus},
        {.uri="/api/networks", .method=HTTP_POST, .handler=StartScan},
        {.uri="/api/networks", .method=HTTP_GET, .handler=GetScan},
        {.uri="/api/network", .method=HTTP_POST, .handler=ConfigureNetwork},
        {.uri="/api/checksum", .method=HTTP_GET, .handler=Checksum},
        {.uri="/api/list", .method=HTTP_GET, .handler=List},
        {.uri="/api/file", .method=HTTP_GET, .handler=Download},
        {.uri="/api/file", .method=HTTP_PUT, .handler=Upload},
        {.uri="/api/folder", .method=HTTP_POST, .handler=MakeDirectory},
    };
    for (unsigned I = 0; I < sizeof(Routes)/sizeof(Routes[0]); ++I)
        if ((Result = httpd_register_uri_handler(Server, &Routes[I])) != ESP_OK) return Result;
    return ESP_OK;
}
static void TransferTask(void *Arg)
{
    (void)Arg;
    sdmmc_card_t *Card = NULL;
    bool Lease = false, Wifi = false;
    const char *Failure = "COULD NOT START";
    esp_err_t Result = PCBackup_BeginWireless();
    if (Result != ESP_OK) { Failure = "BUSY: STOP USB TRANSFERS"; goto Cleanup; }
    Lease = true;
    if (Stopping()) goto Cleanup;
    printf("WIFI_FILES before_sd heap8=%u largest_dma=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    Result = SDCard_Mount(&Card);
    if (Result != ESP_OK) { Failure = "SD CARD UNAVAILABLE"; goto Cleanup; }
    printf("WIFI_FILES before_radio heap8=%u largest_dma=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    Result=WifiNetwork_Start(false);
    if(Result != ESP_OK) goto Cleanup;
    Wifi=true;
    unsigned char Random[22]; esp_fill_random(Random,sizeof(Random));
    char Code[13]={0};
    uint32_t CodeValue; memcpy(&CodeValue,Random,sizeof(CodeValue));
    snprintf(Code,sizeof(Code),"%06lu",(unsigned long)(CodeValue % 1000000));
    for(unsigned I=0;I<16;++I)snprintf(Token+I*2,3,"%02x",Random[I+6]);
    portENTER_CRITICAL(&Lock); memcpy(Status.AccessCode,Code,sizeof(Code)); portEXIT_CRITICAL(&Lock);
    PairFailures=0; PairBlockedUntil=0;
    Result = CartBackup_SetPCMode(true);
    if (Result != ESP_OK) goto Cleanup;
    printf("WIFI_FILES before_http heap8=%u largest_dma=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    Result = StartServer();
    if (Result != ESP_OK) goto Cleanup;
    portENTER_CRITICAL(&Lock);
    Status.Ready = true; LastActivity = esp_timer_get_time();
    portEXIT_CRITICAL(&Lock);
    printf("WIFI_FILES ready heap8=%u largest_dma=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    while (!Stopping())
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        Result = WifiNetwork_Poll();
        if(Result != ESP_OK) { Failure="NETWORK MODE FAILED"; break; }
        Result = CartBackup_SetPCMode(true);
        if (Result != ESP_OK) { Failure = "TRANSFER MODE LOST"; break; }
        portENTER_CRITICAL(&Lock);
        bool Idle = esp_timer_get_time() - LastActivity > 10LL * 60 * 1000000;
        portEXIT_CRITICAL(&Lock);
        if (Idle) { Failure = "IDLE: WI-FI STOPPED"; break; }
    }
Cleanup:
    portENTER_CRITICAL(&Lock); StopRequested = true; Status.Ready = false; portEXIT_CRITICAL(&Lock);
    if (Result == ESP_ERR_NO_MEM) Failure = "NOT ENOUGH MEMORY";
    Message("STOPPING WI-FI");
    if (Server) { httpd_stop(Server); Server = NULL; }
    if (Wifi) WifiNetwork_Stop();
    memset(Token,0,sizeof(Token));
    if (Card)
    {
        esp_err_t Unmount = SDCard_Unmount(Card);
        if (Unmount != ESP_OK) { Result = Unmount; Failure = "SD UNMOUNT FAILED"; }
    }
    if (Lease)
    {
        esp_err_t Release = PCBackup_EndWireless();
        if (Release != ESP_OK) { Result = Release; Failure = "MODE EXIT FAILED"; }
    }
    printf("WIFI_FILES stopped error=%s\n", esp_err_to_name(Result));
    portENTER_CRITICAL(&Lock);
    memset(&Status, 0, sizeof(Status));
    snprintf(Status.Message, sizeof(Status.Message), "%s", Result == ESP_OK && strcmp(Failure, "IDLE: WI-FI STOPPED") ? "WI-FI OFF" : Failure);
    portEXIT_CRITICAL(&Lock);
    vTaskDelete(NULL);
}
static void Start(void)
{
    portENTER_CRITICAL(&Lock);
    if(Status.Busy)
    {
        portEXIT_CRITICAL(&Lock);
        return;
    }
    Status.Busy=true; StopRequested=false;
    snprintf(Status.Message,sizeof(Status.Message),"SAVING / STARTING...");
    portEXIT_CRITICAL(&Lock);
    if (xTaskCreate(TransferTask, "wifi_files", 6144, NULL, 3, NULL) != pdPASS)
    {
        portENTER_CRITICAL(&Lock); Status.Busy = false; portEXIT_CRITICAL(&Lock);
        Message("NOT ENOUGH MEMORY");
    }
}
static void Toggle(bool Enable)
{
    if(Enable) Start();
    else { portENTER_CRITICAL(&Lock); StopRequested=true; portEXIT_CRITICAL(&Lock); }
}
static bool ScanFromScreen(void) { return WifiNetwork_RequestScan() == ESP_OK; }
static bool JoinFromScreen(const char *SSID, const char *Password)
{
    WifiCredentials C={0};
    snprintf(C.SSID,sizeof(C.SSID),"%s",SSID);
    snprintf(C.Password,sizeof(C.Password),"%s",Password);
    esp_err_t R=WifiNetwork_Configure(&C);
    memset(&C,0,sizeof(C));
    return R==ESP_OK;
}
void WifiFiles_Init(void)
{
    WifiFilesUI_Register(Toggle,Snapshot);
    WifiFilesUI_RegisterNetwork(ScanFromScreen,WifiNetwork_GetScan,JoinFromScreen);
}
