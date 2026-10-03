#include <Windows.h>
#include <bcrypt.h>
#include <SPF_Plugin.h>
#include <SPF_Manifest_API.h>
#include <SPF_Logger_API.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
namespace {
constexpr char kName[]="SPF_SeatDiagnostics";
constexpr char kExecutableHash[]="5d3b4e343154de6b25dd366b6048c5bc5c452a4daa50bc38b33fd2e571d54257";
constexpr uintptr_t kApplyRva=0x114e000;
constexpr char kApplyPattern[]="48 89 5C 24 08 57 48 83 EC 70 48 8B F9 E8 4E E6 FF FF F3 0F 10 8F 78 03 00 00 F3 0F 5C 8F 9C 03 00 00 F3 0F 10 87 80 03 00 00";
using ApplyFn=void(__fastcall*)(void*);
ApplyFn originalApply=nullptr;
const SPF_Load_API* load=nullptr;
const SPF_Core_API* core=nullptr;
SPF_Logger_Handle* logger=nullptr;
SPF_Hook_Handle* applyHook=nullptr;
std::atomic<bool> observing{false};
bool SafeRead(uintptr_t address,void* output,size_t size) noexcept {
 if(!address || !output || size>256) return false;
 __try { std::memcpy(output,reinterpret_cast<void*>(address),size); return true; }
 __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(uintptr_t address,T& value) { return SafeRead(address,&value,sizeof(value)); }
struct Snapshot {
 bool valid=false;
 uint64_t category=0;
 std::array<float,3> xyz{};
 std::array<float,8> vehicle{};
};
Snapshot Capture(uintptr_t context) {
 Snapshot s; uintptr_t global=0,service=0,actor=0,settings=0;
 const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
 s.valid=Read(context+0x370,s.category) && SafeRead(context+0x378,s.xyz.data(),sizeof(s.xyz)) &&
  Read(base+0x36b8d70,global) && global && Read(global+0x31b0,service) && service &&
  Read(service+0x18,actor) && actor && Read(actor+0x1f8,settings) && settings &&
  SafeRead(settings+0x1f0,s.vehicle.data(),sizeof(s.vehicle));
 if(s.valid) { for(float v:s.xyz) s.valid=s.valid && std::isfinite(v); for(float v:s.vehicle) s.valid=s.valid && std::isfinite(v); }
 return s;
}
struct Event { Snapshot before,after; };
std::mutex eventMutex;
std::array<Event,64> events{};
size_t eventCount=0;
void DetourApply(void* context) {
 const bool capture=observing.load();
 Snapshot before; if(capture) before=Capture(reinterpret_cast<uintptr_t>(context));
 if(originalApply) originalApply(context);
 if(!capture) return;
 const auto after=Capture(reinterpret_cast<uintptr_t>(context));
 std::lock_guard lock(eventMutex);
 if(eventCount<events.size()) events[eventCount++]={before,after};
}
void Log(SPF_LogLevel level,const char* message) {
    if (load && load->logger && logger) load->logger->Log(logger,level,message);
}
bool HashExecutable(char (&hex)[65]) {
    wchar_t path[32768];
    if (!GetModuleFileNameW(nullptr,path,32768)) return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD size=0, returned=0;
    bool okay=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if (okay) okay=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&returned,0)>=0;
    std::vector<BYTE> object(size);
    if (okay) okay=BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)>=0;
    std::array<BYTE,65536> buffer{}; DWORD bytes=0;
    while (okay) {
        if (!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)) { okay=false; break; }
        if (!bytes) break;
        okay=BCryptHashData(hash,buffer.data(),bytes,0)>=0;
    }
    std::array<BYTE,32> digest{};
    if (okay) okay=BCryptFinishHash(hash,digest.data(),32,0)>=0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    CloseHandle(file);
    if (okay) for (size_t i=0;i<32;++i) std::snprintf(hex+i*2,3,"%02x",digest[i]);
    return okay;
}

void BuildManifest(SPF_Manifest_Builder_Handle* h,const SPF_Manifest_Builder_API* api) {
 api->Info_SetName(h,kName); api->Info_SetVersion(h,"0.1.0"); api->Info_SetMinFrameworkVersion(h,"1.2.5");
 api->Info_SetAuthor(h,"SPF Adjustment Hotkeys");
 api->Info_SetDescriptionLiteral(h,"Read-only observer of native seat menu apply and vehicle adjustment settings.");
 api->Policy_SetAllowUserConfig(h,true); api->Policy_AddConfigurableSystem(h,"logging"); api->Defaults_SetLogging(h,"info",true);
}
void OnLoad(const SPF_Load_API* api) { load=api; if(api && api->logger) logger=api->logger->Log_GetContext(kName); }
void OnActivated(const SPF_Core_API* api) {
 core=api; char hash[65]{};
 if(!core || !core->hooks || !HashExecutable(hash) || std::strcmp(hash,kExecutableHash)) { Log(SPF_LOG_WARN,"SEAT DIAG: inspected executable mismatch or hooks unavailable; no hook registered."); return; }
 if(core->hooks->Hook_FindPattern(kApplyPattern)!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+kApplyRva) { Log(SPF_LOG_ERROR,"SEAT DIAG: apply signature mismatch; no hook registered."); return; }
 observing.store(true);
 applyHook=core->hooks->Hook_Register(kName,"NativeSeatApply","Observe native seat apply",reinterpret_cast<void*>(DetourApply),reinterpret_cast<void**>(&originalApply),kApplyPattern,true);
 Log(SPF_LOG_INFO,"SEAT DIAG: hook requested. Adjust seat position axes separately in F4, then tilt/rotation/FOV separately.");
}
void OnUpdate() {
 if(!observing.load() || !core || !core->hooks) return;
 static bool reported=false;
 if(!reported && applyHook && core->hooks->Hook_IsInstalled(applyHook) && core->hooks->Hook_IsEnabled(applyHook)) { reported=true; Log(SPF_LOG_INFO,"SEAT DIAG: native apply observer installed and enabled."); }
 std::array<Event,64> pending{}; size_t count;
 { std::lock_guard lock(eventMutex); count=eventCount; std::copy_n(events.begin(),count,pending.begin()); eventCount=0; }
 char text[768];
 for(size_t i=0;i<count;++i) {
  const auto& e=pending[i]; const auto& v=e.after.vehicle;
  std::snprintf(text,sizeof(text),"SEAT DIAG: category=%llu before_valid=%d after_valid=%d menu_xyz=(%.6f,%.6f,%.6f) vehicle_xyz=(%.6f,%.6f,%.6f) extra=(%.6f,%.6f,%.6f,%.6f,%.6f) delta_xyz=(%.6f,%.6f,%.6f)",
   static_cast<unsigned long long>(e.after.category),e.before.valid,e.after.valid,e.after.xyz[0],e.after.xyz[1],e.after.xyz[2],v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[0]-e.before.vehicle[0],v[1]-e.before.vehicle[1],v[2]-e.before.vehicle[2]);
  Log(SPF_LOG_INFO,text);
 }
}
void OnUnload() { observing.store(false); }
}
extern "C" {
SPF_PLUGIN_EXPORT bool SPF_GetManifestAPI(SPF_Manifest_API* api) { if(!api) return false; api->BuildManifest=BuildManifest; return true; }
SPF_PLUGIN_EXPORT bool SPF_GetPlugin(SPF_Plugin_Exports* api) { if(!api) return false; api->OnLoad=OnLoad; api->OnActivated=OnActivated; api->OnUpdate=OnUpdate; api->OnUnload=OnUnload; return true; }
}
