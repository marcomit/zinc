#include "zvm.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define DEFAULT_STACK_SIZE MiB(64)

typedef struct ZVMStack ZVMStack;
typedef ZVMStack *ZVMStackRef;

typedef struct ZVMThread ZVMThread;
typedef ZVMThread *ZVMThreadRef;

struct ZVMStack {
    /* Stack pointer. */
    usize sp;

    /* Stack buffer size. */
    usize size;

    /* Pre-allocated buffer at the start and never grows. */
    void *buffer;
};

struct ZVirtualMachine {
    ZMIRModuleRef   *modules;
    ZVMStackRef     stack;
};

struct ZVMThread {
    ZVMRef      vm;
    ZVMStackRef stack;
};

static ZVMStackRef makestack(Allocator *a) {
    void *buffer = NULL;
    usize size = DEFAULT_STACK_SIZE;
    #if defined(_WIN32)
    buffer = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE | PAGE_READWRITE);
    #else
    buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    #endif
    if (!buffer) return NULL;
    return copy(a, ZVMStack, ((ZVMStack){
        .buffer = buffer,
        .size   = DEFAULT_STACK_SIZE,
        .sp     = 0
    }));
}

void ZVMRun(ZVMRef vm, ZMIRModuleRef module) {

}

void ZVMReset(ZVMRef vm) {
    vm->stack->sp = 0;
}

ZVMRef ZVMInit(Allocator *a, ZState *state, ZMIRModuleRef *modules) {
    return copy(a, ZVirtualMachine, ((ZVirtualMachine){
        .modules    = modules,
        .stack      = makestack(a)
    }));
    return NULL;
}

void ZVMDispose(ZVMRef vm) {
}
