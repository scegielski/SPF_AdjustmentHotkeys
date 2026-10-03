#include <Windows.h>
#include <bcrypt.h>
#include <mmsystem.h>
#include <filesystem>
#include <string>
#include <fstream>
#include "vendor/json.hpp"
#include "ServoLoop.hpp"
#include "SelectionClick.hpp"
#include <SPF_Plugin.h>
#include <SPF_Manifest_API.h>
#include <SPF_Logger_API.h>
#include <SPF_Formatting_API.h>
#include "Signatures.hpp"
#include <SPF_Config_API.h>
#include <SPF_KeyBinds_API.h>
#include <SPF_GameWorld_API.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
uintptr_t gOverrideContext=0;
unsigned int gOverrideSlot=0;
unsigned char gInputMask=0;
void* gLeftOriginal=nullptr;
void LeftInputStub();
void* gRightOriginal=nullptr;
void RightInputStub();
void* gUpOriginal=nullptr;
void UpInputStub();
void* gDownOriginal=nullptr;
void DownInputStub();
void* gResetOriginal=nullptr;
void ResetInputStub();

uintptr_t gSyntheticSeatContext=0;
void* gSeatUiOriginal=nullptr;
void* gSeatApplyReturn=nullptr;
void SeatUiStub();
void* gSelectOriginal=nullptr;
void* gSlotOriginal=nullptr;
void CenterSelectStub();
void CenterSlotStub();
}
namespace {
constexpr char kName[] = "SPF_MirrorControls";
constexpr char kExecutableHash[] = "5d3b4e343154de6b25dd366b6048c5bc5c452a4daa50bc38b33fd2e571d54257";
using UpdateFn = void(__fastcall*)(void*, float);
using SetRotationFn = void(__fastcall*)(void*, const float*);
UpdateFn originalUpdate = nullptr;
SetRotationFn originalSetRotation = nullptr;
const SPF_Core_API* core = nullptr;
const SPF_Load_API* load = nullptr;
SPF_Logger_Handle* logger = nullptr;
SPF_Hook_Handle* updateHook = nullptr;
SPF_Hook_Handle* rotationHook = nullptr;
SPF_Hook_Handle* selectHook=nullptr;
SPF_Hook_Handle* slotHook=nullptr;
SPF_KeyBinds_Handle* keys=nullptr;
SPF_Config_Handle* config=nullptr;
std::atomic<bool> holdSelectors{false};
bool voiceCuesEnabled=false;
bool servoEnabled=true;
ServoLoop servo;
SelectionClick selectionClick;
bool selectionClicksEnabled=true;
void PlaySelectionClick(int slot) { selectionClick.Play(slot==6?4:slot); }
void (*playSelectionClick)(int)=PlaySelectionClick;
std::filesystem::path PluginFolder();
int announcedSlot=-1;
bool cuePlaying=false;
std::array<std::wstring,4> cuePaths{};
using PlayCueFn=BOOL(WINAPI*)(LPCWSTR,HMODULE,DWORD);
PlayCueFn playCueSound=PlaySoundW;
unsigned char previousHeld=0;
int lastCycleSlot=-1;
int heldCycleSlot=-1;
std::atomic<uintptr_t> currentContext{0};
std::atomic<int> selectedSlot{-1};
std::atomic<unsigned char> inputMask{0};
std::array<SPF_Hook_Handle*,5> inputHooks{};
std::atomic<bool> movementAllowed{false};
std::mutex updateMutex;
void Log(SPF_LogLevel,const char*);
bool SeatReady();
void ClearSeatCalibration();
void MoveNativeSeat(float dt,unsigned char mask);
void RegisterSeatHooks();
void ToggleSeat();
bool HooksReady() {
 if (!core || !core->hooks || !updateHook || !rotationHook || !selectHook || !slotHook) return false;
 for(auto h:inputHooks) if(!h || !core->hooks->Hook_IsInstalled(h) || !core->hooks->Hook_IsEnabled(h)) return false;
 for (auto h:{updateHook,rotationHook,selectHook,slotHook})
  if (!core->hooks->Hook_IsInstalled(h) || !core->hooks->Hook_IsEnabled(h)) return false;
 return true;
}
int CueIndex(int slot) { return slot==0?0:slot==4?1:slot==2?2:slot==6?3:-1; }
void InitializeCuePaths() {
 HMODULE module=nullptr;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&gOverrideContext),&module)) return;
 wchar_t path[32768]{};
 if(!GetModuleFileNameW(module,path,32768)) return;
 const auto folder=std::filesystem::path(path).parent_path()/L"sounds";
 const wchar_t* names[]={L"left.wav",L"center.wav",L"right.wav",L"seat.wav"};
 for(size_t i=0;i<4;++i) cuePaths[i]=(folder/names[i]).wstring();
}
void UpdateVoiceCue(int slot) {
 if(slot==announcedSlot) return;
 announcedSlot=slot;
 const int index=CueIndex(slot);
 if(selectionClicksEnabled && index>=0) playSelectionClick(slot);
 if(!voiceCuesEnabled || index<0 || cuePaths[index].empty()) return;
 // Asynchronous playback interrupts the old cue instead of building a queue.
 if(playCueSound(cuePaths[index].c_str(),nullptr,SND_FILENAME|SND_ASYNC|SND_NODEFAULT)) cuePlaying=true;
 else Log(SPF_LOG_WARN,"MIRRORS: voice cue could not play; check the sounds folder.");
}
void StopVoiceCue() { if(cuePlaying) { playCueSound(nullptr,nullptr,0); cuePlaying=false; } }
const char* movementActions[]={"Move.left","Move.right","Move.up","Move.down","Move.reset"};
void SetMovementBlocking(bool block) {
 if(core && core->keybinds && keys) for(const auto* action:movementActions) core->keybinds->Kbind_SetBlockState(keys,action,block);
}
void ExitCenter() {
 if(selectedSlot.exchange(-1)>=0) Log(SPF_LOG_INFO,"MIRRORS: mode off.");
 inputMask.store(0); servo.SetPlaying(false); SetMovementBlocking(false);
}
int NextSelection(int current,int requested) { return current==requested?-1:requested; }
void ToggleSlot(int slot) {
 if(holdSelectors.load()) return;
 if(selectedSlot.load()==slot) { ExitCenter(); return; }
 if(slot==6 && !SeatReady()) { Log(SPF_LOG_WARN,"SEAT: native vehicle defaults unavailable; seat selection deferred."); return; }
 if(!movementAllowed.load() || !HooksReady()) { Log(SPF_LOG_WARN,"MIRRORS: unavailable until driving context and hooks are ready."); return; }
 selectedSlot.store(slot); inputMask.store(0); SetMovementBlocking(true);
 Log(SPF_LOG_INFO,slot==0?"MIRRORS: left on.":slot==2?"MIRRORS: right on.":slot==6?"SEAT: on.":"MIRRORS: center on.");
}
int ResolveHeldSelection(int current,unsigned char held,unsigned char previous) {
 const int slots[]={0,2,4,6};
 int target=current;
 // Newly pressed selector wins; simultaneous new presses use center/right/left priority.
 const auto pressed=static_cast<unsigned char>(held & ~previous);
 for(int i=0;i<4;++i) if(pressed & (1u<<i)) target=slots[i];
 for(int i=0;i<4;++i) if(target==slots[i] && (held & (1u<<i))) return target;
 for(int i=3;i>=0;--i) if(held & (1u<<i)) return slots[i];
 return -1;
}
int NextCycleSlot(int current,bool hold) {
 if(current==0) return 4;
 if(current==4) return 2;
 if(current==2) return 6;
 if(current==6) return hold?0:-1;
 return 0;
}
void CycleMirror() {
 if(!movementAllowed.load() || !HooksReady()) return;
 const bool hold=holdSelectors.load();
 int next=NextCycleSlot(hold?lastCycleSlot:selectedSlot.load(),hold);
 if(next==6 && !SeatReady()) { Log(SPF_LOG_WARN,"SEAT: native vehicle defaults unavailable; skipping seat."); next=hold?0:-1; }
 if(next<0) { ExitCenter(); return; }
 if(hold) { lastCycleSlot=next; heldCycleSlot=next; }
 selectedSlot.store(next); inputMask.store(0); SetMovementBlocking(true);
 Log(SPF_LOG_INFO,next==0?"MIRRORS: cycled to left.":next==4?"MIRRORS: cycled to center.":next==6?"SEAT: cycled to seat.":"MIRRORS: cycled to right.");
}
int ResolveHeldWithCycle(int current,unsigned char held,unsigned char previous,bool cycleHeld,int cycleSlot) {
 if((held & ~previous)!=0) return ResolveHeldSelection(current,held,previous);
 if(cycleHeld && cycleSlot>=0) return cycleSlot;
 return ResolveHeldSelection(current,held,previous);
}
void PollHoldSelection() {
 unsigned char held=0;
 const char* actions[]={"Select.left","Select.right","Select.center","Select.seat"};
 for(int i=0;i<4;++i) if(core->keybinds->Kbind_GetActionValue(keys,actions[i])>0.5f) held|=static_cast<unsigned char>(1u<<i);
 const int current=selectedSlot.load();
 const bool cycleHeld=core->keybinds->Kbind_GetActionValue(keys,"Select.cycle")>0.5f;
 const int next=ResolveHeldWithCycle(current,held,previousHeld,cycleHeld,heldCycleSlot);
 previousHeld=held;
 if(next==current) return;
 if(next==6 && !SeatReady()) { Log(SPF_LOG_WARN,"SEAT: native vehicle defaults unavailable; seat selection deferred."); return; }
 if(next<0) { ExitCenter(); return; }
 selectedSlot.store(next); inputMask.store(0); SetMovementBlocking(true);
 Log(SPF_LOG_INFO,next==0?"MIRRORS: left held.":next==2?"MIRRORS: right held.":next==6?"SEAT: held.":"MIRRORS: center held.");
}
void ToggleLeft() { ToggleSlot(0); }
void ToggleRight() { ToggleSlot(2); }
void ToggleCenter() { ToggleSlot(4); }
void ToggleSeat() { ToggleSlot(6); }
void NoAction() {}
void MapContext(uintptr_t) noexcept;
void WorldReset() { ClearSeatCalibration(); ExitCenter(); movementAllowed.store(false); currentContext.store(0); previousHeld=0; heldCycleSlot=-1; lastCycleSlot=-1; MapContext(0); }

std::atomic<bool> observing{false};
std::atomic<uint64_t> contextCalls{0};

// SEH is isolated from C++ objects with destructors. Only reads diagnostic data.
bool SafeRead(uintptr_t address, void* output, size_t bytes) noexcept {
    if (!address || !output || bytes > 4096) return false;
    __try { std::memcpy(output, reinterpret_cast<const void*>(address), bytes); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(uintptr_t address, T& value) noexcept {
    return SafeRead(address, &value, sizeof(value));
}
struct Event {
    int slot = -1;
    uint64_t calls = 0;
    bool hasAngles = false;
    std::array<float, 3> angles{};
    std::array<float, 4> rotation{};
};
struct State {
    std::mutex mutex;
    uintptr_t entries = 0;
    uint64_t count = 0;
    uint64_t generation = 0;
    std::array<Event, 9> pending{};
    std::array<bool, 9> changed{};
    std::array<float, 36> snapshot{};
    bool hasSnapshot = false;
    uint64_t matchedCalls = 0;
} state;

int Slot(uintptr_t object, uintptr_t entries, uint64_t count) noexcept {
    if (!entries || count == 0 || count > 9 || object < entries) return -1;
    const uintptr_t relative = object - entries;
    if (relative % 16 || relative / 16 >= count) return -1;
    return static_cast<int>(relative / 16);
}
bool Finite(const float* values, size_t count) noexcept {
    for (size_t i=0; i<count; ++i) if (!std::isfinite(values[i])) return false;
    return true;
}
void MapContext(uintptr_t context) noexcept {
    uintptr_t actor=0, mirrors=0, entries=0;
    uint64_t count=0;
    bool valid = Read(context+0x118, actor) && actor &&
        Read(actor+0x1f8, mirrors) && mirrors &&
        Read(mirrors+0x1d0, entries) && entries &&
        Read(mirrors+0x1d8, count) && count > 0 && count <= 9;
    std::array<float,36> sample{};
    valid = valid && SafeRead(entries, sample.data(), static_cast<size_t>(count)*16) &&
        Finite(sample.data(), static_cast<size_t>(count)*4);
    if (!valid) { entries=0; count=0; }
    std::lock_guard lock(state.mutex);
    if (state.entries != entries || state.count != count) {
        state.entries=entries;
        state.count=count;
        ++state.generation;
        state.hasSnapshot=false;
        state.changed.fill(false);
        state.pending.fill(Event{});
    }
}
void CaptureSnapshot() noexcept {
    uintptr_t entries; uint64_t count, generation;
    { std::lock_guard lock(state.mutex); entries=state.entries; count=state.count; generation=state.generation; }
    if (!entries || !count) return;
    std::array<float,36> sample{};
    if (!SafeRead(entries,sample.data(),static_cast<size_t>(count)*16) ||
        !Finite(sample.data(),static_cast<size_t>(count)*4)) return;
    std::lock_guard lock(state.mutex);
    if (state.generation!=generation) return;
    if (state.hasSnapshot) {
        for (size_t i=0;i<count;++i) {
            if (std::memcmp(sample.data()+i*4,state.snapshot.data()+i*4,16)) {
                auto& e=state.pending[i];
                e.slot=static_cast<int>(i);
                if (std::memcmp(e.rotation.data(),sample.data()+i*4,16)) e.hasAngles=false;
                std::memcpy(e.rotation.data(),sample.data()+i*4,16);
                state.changed[i]=true;
            }
        }
    }
    state.snapshot=sample;
    state.hasSnapshot=true;
}
bool CanOverride(uintptr_t context,int slot) {
 if (slot<0 || slot==6 || !movementAllowed.load()) return false;
 std::lock_guard lock(state.mutex);
 return context && state.entries && static_cast<uint64_t>(slot)<state.count && state.count<=9;
}
void DetourUpdate(void* context,float dt) {
 // Serializes scoped register override across calls; no host input or object writes.
 std::lock_guard callLock(updateMutex);
 if(observing.load()) {
  currentContext.store(reinterpret_cast<uintptr_t>(context)); ++contextCalls;
  MapContext(reinterpret_cast<uintptr_t>(context));
 }
 const int slot=selectedSlot.load();
 bool enabled=CanOverride(reinterpret_cast<uintptr_t>(context),slot);
 if (enabled && std::isfinite(dt) && dt>=0 && dt<=0.1f)
  { gOverrideSlot=static_cast<unsigned int>(slot); gInputMask=inputMask.load(); gOverrideContext=reinterpret_cast<uintptr_t>(context); }
 else gOverrideContext=0;
 originalUpdate(context,dt); // Exactly one call; native getter, setter and camera refresh.
 gOverrideContext=0; gInputMask=0;
 if(slot==6 && movementAllowed.load()) MoveNativeSeat(dt,inputMask.load());
}
void DetourSetRotation(void* object, const float* angles) {
    int slot=-1; uint64_t generation=0;
    std::array<float,3> requested{};
    bool gotAngles=false;
    if (observing.load(std::memory_order_relaxed)) {
        { std::lock_guard lock(state.mutex); slot=Slot(reinterpret_cast<uintptr_t>(object),state.entries,state.count); generation=state.generation; }
        if (slot>=0) gotAngles=SafeRead(reinterpret_cast<uintptr_t>(angles),requested.data(),sizeof(requested)) && Finite(requested.data(),3);
    }
    originalSetRotation(object,angles); // Exactly the original pointers, exactly once.
    if (slot<0) return;
    std::array<float,4> rotation{};
    if (!SafeRead(reinterpret_cast<uintptr_t>(object),rotation.data(),sizeof(rotation)) || !Finite(rotation.data(),4)) return;
    std::lock_guard lock(state.mutex);
    if (generation!=state.generation || slot!=Slot(reinterpret_cast<uintptr_t>(object),state.entries,state.count)) return;
    auto& e=state.pending[slot];
    e.slot=slot;
    ++e.calls;
    e.hasAngles=gotAngles;
    e.angles=requested;
    e.rotation=rotation;
    state.changed[slot]=true;
    ++state.matchedCalls;
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
std::filesystem::path PluginFolder() {
 HMODULE module=nullptr;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
   reinterpret_cast<LPCWSTR>(&gOverrideContext),&module)) return {};
 wchar_t path[32768]{};
 if(!GetModuleFileNameW(module,path,32768)) return {};
 return std::filesystem::path(path).parent_path();
}
nlohmann::json ReadSavedSettings(const std::filesystem::path& file) {
 std::ifstream stream(file);
 if(!stream) return nlohmann::json::object();
 auto parsed=nlohmann::json::parse(stream,nullptr,false);
 return parsed.is_object()?parsed:nlohmann::json::object();
}
bool DefaultBindingNeeded(const nlohmann::json& saved,const char* group,const char* action) {
 const std::string fullGroup=std::string(kName)+"."+group;
 if(!saved.contains("keybinds") || !saved["keybinds"].is_object()) return true;
 const auto& binds=saved["keybinds"];
 if(!binds.contains(fullGroup) || !binds[fullGroup].is_object()) return true;
 const auto& actions=binds[fullGroup];
 if(!actions.contains(action) || !actions[action].is_object()) return true;
 const auto& value=actions[action];
 // An explicitly empty list is a saved choice, rather than a missing default.
 return !value.contains("bindings") || !value["bindings"].is_array();
}
#include "NativeSeat.inc"
void BuildManifest(SPF_Manifest_Builder_Handle* h,const SPF_Manifest_Builder_API* api) {
    const auto saved=ReadSavedSettings(PluginFolder()/"config"/"settings.json");
    api->Info_SetName(h,kName);
    api->Info_SetVersion(h,"0.2.1");
    api->Info_SetMinFrameworkVersion(h,"1.2.5");
    api->Info_SetAuthor(h,"SPF Adjustment Hotkeys");
    api->Info_SetDescriptionLiteral(h,"Live mirror and native VR seat adjustment through SPF hotkeys.");
    api->Policy_SetAllowUserConfig(h,true);
    api->Policy_AddConfigurableSystem(h,"logging");
    api->Policy_AddConfigurableSystem(h,"settings");
    api->Settings_SetJson(h,R"json({"hold_selectors":false,"voice_cues":false,"servo_sound":true,"selection_clicks":true})json");
    api->Meta_AddCustomSetting(h,"settings.selection_clicks","Adjustment selection click","Play a click when selecting mirrors or seat.","checkbox",nullptr,false);
    api->Meta_AddCustomSetting(h,"settings.servo_sound","Adjustment servo sound","Play a quiet motor sound while a mirror or seat is moving.","checkbox",nullptr,false);
    api->Meta_AddCustomSetting(h,"settings.voice_cues","Adjustment voice cues","Announce the selected mirror or seat when adjustment starts or switches.","checkbox",nullptr,false);
    api->Meta_AddCustomSetting(h,"settings.hold_selectors","Hold adjustment selector","Off: tap to toggle. On: hold selector while adjusting; release to exit.","checkbox",nullptr,false);
    api->Defaults_SetLogging(h,"info",true);
    if(DefaultBindingNeeded(saved,"Move","left")) api->Defaults_AddKeybind(h,"Move","left","keyboard","KEY_A","manual");
    if(DefaultBindingNeeded(saved,"Move","right")) api->Defaults_AddKeybind(h,"Move","right","keyboard","KEY_D","manual");
    if(DefaultBindingNeeded(saved,"Move","up")) api->Defaults_AddKeybind(h,"Move","up","keyboard","KEY_W","manual");
    if(DefaultBindingNeeded(saved,"Move","down")) api->Defaults_AddKeybind(h,"Move","down","keyboard","KEY_S","manual");
    api->Meta_AddKeybind(h,"Select","seat","Select seat","Toggle or hold; W/S up/down, A/D forward/back using current movement assignments.");
    api->Meta_AddKeybind(h,"Select","cycle","Cycle mirrors and seat","Toggle: left, center, right, seat, off. Hold: each press advances; release exits.");
    api->Meta_AddKeybind(h,"Select","left","Toggle left mirror","Tap on/off, or switch from another mirror.");
    api->Meta_AddKeybind(h,"Select","right","Toggle right mirror","Tap on/off, or switch from another mirror.");
    api->Meta_AddKeybind(h,"Select","center","Toggle center mirror","Tap on/off, or switch from another mirror.");
}
void OnLoad(const SPF_Load_API* api) { load=api; InitializeCuePaths(); if(api && api->config) { config=api->config->Cfg_GetContext(kName); holdSelectors.store(api->config->Cfg_GetBool(config,"settings.hold_selectors",false)); } if (api && api->logger) logger=api->logger->Log_GetContext(kName); }
void OnActivated(const SPF_Core_API* api) {
    core=api;
    if(core && core->keybinds) {
      keys=core->keybinds->Kbind_GetContext(kName);
      // Migrate obsolete exit actions out of existing configurations and UI ownership.
      for(const char* action:{"Select.menu","Select.escape"}) core->keybinds->Kbind_UnregisterActionMetadata(keys,action);
      if(core->config && config) {
        core->config->Cfg_RemoveKey(config,"keybinds.SPF_MirrorControls.Select.menu");
        core->config->Cfg_RemoveKey(config,"keybinds.SPF_MirrorControls.Select.escape");
      }
      // Declare bindable actions without assigning keys on a fresh installation.
      core->keybinds->Kbind_RegisterActionMetadata(keys,"Select.left","Select left mirror","Toggle or hold to adjust the left mirror.",nullptr,nullptr);
      core->keybinds->Kbind_RegisterActionMetadata(keys,"Select.right","Select right mirror","Toggle or hold to adjust the right mirror.",nullptr,nullptr);
      core->keybinds->Kbind_RegisterActionMetadata(keys,"Select.center","Select center mirror","Toggle or hold to adjust the center mirror.",nullptr,nullptr);
      core->keybinds->Kbind_RegisterActionMetadata(keys,"Select.seat","Select seat","Adjust native seat up/down and forward/back.",nullptr,nullptr);
      core->keybinds->Kbind_Register(keys,"Select.seat",ToggleSeat);
      core->keybinds->Kbind_RegisterActionMetadata(keys,"Select.cycle","Cycle mirrors and seat","Cycle left, center, right, seat, then off; hold mode wraps to left.",nullptr,nullptr);
      core->keybinds->Kbind_RegisterActionMetadata(keys,"Move.reset","Reset selected mirror","Reset the mirror currently being adjusted.",nullptr,nullptr);
      core->keybinds->Kbind_Register(keys,"Select.left",ToggleLeft);
      core->keybinds->Kbind_Register(keys,"Select.right",ToggleRight);
      core->keybinds->Kbind_Register(keys,"Select.center",ToggleCenter);
      core->keybinds->Kbind_Register(keys,"Select.cycle",CycleMirror);
      for(const auto* action:movementActions) core->keybinds->Kbind_Register(keys,action,NoAction);
    }
    if (!core || !core->hooks || !core->gameworld || !core->keybinds) { Log(SPF_LOG_ERROR,"DIAG: hooks API unavailable; observation disabled."); return; }
    char hash[65]{};
    if (!HashExecutable(hash) || std::strcmp(hash,kExecutableHash)) {
        Log(SPF_LOG_WARN,"DIAG: executable does not match the inspected ATS build; no hooks registered."); return;
    }
    auto hooks=core->hooks;
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (hooks->Hook_FindPattern(kUpdatePattern)!=base+kUpdateRva || hooks->Hook_FindPattern(kSetRotationPattern)!=base+kSetRotationRva || hooks->Hook_FindPattern(kSelectPattern)!=base+kSelectRva || hooks->Hook_FindPattern(kSlotPattern)!=base+kSlotRva || hooks->Hook_FindPattern(kLeftPattern)!=base+kLeftRva || hooks->Hook_FindPattern(kRightPattern)!=base+kRightRva || hooks->Hook_FindPattern(kUpPattern)!=base+kUpRva || hooks->Hook_FindPattern(kDownPattern)!=base+kDownRva || hooks->Hook_FindPattern(kResetPattern)!=base+kResetRva) {
        Log(SPF_LOG_ERROR,"DIAG: expected function signatures not found at verified locations; no hooks registered."); return;
    }
    if(!servo.Open(PluginFolder()/"sounds"/"servo.wav")) Log(SPF_LOG_WARN,"MIRRORS: servo sound unavailable; check sounds/servo.wav and Windows audio output.");
    if(!selectionClick.Open(PluginFolder()/"sounds"/"click.wav")) Log(SPF_LOG_WARN,"MIRRORS: selection click unavailable; check sounds/click.wav and Windows audio output.");
    RegisterSeatHooks();
    observing.store(true);
    updateHook=hooks->Hook_Register(kName,"MirrorContext","Observe mirror context",reinterpret_cast<void*>(DetourUpdate),reinterpret_cast<void**>(&originalUpdate),kUpdatePattern,true);
    rotationHook=hooks->Hook_Register(kName,"MirrorRotation","Observe mirror rotation",reinterpret_cast<void*>(DetourSetRotation),reinterpret_cast<void**>(&originalSetRotation),kSetRotationPattern,true);
    selectHook=hooks->Hook_Register(kName,"CenterSelect","Center selector override",reinterpret_cast<void*>(CenterSelectStub),&gSelectOriginal,kSelectPattern,true);
    slotHook=hooks->Hook_Register(kName,"CenterSlot","Center physical slot override",reinterpret_cast<void*>(CenterSlotStub),&gSlotOriginal,kSlotPattern,true);
    inputHooks[0]=hooks->Hook_Register(kName,"InputLeft","Mirror Left input",reinterpret_cast<void*>(LeftInputStub),&gLeftOriginal,kLeftPattern,true);
    inputHooks[1]=hooks->Hook_Register(kName,"InputRight","Mirror Right input",reinterpret_cast<void*>(RightInputStub),&gRightOriginal,kRightPattern,true);
    inputHooks[2]=hooks->Hook_Register(kName,"InputUp","Mirror Up input",reinterpret_cast<void*>(UpInputStub),&gUpOriginal,kUpPattern,true);
    inputHooks[3]=hooks->Hook_Register(kName,"InputDown","Mirror Down input",reinterpret_cast<void*>(DownInputStub),&gDownOriginal,kDownPattern,true);
    inputHooks[4]=hooks->Hook_Register(kName,"InputReset","Mirror Reset input",reinterpret_cast<void*>(ResetInputStub),&gResetOriginal,kResetPattern,true);
    Log(SPF_LOG_INFO,"MIRRORS: nine hooks requested. Bind selectors/cycle/reset in SPF; only WASD is assigned on first installation.");
}
void OnUpdate() {
    if (!observing.load() || !core || !core->hooks) return;
    DWORD foregroundPid=0;
    GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
    const bool allowed=foregroundPid==GetCurrentProcessId() && core->gameworld && !core->gameworld->GW_IsGamePaused();
    movementAllowed.store(allowed && HooksReady());
    if (!allowed) ExitCenter();
    const bool requestedHold=core->config && config && core->config->Cfg_GetBool(config,"settings.hold_selectors",false);
    if(requestedHold!=holdSelectors.load()) {
      ExitCenter(); previousHeld=0; heldCycleSlot=-1; lastCycleSlot=-1; holdSelectors.store(requestedHold);
      Log(SPF_LOG_INFO,requestedHold?"MIRRORS: hold selection enabled.":"MIRRORS: tap-to-toggle enabled.");
    }
    if(holdSelectors.load() && movementAllowed.load() && allowed) PollHoldSelection();
    const bool requestedVoice=core->config && config && core->config->Cfg_GetBool(config,"settings.voice_cues",false);
    if(!requestedVoice && voiceCuesEnabled) StopVoiceCue();
    voiceCuesEnabled=requestedVoice;
    selectionClicksEnabled=!core->config || !config || core->config->Cfg_GetBool(config,"settings.selection_clicks",true);
    if(selectedSlot.load()==6 && !SeatReady()) ExitCenter();
    UpdateVoiceCue(selectedSlot.load());
    unsigned char mask=0;
    if(selectedSlot.load()>=0 && movementAllowed.load()) {
      for(size_t i=0;i<5;++i) if(core->keybinds->Kbind_GetActionValue(keys,movementActions[i])>0.5f) mask|=static_cast<unsigned char>(1u<<i);
    }
    inputMask.store(mask);
    servoEnabled=!core->config || !config || core->config->Cfg_GetBool(config,"settings.servo_sound",true);
    servo.SetMirror(selectedSlot.load()==6?4:selectedSlot.load());
    servo.SetPlaying(ServoMovement(selectedSlot.load(),mask,servoEnabled,movementAllowed.load()));
    static bool reported=false;
    if (!reported && HooksReady()) {
        Log(SPF_LOG_INFO,"DIAG: all nine hooks installed and enabled. Mirror controls ready when driving."); reported=true;
    }
    const auto context=currentContext.load(std::memory_order_relaxed);
    if (context) { MapContext(context); CaptureSnapshot(); }
    static uint64_t last=0;
    const uint64_t now=GetTickCount64();
    if (now-last<250) return;
    last=now;
    std::array<Event,9> pending{}; std::array<bool,9> changed{};
    static uint64_t lastGeneration=~uint64_t{0};
    uint64_t count,generation;
    { std::lock_guard lock(state.mutex); pending=state.pending; changed=state.changed; state.changed.fill(false); count=state.count; generation=state.generation; }
    char message[512];
    const auto fmt=core->formatting;
    if (!fmt) return;
    if (generation!=lastGeneration) {
        fmt->Fmt_Format(message,sizeof(message),"DIAG: mirror array discovered: %llu entries; capture generation %llu.",static_cast<unsigned long long>(count),static_cast<unsigned long long>(generation));
        Log(SPF_LOG_INFO,message); lastGeneration=generation;
    }
    for (size_t i=0;i<9;++i) if (changed[i]) {
        const auto& e=pending[i];
        fmt->Fmt_Format(message,sizeof(message),"DIAG: slot=%d rotation=(%.6f,%.6f,%.6f,%.6f) setter_calls=%llu angles_valid=%d angles=(%.6f,%.6f,%.6f)",
            static_cast<int>(i),e.rotation[0],e.rotation[1],e.rotation[2],e.rotation[3],static_cast<unsigned long long>(e.calls),e.hasAngles?1:0,e.angles[0],e.angles[1],e.angles[2]);
        Log(SPF_LOG_INFO,message);
    }
}
void OnUnload() {
    StopVoiceCue(); servo.Close(); selectionClick.Close();
    // SPF removes hook objects after OnUnload. Preserve trampolines until then.
    ClearSeatCalibration(); ExitCenter(); movementAllowed.store(false);
    observing.store(false);
    currentContext.store(0);
}
}
extern "C" {
SPF_PLUGIN_EXPORT bool SPF_GetManifestAPI(SPF_Manifest_API* api) {
    if (!api) return false;
    api->BuildManifest=BuildManifest; return true;
}
SPF_PLUGIN_EXPORT bool SPF_GetPlugin(SPF_Plugin_Exports* api) {
    if (!api) return false;
    api->OnLoad=OnLoad; api->OnActivated=OnActivated; api->OnUnload=OnUnload; api->OnUpdate=OnUpdate; api->OnWorldUnloaded=WorldReset; api->OnGameWorldReady=WorldReset;
    return true;
}
}




