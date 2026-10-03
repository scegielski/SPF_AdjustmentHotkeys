#include <algorithm>
#include "MirrorControls.cpp"
#include <iostream>
#include <cstdlib>
#define CHECK(x) do { if(!(x)) { std::cerr<<"FAIL: "<<#x<<" line "<<__LINE__<<'\n'; std::exit(1); } } while(false)
extern "C" void RunSlotStubs(uintptr_t,void*);
extern "C" void TestSelectReturn();
extern "C" void RunInputStubs(uintptr_t,void*);
extern "C" void TestSlotReturn();
namespace {
int cueCalls=0; std::wstring lastCue; DWORD cueFlags=0;
int clickCalls=0; int lastClickSlot=-1;
void StubClick(int slot) { ++clickCalls; lastClickSlot=slot; }
BOOL WINAPI StubCue(LPCWSTR path,HMODULE,DWORD flags) { ++cueCalls; lastCue=path?path:L""; cueFlags=flags; return TRUE; }
int calls=0; void* passed=nullptr; float passedDt=0; uintptr_t scoped=0;
void __fastcall StubUpdate(void* context,float dt) { ++calls; passed=context; passedDt=dt; scoped=gOverrideContext; }
template<class T,size_t N> void Put(std::array<unsigned char,N>& bytes,size_t off,T value) { std::memcpy(bytes.data()+off,&value,sizeof(value)); }
}
int main() {
 playCueSound=StubCue; cuePaths={L"left.wav",L"center.wav",L"right.wav"};
 playSelectionClick=StubClick;
 CHECK(CueIndex(0)==0 && CueIndex(4)==1 && CueIndex(2)==2 && CueIndex(-1)==-1);
 UpdateVoiceCue(0); CHECK(cueCalls==1 && lastCue==L"left.wav" && (cueFlags&SND_ASYNC));
 UpdateVoiceCue(0); CHECK(cueCalls==1);
 UpdateVoiceCue(4); CHECK(cueCalls==2 && lastCue==L"center.wav");
 UpdateVoiceCue(2); CHECK(cueCalls==3 && lastCue==L"right.wav");
 UpdateVoiceCue(-1); CHECK(cueCalls==3);
 voiceCuesEnabled=false; UpdateVoiceCue(0); CHECK(cueCalls==3);
 StopVoiceCue(); CHECK(cueCalls==4 && lastCue.empty());
 voiceCuesEnabled=true; UpdateVoiceCue(-1); UpdateVoiceCue(0); CHECK(cueCalls==5);
 CHECK(clickCalls==5 && lastClickSlot==0); // Includes selection with speech muted.
 UpdateVoiceCue(0); CHECK(clickCalls==5); // Held selection cannot repeat.
 UpdateVoiceCue(4); CHECK(clickCalls==6 && lastClickSlot==4);
 UpdateVoiceCue(2); CHECK(clickCalls==7 && lastClickSlot==2);
 UpdateVoiceCue(-1); CHECK(clickCalls==7); // Cycling to off is silent.
 selectionClicksEnabled=false; UpdateVoiceCue(0); CHECK(clickCalls==7 && cueCalls==8);
 selectionClicksEnabled=true;
 const auto saved=nlohmann::json::parse(R"json({"keybinds":{"SPF_MirrorControls.Select":{"left":{"bindings":[]},"cycle":{"bindings":[{"key":"KEY_1","type":"keyboard"}]}}}})json");
 CHECK(!DefaultBindingNeeded(saved,"Select","left"));
 CHECK(!DefaultBindingNeeded(saved,"Select","cycle"));
 CHECK(DefaultBindingNeeded(saved,"Select","right"));
 CHECK(DefaultBindingNeeded(nlohmann::json::object(),"Select","left"));
 const auto malformed=nlohmann::json::parse(R"json({"keybinds":{"SPF_MirrorControls.Select":{"left":{"bindings":null}}}})json");
 CHECK(DefaultBindingNeeded(malformed,"Select","left"));
 const auto fixture=std::filesystem::temp_directory_path()/"spf-mirror-persistence-regression.json";
 { std::ofstream file(fixture); file<<saved.dump(); }
 for(int run=0;run<2;++run) { const auto reloaded=ReadSavedSettings(fixture); CHECK(!DefaultBindingNeeded(reloaded,"Select","left")); CHECK(!DefaultBindingNeeded(reloaded,"Select","cycle")); }
 std::filesystem::remove(fixture);
 CHECK(ServoChannelVolume(0)==0x00004000); CHECK(ServoChannelVolume(4)==0x40004000); CHECK(ServoChannelVolume(2)==0x40000000); CHECK(ServoChannelVolume(-1)==0);
 CHECK(ServoMovement(0,1,true,true)); CHECK(ServoMovement(4,4,true,true));
 CHECK(!ServoMovement(0,0,true,true)); CHECK(!ServoMovement(-1,1,true,true));
 CHECK(!ServoMovement(0,3,true,true)); CHECK(!ServoMovement(0,12,true,true));
 CHECK(!ServoMovement(0,16,true,true)); CHECK(!ServoMovement(0,1,false,true)); CHECK(!ServoMovement(0,1,true,false));
 ServoWaveHeader wav{}; std::memcpy(wav.riff,"RIFF",4); std::memcpy(wav.wave,"WAVE",4); std::memcpy(wav.fmt,"fmt ",4); std::memcpy(wav.data,"data",4);
 wav.fmtSize=16; wav.format=1; wav.channels=1; wav.bits=16; wav.rate=44100; wav.align=2; wav.byteRate=88200; wav.bytes=44100;
 CHECK(ValidServoHeader(wav)); wav.channels=2; wav.align=4; wav.byteRate=176400; CHECK(ValidServoHeader(wav)); wav.bytes=1048578; CHECK(!ValidServoHeader(wav)); wav.bytes=3; CHECK(!ValidServoHeader(wav));
 int invalidRead=0; CHECK(!SafeRead(1,&invalidRead,sizeof(invalidRead))); CHECK(Slot(0x1040,0x1000,9)==4); CHECK(Slot(0x1041,0x1000,9)==-1);
 gSelectOriginal=reinterpret_cast<void*>(TestSelectReturn); gSlotOriginal=reinterpret_cast<void*>(TestSlotReturn);
 alignas(8) unsigned char out[16]{};
 gOverrideContext=0; RunSlotStubs(0x1234,out); CHECK(out[0]==0 && *reinterpret_cast<int*>(out+4)==2 && out[8]==1 && out[9]==1);
 gOverrideSlot=4; gOverrideContext=0x1234; RunSlotStubs(0x1234,out); CHECK(out[0]==1 && *reinterpret_cast<int*>(out+4)==4 && out[8]==1 && out[9]==1);
 RunSlotStubs(0x5678,out); CHECK(out[0]==0 && *reinterpret_cast<int*>(out+4)==2 && out[8]==1 && out[9]==1);
 gOverrideContext=0; RunSlotStubs(0,out); CHECK(out[0]==0 && *reinterpret_cast<int*>(out+4)==2);
 for(int slot:{0,2,4}) {
  gOverrideContext=0x1234; gOverrideSlot=slot; RunSlotStubs(0x1234,out);
  CHECK(out[0]==1 && *reinterpret_cast<int*>(out+4)==slot && out[8]==1 && out[9]==1);
 }
 gLeftOriginal=gRightOriginal=gUpOriginal=gDownOriginal=gResetOriginal=reinterpret_cast<void*>(TestSelectReturn);
 for(unsigned int mask=0;mask<32;++mask) {
  gOverrideContext=0x1234; gInputMask=static_cast<unsigned char>(mask); RunInputStubs(0x1234,out);
  for(int i=0;i<5;++i) CHECK(out[i]==((mask>>i)&1) && out[8+i]==1);
 }
 gOverrideContext=0; gInputMask=0; RunInputStubs(0x1234,out); for(int i=0;i<5;++i) CHECK(out[i]==1);
 gOverrideContext=0x1234; RunInputStubs(0x5678,out); for(int i=0;i<5;++i) CHECK(out[i]==1);
 for(int current:{-1,0,2,4}) for(int requested:{0,2,4}) CHECK(NextSelection(current,requested)==(current==requested?-1:requested));
 gOverrideContext=0;
 CHECK(ResolveHeldSelection(-1,1,0)==0);
 CHECK(ResolveHeldSelection(0,1,1)==0);
 CHECK(ResolveHeldSelection(0,0,1)==-1);
 CHECK(ResolveHeldSelection(0,3,1)==2);
 CHECK(ResolveHeldSelection(2,1,3)==0);
 CHECK(ResolveHeldSelection(2,7,3)==4);
 CHECK(ResolveHeldSelection(4,3,7)==2);
 CHECK(ResolveHeldSelection(-1,4,0)==4);
 CHECK(ResolveHeldSelection(-1,7,0)==4);
 CHECK(ResolveHeldSelection(4,0,4)==-1);
 CHECK(NextCycleSlot(-1,false)==0); CHECK(NextCycleSlot(0,false)==4); CHECK(NextCycleSlot(4,false)==2); CHECK(NextCycleSlot(2,false)==-1);
 CHECK(NextCycleSlot(-1,true)==0); CHECK(NextCycleSlot(0,true)==4); CHECK(NextCycleSlot(4,true)==2); CHECK(NextCycleSlot(2,true)==0);
 CHECK(ResolveHeldWithCycle(4,0,0,true,4)==4);
 CHECK(ResolveHeldWithCycle(4,0,0,false,4)==-1);
 CHECK(ResolveHeldWithCycle(4,1,0,true,4)==0);
 CHECK(ResolveHeldWithCycle(4,1,1,false,4)==0);
 holdSelectors=true; selectedSlot=-1; ToggleLeft(); ToggleRight(); ToggleCenter(); CHECK(selectedSlot==-1);
 holdSelectors=false;
 std::array<float,36> entries{}; for(int i=0;i<9;++i) entries[i*4+3]=1;
 const auto beforeEntries=entries;
 std::array<unsigned char,0x130> context{}; std::array<unsigned char,0x210> actor{},mirrors{};
 Put(context,0x118,reinterpret_cast<uintptr_t>(actor.data())); Put(actor,0x1f8,reinterpret_cast<uintptr_t>(mirrors.data()));
 Put(mirrors,0x1d0,reinterpret_cast<uintptr_t>(entries.data())); Put(mirrors,0x1d8,uint64_t{9});
 const auto beforeContext=context; const auto beforeActor=actor; const auto beforeMirrors=mirrors;
 observing=true; originalUpdate=StubUpdate;
 selectedSlot=-1; movementAllowed=true; DetourUpdate(context.data(),0.016f);
 CHECK(calls==1 && passed==context.data() && passedDt==0.016f && scoped==0 && gOverrideContext==0);
 selectedSlot=4; DetourUpdate(context.data(),0.016f);
 CHECK(calls==2 && scoped==reinterpret_cast<uintptr_t>(context.data()) && gOverrideContext==0);
 movementAllowed=false; DetourUpdate(context.data(),0.016f); CHECK(calls==3 && scoped==0);
 movementAllowed=true; DetourUpdate(context.data(),0.5f); CHECK(calls==4 && scoped==0);
 Put(mirrors,0x1d8,uint64_t{3}); DetourUpdate(context.data(),0.016f); CHECK(calls==5 && scoped==0);
 Put(mirrors,0x1d8,uint64_t{9});
 CHECK(context==beforeContext && actor==beforeActor && mirrors==beforeMirrors && entries==beforeEntries);
 ExitCenter(); CHECK(selectedSlot.load()==-1); CHECK(!HooksReady()); ToggleCenter(); CHECK(selectedSlot.load()==-1);
 std::cout<<"PASS: real assembly stubs select all mirror slots only for matching scoped context; flags preserved; side/default pass-through; one native call with unchanged arguments; override cleared; input override masks, toggle/switch and hold/release/overlap and cycle transitions, voice cue mapping and no-repeat/mute/asynchronous playback, persisted assigned/cleared bindings across reloads, servo movement/mute/cancel guards and WAV validation and left/center/right channel mapping, disallowed/large-dt/short-array guards; host metadata and rotations untouched by wrapper.\n";
}

