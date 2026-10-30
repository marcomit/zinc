#ifndef ZVM_H
#define ZVM_H

#include "zinc.h"
#include "zmir.h"

typedef struct ZVirtualMachine ZVirtualMachine;

typedef ZVirtualMachine *ZVMRef;

ZVMRef ZVMInit(Allocator *, ZState *, ZMIRModuleRef *);
void ZVMRun(ZVMRef , ZMIRModuleRef);
void ZVMReset(ZVMRef );
void ZVMDispose(ZVMRef );

#endif //!ZVM_H
