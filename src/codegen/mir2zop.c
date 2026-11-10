#include "_zvm.h"
#include "zmir.h"

typedef ZMap(ZMIRBasicBlockRef, u32) BlockMap;
typedef ZMap(ZMIRValueRef, u32) SlotMap;

typedef struct {
    Allocator   *allocator;
    ZVMStatus   status;
    BlockMap    blocks;
    SlotMap     slots;
    u32         pc;
    u16         nslots;
} ZLower;

// u32 pc;
// u16 op, dst, a, b;
// u32 imm;

ZVMInstr *instr(ZLower *ctx, ZVMOp op, u16 dst, u16 a, u16 b, u32 imm) {
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

/* The VM only loads and stores scalars (integers, floats, pointers), so an
 * aggregate SSA value lives in consecutive slots, one per scalar leaf in
 * layout order, and a load or store of it becomes one LOADn/STOREn per leaf
 * at that leaf's byte offset from the base address. */
typedef struct {
    bool store;
    u16  ptr;   // slot holding the base address
    u16  slot;  // slot of the next leaf
} MemOp;

static inline usize alignUp(usize n, usize align) {
    return align ? (n + align - 1) / align * align : n;
}

static ZVMOp scalarOp(bool store, usize size) {
    switch (size) {
    case 1:  return store ? ZOP_STORE8  : ZOP_LOAD8;
    case 2:  return store ? ZOP_STORE16 : ZOP_LOAD16;
    case 4:  return store ? ZOP_STORE32 : ZOP_LOAD32;
    case 8:  return store ? ZOP_STORE64 : ZOP_LOAD64;
    default: return ZOP_TRAP;
    }
}

/* Returns the number of scalar leaves of `t`; with `m` it also emits the
 * per-leaf memory op, so slot counts and emitted code always agree. */
static u16 walkLeaves(ZLower *ctx, ZMIRTypeRef t, u32 offset, MemOp *m) {
    switch (t->kind) {
    case ZMIRVoidTypeKind:
        return 0;
    case ZMIRStructTypeKind: {
        u16 n = 0;
        usize off = 0;
        for (usize i = 0; i < veclen(t->strct.fields); i++) {
            ZMIRTypeRef f = t->strct.fields[i];
            off = alignUp(off, t->strct.packed ? 1 : ZMIRABIAlignmentOfType(NULL, f));
            n += walkLeaves(ctx, f, offset + off, m);
            off += ZMIRABISizeOfType(NULL, f);
        }
        return n;
    }
    case ZMIRArrayTypeKind: {
        u16 n = 0;
        usize stride = ZMIRABISizeOfType(NULL, t->array.elem);
        for (usize i = 0; i < t->array.len; i++) {
            n += walkLeaves(ctx, t->array.elem, offset + i * stride, m);
        }
        return n;
    }
    default:
        if (m) {
            ZVMOp op = scalarOp(m->store, ZMIRABISizeOfType(NULL, t));
            if (m->store) instr(ctx, op, 0, m->ptr, m->slot, offset);
            else          instr(ctx, op, m->slot, m->ptr, 0, offset);
            m->slot++;
        }
        return 1;
    }
}

static u16 slotOf(ZLower *ctx, ZMIRValueRef v) {
    u32 *slot = mapget(ctx->slots, v);
    if (slot) return (u16)*slot;
    u16 base = ctx->nslots;
    ctx->nslots += walkLeaves(ctx, v->type, 0, NULL);
    mapput(ctx->slots, v, base);
    return base;
}

static void lowerLoad(ZLower *ctx, ZMIRValueRef value) {
    MemOp m = {
        .store  = false,
        .ptr    = slotOf(ctx, value->instr.ops[0]),
        .slot   = slotOf(ctx, value),
    };
    walkLeaves(ctx, value->type, 0, &m);
}

static void lowerStore(ZLower *ctx, ZMIRValueRef value) {
    ZMIRValueRef val = value->instr.ops[0];
    MemOp m = {
        .store  = true,
        .ptr    = slotOf(ctx, value->instr.ops[1]),
        .slot   = slotOf(ctx, val),
    };
    walkLeaves(ctx, val->type, 0, &m);
}

void lowerInstr(ZLower *ctx, ZMIRValueRef value) {
    switch (value->instr.op) {
    case ZMIRRet: break;
    case ZMIRBr: break;
    case ZMIRSwitch: break;
    case ZMIRIndirectBr: break;
    case ZMIRInvoke: break;
    case ZMIRUnreachable: break;
    case ZMIRCallBr: break;
    case ZMIRFNeg: break;
    case ZMIRAdd: break;
    case ZMIRFAdd: break;
    case ZMIRSub: break;
    case ZMIRFSub: break;
    case ZMIRMul: break;
    case ZMIRFMul: break;
    case ZMIRUDiv: break;
    case ZMIRSDiv: break;
    case ZMIRFDiv: break;
    case ZMIRURem: break;
    case ZMIRSRem: break;
    case ZMIRFRem: break;
    case ZMIRShl: break;
    case ZMIRLShr: break;
    case ZMIRAShr: break;
    case ZMIRAnd: break;
    case ZMIROr: break;
    case ZMIRXor: break;
    case ZMIRAlloca: break;
    case ZMIRLoad: lowerLoad(ctx, value); break;
    case ZMIRStore: lowerStore(ctx, value); break;
    case ZMIRGetElementPtr: break;
    case ZMIRTrunc: break;
    case ZMIRZExt: break;
    case ZMIRSExt: break;
    case ZMIRFPToUI: break;
    case ZMIRFPToSI: break;
    case ZMIRUIToFP: break;
    case ZMIRSIToFP: break;
    case ZMIRFPTrunc: break;
    case ZMIRFPExt: break;
    case ZMIRPtrToInt: break;
    case ZMIRPtrToAddr: break;
    case ZMIRIntToPtr: break;
    case ZMIRBitCast: break;
    case ZMIRAddrSpaceCast: break;
    case ZMIRICmp: break;
    case ZMIRFCmp: break;
    case ZMIRPHI: break;
    case ZMIRCall: break;
    case ZMIRSelect: break;
    case ZMIRUserOp1: break;
    case ZMIRUserOp2: break;
    case ZMIRVAArg: break;
    case ZMIRExtractElement: break;
    case ZMIRInsertElement: break;
    case ZMIRShuffleVector: break;
    case ZMIRExtractValue: break;
    case ZMIRInsertValue: break;
    case ZMIRFreeze: break;
    case ZMIRMemSet: break;
    case ZMIRFence: break;
    case ZMIRAtomicCmpXchg: break;
    case ZMIRAtomicRMW: break;
    case ZMIRResume: break;
    case ZMIRLandingPad: break;
    case ZMIRCleanupRet: break;
    case ZMIRCatchRet: break;
    case ZMIRCatchPad: break;
    case ZMIRCleanupPad: break;
    case ZMIRCatchSwitch: break;
    }
}

void lowerValue(ZLower *ctx, ZMIRValueRef value) {
    switch (value->kind) {
    case ZMIRInstructionValueKind:
        lowerInstr(ctx, value);
        break;
    default:
        break;
    }
}

ZVMFunction lowerFunc(ZLower *ctx, ZMIRValueRef func) {
    ZMIRBasicBlockRef *blks = func->fn.blocks;

    ZVMFunction f   = { 0 };
    f.nparams       = veclen(func->fn.params);
    ctx->nslots     = 0;

    for (usize i = 0; i < veclen(func->fn.params); i++) {
        slotOf(ctx, func->fn.params[i]);
    }

    for (usize i = 0; i < veclen(func->fn.blocks); i++) {
        ZMIRBasicBlockRef blk = func->fn.blocks[i];
        mapput(ctx->blocks, blk, ctx->pc + 1);
        for (usize j = 0; j < veclen(blk->instrs); j++) {
            lowerValue(ctx, blk->instrs[j]);
        }
    }

    f.nslots = ctx->nslots;
    return f;
}

void mir2zop(ZMIRModuleRef module) {
    ZLower ctx = { 0 };
    for (usize i = 0; i < veclen(module->decls); i++) {
        lowerFunc(&ctx, module->decls[i]);
    }
}
