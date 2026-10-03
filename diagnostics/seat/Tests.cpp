#include "SeatDiagnostics.cpp"
#include <cstdlib>
#include <iostream>
int calls=0; void* forwarded=nullptr;
void __fastcall Stub(void* context) { ++calls; forwarded=context; }
int main() {
 originalApply=Stub;
 void* context=reinterpret_cast<void*>(1);
 observing.store(false); DetourApply(context);
 if(calls!=1 || forwarded!=context || eventCount!=0) return 1;
 observing.store(true); DetourApply(context);
 if(calls!=2 || forwarded!=context || eventCount!=1 || events[0].before.valid || events[0].after.valid) return 2;
 for(int i=0;i<100;++i) DetourApply(context);
 if(calls!=102 || eventCount!=64) return 3;
 float output=0;
 if(SafeRead(0,&output,4) || SafeRead(1,&output,4) || SafeRead(1,&output,257)) return 4;
 std::cout<<"PASS: observer forwards context exactly once, invalid memory guarded, bounded records, disabled observer pass-through.\n";
}
