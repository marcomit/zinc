#ifndef _ZVM_H
#define _ZVM_H

#include "zvm.h"

typedef ZMap(const char *, u32) BlockMap;
typedef ZMap(u16, u16) SlotMap;

typedef enum {
    #define ZOP(name, fmt) ZOP_##name,
    #include "zop.def"
    #undef ZOP
    ZOP__COUNT
} ZVMOp;

typedef struct {
    u32 pc;
    u16 op, dst, a, b;
    u32 imm;
} ZVMInstr;

typedef struct {
    const char  *name;
    ZVMInstr    *code;
    u16         nparams;
    u16         nslots;
    u32         frameSize;
    u32         frameAlign;
} ZVMFunction;

typedef struct {
    ZVMFunction *funcs;
    u64         *consts;
    u8          *data;
    usize       dataSize;
    u32         entry;
} ZVMProgram;

#endif //!_ZVM_H
