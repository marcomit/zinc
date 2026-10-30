#ifndef ZVM_H
#define ZVM_H

#include "zinc.h"
#include "zmir.h"

typedef struct ZVirtualMachine ZVirtualMachine;

ZVirtualMachine *ZVMInit(Allocator *, ZState *);
void ZRun(ZVirtualMachine, ZMIRModuleRef);
void ZVMReset(ZVirtualMachine *);
void ZVMDispose(ZVirtualMachine *);

#endif //!ZVM_H
