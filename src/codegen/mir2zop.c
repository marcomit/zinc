#include "_zvm.h"
#include "zmir.h"

typedef struct {
    Allocator *allocator;
    ZVMStatus status;
    u32 pc;
} ZLower;

// u32 pc;
// u16 op, dst, a, b;
// u32 imm;

ZVMInstr *instr(ZLower *ctx, ZVMOp op, u16 dst, u16 a, u16 b, u16 imm) {
    ZVMInstr *self = copy(ctx->allocator, ZVMInstr, ((ZVMInstr){
        .op = (u16)op,
        .pc = ctx->pc++,
        .dst = dst,
        .a = a,
        .b = b,
        .imm = imm
    }));
    return self;
}

void lowerValue(ZLower *ctx, ZMIRValueRef value) {

}

void lowerBlock(ZLower *ctx, ZMIRBasicBlockRef block) {

}


ZVMFunction lowerFunc(ZLower *ctx, ZMIRValueRef func) {
    ZMIRBasicBlockRef *blks = func->fn.blocks;


    u16 offset = 0;
    for (usize i = 0; i < veclen(func->fn.params); i++) {
        ZMIRTypeRef type = ZMIRTypeOf(func->fn.params[i]);
        u16 align = ZMIRGetAlignment(func->fn.params[i]);
        u16 padding = (align - offset % align) % align;
        offset += padding;
        instr(ctx, ZOP_FRAMEADDR, 0, 0, 0, offset);
        usize len = ZMIRSizeOf(type);
        offset += len;
    }
    return (ZVMFunction){};
}

void mir2zop(ZMIRModuleRef module) {
    ZLower ctx;
    memset(&ctx, 0, sizeof(ctx));

    for (usize i = 0; i < veclen(module->decls); i++) {
        lowerFunc(&ctx, module->decls[i]);
    }
}
