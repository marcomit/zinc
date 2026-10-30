/**
 * @file zllvm.c
 *
 * @brief LLVM backend: lowers the ZMIR modules produced by zgen to LLVM IR,
 * links them into a single module and emits IR, assembly, objects or an
 * executable.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */
#include "mir2llvm.h"
#include "zcolors.h"
#include "zgen.h"
#include "zinc.h"
#include "zlink.h"

#include "zmap.h"

#include <stdarg.h>

/* ========== ZMIR -> LLVM ========== */

typedef ZMap(ZMIRTypeRef,       LLVMTypeRef)        TypeMap;
typedef ZMap(ZMIRValueRef,      LLVMValueRef)       ValueMap;
typedef ZMap(ZMIRBasicBlockRef, LLVMBasicBlockRef)  BlockMap;
typedef ZMap(ZMIRBasicBlockRef, bool)               BlockSet;

typedef struct {
    LLVMContextRef  ctx;
    LLVMModuleRef   mod;
    LLVMBuilderRef  builder;
    ZMIRModuleRef   mir;

    TypeMap         types;
    ValueMap        values;
    BlockMap        blocks;

    ZMIRValueRef    *phis;
    bool            failed;
} ZLower;

static void lowerError(ZLower *l, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, COLOR_RED "zmir2llvm: " COLOR_RESET);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    l->failed = true;
}

static inline const char *lname(const char *name) {
    return name ? name : "";
}

/* The maps only live for one lowering, so they use the heap rather than the
 * default allocator, which may be an arena that never frees. */
static inline LLVMTypeRef getType(ZLower *l, ZMIRTypeRef t) {
    LLVMTypeRef *out = mapget(l->types, t);
    return out ? *out : NULL;
}

static inline LLVMValueRef getValue(ZLower *l, ZMIRValueRef v) {
    LLVMValueRef *out = mapget(l->values, v);
    return out ? *out : NULL;
}

static inline LLVMBasicBlockRef getBlock(ZLower *l, ZMIRBasicBlockRef bb) {
    LLVMBasicBlockRef *out = mapget(l->blocks, bb);
    return out ? *out : NULL;
}

static inline void putType(ZLower *l, ZMIRTypeRef t, LLVMTypeRef out) {
    mapputwith(l->types, t, out, heapAllocator);
}

static inline void putValue(ZLower *l, ZMIRValueRef v, LLVMValueRef out) {
    mapputwith(l->values, v, out, heapAllocator);
}

static inline void putBlock(ZLower *l, ZMIRBasicBlockRef bb, LLVMBasicBlockRef out) {
    mapputwith(l->blocks, bb, out, heapAllocator);
}

/* ---------- Types ---------- */

static LLVMTypeRef lowerType(ZLower *l, ZMIRTypeRef t);

static LLVMTypeRef lowerStructType(ZLower *l, ZMIRTypeRef t) {
    usize n = veclen(t->strct.fields);

    if (!t->name) {
        LLVMTypeRef fields[n ? n : 1];
        for (usize i = 0; i < n; i++) fields[i] = lowerType(l, t->strct.fields[i]);
        return LLVMStructTypeInContext(l->ctx, fields, n, t->strct.packed);
    }

    /* Named structs live in the LLVM context, which every module shares:
     * reuse the one an earlier module created so the linker sees one type. */
    LLVMTypeRef named = LLVMGetTypeByName2(l->ctx, t->name);
    if (!named) named = LLVMStructCreateNamed(l->ctx, t->name);

    /* Cache before lowering the fields, so a struct can point to itself. */
    putType(l, t, named);

    if (t->strct.fields && LLVMIsOpaqueStruct(named)) {
        LLVMTypeRef fields[n ? n : 1];
        for (usize i = 0; i < n; i++) fields[i] = lowerType(l, t->strct.fields[i]);
        LLVMStructSetBody(named, fields, n, t->strct.packed);
    }
    return named;
}

static LLVMTypeRef lowerType(ZLower *l, ZMIRTypeRef t) {
    if (!t) return LLVMVoidTypeInContext(l->ctx);

    LLVMTypeRef cached = getType(l, t);
    if (cached) return cached;

    LLVMTypeRef out = NULL;
    switch (t->kind) {
    case ZMIRVoidTypeKind:      out = LLVMVoidTypeInContext(l->ctx);                break;
    case ZMIRHalfTypeKind:      out = LLVMHalfTypeInContext(l->ctx);                break;
    case ZMIRBFloatTypeKind:    out = LLVMBFloatTypeInContext(l->ctx);              break;
    case ZMIRFloatTypeKind:     out = LLVMFloatTypeInContext(l->ctx);               break;
    case ZMIRDoubleTypeKind:    out = LLVMDoubleTypeInContext(l->ctx);              break;
    case ZMIRX86_FP80TypeKind:  out = LLVMX86FP80TypeInContext(l->ctx);             break;
    case ZMIRFP128TypeKind:     out = LLVMFP128TypeInContext(l->ctx);               break;
    case ZMIRPPC_FP128TypeKind: out = LLVMPPCFP128TypeInContext(l->ctx);            break;
    case ZMIRPointerTypeKind:   out = LLVMPointerTypeInContext(l->ctx, 0);          break;
    case ZMIRIntegerTypeKind:
        out = t->width ? LLVMIntTypeInContext(l->ctx, t->width)
                       : LLVMVoidTypeInContext(l->ctx);
        break;
    case ZMIRArrayTypeKind:
        out = LLVMArrayType2(lowerType(l, t->array.elem), t->array.len);
        break;
    case ZMIRStructTypeKind:
        return lowerStructType(l, t);
    case ZMIRFunctionTypeKind: {
        usize n = veclen(t->func.args);
        LLVMTypeRef args[n ? n : 1];
        for (usize i = 0; i < n; i++) args[i] = lowerType(l, t->func.args[i]);
        out = LLVMFunctionType(lowerType(l, t->func.ret), args, n, t->func.vararg);
        break;
    }
    default:
        lowerError(l, "unsupported type kind %d", t->kind);
        out = LLVMVoidTypeInContext(l->ctx);
        break;
    }
    putType(l, t, out);
    return out;
}

/* ---------- Values ---------- */

static LLVMValueRef lowerConst(ZLower *l, ZMIRValueRef v) {
    LLVMTypeRef ty = lowerType(l, v->type);

    switch (v->kind) {
    case ZMIRConstantIntValueKind:
        return LLVMConstInt(ty, v->cint.value, v->cint.signext);
    case ZMIRConstantFPValueKind:
        return LLVMConstReal(ty, v->creal);
    case ZMIRConstantPointerNullValueKind:
        return LLVMGetTypeKind(ty) == LLVMPointerTypeKind
            ? LLVMConstPointerNull(ty) : LLVMConstNull(ty);
    case ZMIRConstantAggregateZeroValueKind:
        return LLVMConstNull(ty);
    case ZMIRUndefValueValueKind:
        return LLVMGetUndef(ty);
    case ZMIRPoisonValueValueKind:
        return LLVMGetPoison(ty);
    case ZMIRConstantDataArrayValueKind: {
        /* The type includes the terminator when there is one. */
        bool dontNullTerminate = v->type->array.len == v->str.len;
        return LLVMConstStringInContext2(l->ctx, v->str.data, v->str.len, dontNullTerminate);
    }
    case ZMIRConstantStructValueKind:
    case ZMIRConstantArrayValueKind: {
        usize n = veclen(v->aggregate);
        LLVMValueRef elems[n ? n : 1];
        for (usize i = 0; i < n; i++) {
            LLVMValueRef e = getValue(l, v->aggregate[i]);
            elems[i] = e ? e : lowerConst(l, v->aggregate[i]);
        }
        if (v->kind == ZMIRConstantArrayValueKind)
            return LLVMConstArray2(lowerType(l, v->type->array.elem), elems, n);
        if (v->type->name)
            return LLVMConstNamedStruct(ty, elems, n);
        return LLVMConstStructInContext(l->ctx, elems, n, v->type->strct.packed);
    }
    default:
        lowerError(l, "value of kind %d is not a constant", v->kind);
        return LLVMGetPoison(ty);
    }
}

/* An operand: something already lowered (global, function, argument,
 * instruction) or a constant built on the spot. */
static LLVMValueRef lowerValue(ZLower *l, ZMIRValueRef v) {
    if (!v) {
        lowerError(l, "missing operand");
        return LLVMGetPoison(LLVMInt8TypeInContext(l->ctx));
    }

    LLVMValueRef out = getValue(l, v);
    if (out) return out;

    switch (v->kind) {
    case ZMIRFunctionValueKind:
    case ZMIRGlobalVariableValueKind:
    case ZMIRArgumentValueKind:
    case ZMIRInstructionValueKind:
        lowerError(l, "use of '%s' before its definition", lname(v->name));
        return LLVMGetPoison(lowerType(l, v->type));
    default:
        return lowerConst(l, v);
    }
}

/* ---------- Instructions ---------- */

static void lowerInstr(ZLower *l, ZMIRValueRef v) {
    LLVMBuilderRef b    = l->builder;
    ZMIRValueRef *ops   = v->instr.ops;
    usize nops          = veclen(ops);
    const char *name    = lname(v->name);
    LLVMValueRef out    = NULL;

    #define OP(i)    lowerValue(l, ops[i])
    #define BLOCK(i) getBlock(l, v->instr.blocks[i])

    switch (v->instr.op) {
    case ZMIRRet:
        out = nops ? LLVMBuildRet(b, OP(0)) : LLVMBuildRetVoid(b);
        break;
    case ZMIRBr:
        out = nops ? LLVMBuildCondBr(b, OP(0), BLOCK(0), BLOCK(1))
                   : LLVMBuildBr(b, BLOCK(0));
        break;
    case ZMIRUnreachable:
        out = LLVMBuildUnreachable(b);
        break;

    case ZMIRFNeg:
        out = LLVMBuildFNeg(b, OP(0), name);
        break;

    case ZMIRAdd:  case ZMIRFAdd: case ZMIRSub:  case ZMIRFSub:
    case ZMIRMul:  case ZMIRFMul: case ZMIRUDiv: case ZMIRSDiv:
    case ZMIRFDiv: case ZMIRURem: case ZMIRSRem: case ZMIRFRem:
    case ZMIRShl:  case ZMIRLShr: case ZMIRAShr:
    case ZMIRAnd:  case ZMIROr:   case ZMIRXor:
        /* ZMIR opcodes share LLVM's numbering. */
        out = LLVMBuildBinOp(b, (LLVMOpcode)v->instr.op, OP(0), OP(1), name);
        break;

    case ZMIRAlloca:
        out = LLVMBuildAlloca(b, lowerType(l, v->instr.auxType), name);
        if (v->instr.align) LLVMSetAlignment(out, v->instr.align);
        break;
    case ZMIRLoad:
        out = LLVMBuildLoad2(b, lowerType(l, v->type), OP(0), name);
        if (v->instr.align) LLVMSetAlignment(out, v->instr.align);
        LLVMSetVolatile(out, v->instr.isVolatile);
        break;
    case ZMIRStore:
        out = LLVMBuildStore(b, OP(0), OP(1));
        if (v->instr.align) LLVMSetAlignment(out, v->instr.align);
        LLVMSetVolatile(out, v->instr.isVolatile);
        break;
    case ZMIRGetElementPtr: {
        LLVMValueRef idx[nops > 1 ? nops - 1 : 1];
        for (usize i = 1; i < nops; i++) idx[i - 1] = OP(i);
        out = LLVMBuildGEP2(b, lowerType(l, v->instr.auxType), OP(0), idx, nops - 1, name);
        break;
    }

    case ZMIRTrunc:    case ZMIRZExt:     case ZMIRSExt:
    case ZMIRFPToUI:   case ZMIRFPToSI:   case ZMIRUIToFP:
    case ZMIRSIToFP:   case ZMIRFPTrunc:  case ZMIRFPExt:
    case ZMIRPtrToInt: case ZMIRIntToPtr: case ZMIRBitCast:
    case ZMIRAddrSpaceCast:
        out = LLVMBuildCast(b, (LLVMOpcode)v->instr.op, OP(0), lowerType(l, v->type), name);
        break;

    case ZMIRICmp:
        out = LLVMBuildICmp(b, (LLVMIntPredicate)v->instr.ipred, OP(0), OP(1), name);
        break;
    case ZMIRFCmp:
        out = LLVMBuildFCmp(b, (LLVMRealPredicate)v->instr.fpred, OP(0), OP(1), name);
        break;

    case ZMIRPHI:
        out = LLVMBuildPhi(b, lowerType(l, v->type), name);
        vecpush(l->phis, v);
        break;

    case ZMIRCall: {
        LLVMTypeRef fnType = lowerType(l, v->instr.auxType);
        LLVMValueRef args[nops > 1 ? nops - 1 : 1];
        for (usize i = 1; i < nops; i++) args[i - 1] = OP(i);
        /* LLVM refuses to name a value of type void. */
        bool isVoid = LLVMGetTypeKind(LLVMGetReturnType(fnType)) == LLVMVoidTypeKind;
        out = LLVMBuildCall2(b, fnType, OP(0), args, nops - 1, isVoid ? "" : name);
        break;
    }
    case ZMIRSelect:
        out = LLVMBuildSelect(b, OP(0), OP(1), OP(2), name);
        break;
    case ZMIRExtractValue:
        out = LLVMBuildExtractValue(b, OP(0), v->instr.index, name);
        break;
    case ZMIRInsertValue:
        out = LLVMBuildInsertValue(b, OP(0), OP(1), v->instr.index, name);
        break;
    case ZMIRFreeze:
        out = LLVMBuildFreeze(b, OP(0), name);
        break;

    case ZMIRMemSet: {
        LLVMValueRef len = LLVMConstInt(
            LLVMInt64TypeInContext(l->ctx), v->instr.builtin_memset.len, false);
        out = LLVMBuildMemSet(b, OP(0), OP(1), len, v->instr.builtin_memset.align);
        break;
    }

    default:
        lowerError(l, "unsupported instruction opcode %d", v->instr.op);
        if (v->type) out = LLVMGetPoison(lowerType(l, v->type));
        break;
    }

    #undef OP
    #undef BLOCK

    if (out) putValue(l, v, out);
}

/* ---------- Functions ---------- */

static inline ZMIRValueRef blockTerminator(ZMIRBasicBlockRef bb) {
    return ZMIRGetBasicBlockTerminator(bb) ? veclast(bb->instrs) : NULL;
}

/* Reverse post-order from the entry block: every block comes after the
 * blocks that dominate it, so an instruction is always lowered before
 * its (non-phi) uses. Blocks the entry can't reach go last. */
static void postOrder(ZLower *l, ZMIRBasicBlockRef bb, BlockSet *seen, ZMIRBasicBlockRef **order) {
    if (maphas(*seen, bb)) return;
    mapputwith(*seen, bb, true, heapAllocator);

    ZMIRValueRef term = blockTerminator(bb);
    if (term) {
        for (usize i = 0; i < veclen(term->instr.blocks); i++)
            postOrder(l, term->instr.blocks[i], seen, order);
    }
    vecpush(*order, bb);
}

static void lowerBody(ZLower *l, ZMIRValueRef fn) {
    LLVMValueRef func = getValue(l, fn);
    ZMIRBasicBlockRef *blocks = fn->fn.blocks;
    usize nblocks = veclen(blocks);

    /* Create every block first, in ZMIR order, so branches can target
     * blocks that aren't lowered yet and the layout stays the same. */
    for (usize i = 0; i < nblocks; i++) {
        LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(l->ctx, func, lname(blocks[i]->name));
        putBlock(l, blocks[i], bb);
    }

    BlockSet seen = NULL;
    ZMIRBasicBlockRef *order = NULL;
    postOrder(l, blocks[0], &seen, &order);

    usize reachable = veclen(order);
    for (usize i = 0; i < nblocks; i++)
        if (!maphas(seen, blocks[i])) vecpush(order, blocks[i]);

    /* Reachable blocks in reverse post-order, then the unreachable ones. */
    for (usize k = 0; k < veclen(order); k++) {
        ZMIRBasicBlockRef bb = k < reachable ? order[reachable - 1 - k] : order[k];
        LLVMBasicBlockRef lbb = getBlock(l, bb);
        LLVMPositionBuilderAtEnd(l->builder, lbb);

        for (usize i = 0; i < veclen(bb->instrs); i++)
            lowerInstr(l, bb->instrs[i]);

        /* LLVM requires every block to end in a terminator. */
        if (!LLVMGetBasicBlockTerminator(lbb))
            LLVMBuildUnreachable(l->builder);
    }

    vecfree(order);
    mapfree(seen);
}

static void fillPhis(ZLower *l) {
    for (usize i = 0; i < veclen(l->phis); i++) {
        ZMIRValueRef phi = l->phis[i];
        usize n = veclen(phi->instr.ops);
        LLVMValueRef      vals[n ? n : 1];
        LLVMBasicBlockRef bbs[n ? n : 1];
        for (usize k = 0; k < n; k++) {
            vals[k] = lowerValue(l, phi->instr.ops[k]);
            bbs[k]  = getBlock(l, phi->instr.blocks[k]);
        }
        LLVMAddIncoming(getValue(l, phi), vals, bbs, n);
    }
}

/* ---------- Module ---------- */

static void declareFunction(ZLower *l, ZMIRValueRef fn) {
    LLVMValueRef func = LLVMGetNamedFunction(l->mod, lname(fn->name));
    if (!func) func = LLVMAddFunction(l->mod, lname(fn->name), lowerType(l, fn->fn.fnType));

    /* A declaration (no body) must be visible outside the module. */
    bool isDecl = vecempty(fn->fn.blocks);
    LLVMSetLinkage(func, isDecl ? LLVMExternalLinkage : (LLVMLinkage)fn->fn.linkage);

    for (usize i = 0; i < veclen(fn->fn.attrs); i++) {
        const char *attr = fn->fn.attrs[i];
        unsigned kind = LLVMGetEnumAttributeKindForName(attr, strlen(attr));
        if (kind) {
            LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex,
                LLVMCreateEnumAttribute(l->ctx, kind, 0));
        } else {
            LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex,
                LLVMCreateStringAttribute(l->ctx, attr, strlen(attr), "", 0));
        }
    }

    putValue(l, fn, func);
    for (usize i = 0; i < veclen(fn->fn.params); i++)
        putValue(l, fn->fn.params[i], LLVMGetParam(func, i));
}

static void declareGlobal(ZLower *l, ZMIRValueRef g) {
    LLVMValueRef global = LLVMAddGlobal(l->mod, lowerType(l, g->global.valueType), lname(g->name));
    LLVMSetLinkage(global, (LLVMLinkage)g->global.linkage);
    LLVMSetUnnamedAddress(global, (LLVMUnnamedAddr)g->global.unnamedAddr);
    LLVMSetGlobalConstant(global, g->global.isConst);
    if (g->global.align) LLVMSetAlignment(global, g->global.align);
    putValue(l, g, global);
}

LLVMModuleRef zmir2llvm(LLVMContextRef ctx, ZMIRModuleRef mir) {
    if (!mir || vecempty(mir->decls)) return NULL;

    ZLower l = {
        .ctx        = ctx,
        .mod        = LLVMModuleCreateWithNameInContext(lname(mir->name), ctx),
        .builder    = LLVMCreateBuilderInContext(ctx),
        .mir        = mir,
    };

    /* 1. Declare everything, so initializers and bodies can refer to any
     *    global or function regardless of the order they were added in. */
    for (usize i = 0; i < veclen(mir->decls); i++) {
        ZMIRValueRef d = mir->decls[i];
        switch (d->kind) {
        case ZMIRFunctionValueKind:         declareFunction(&l, d); break;
        case ZMIRGlobalVariableValueKind:   declareGlobal(&l, d);   break;
        default: lowerError(&l, "unexpected declaration of kind %d", d->kind);
        }
    }

    /* 2. Global initializers. */
    for (usize i = 0; i < veclen(mir->decls); i++) {
        ZMIRValueRef d = mir->decls[i];
        if (d->kind != ZMIRGlobalVariableValueKind || !d->global.init) continue;
        LLVMSetInitializer(getValue(&l, d), lowerValue(&l, d->global.init));
    }

    /* 3. Function bodies. */
    for (usize i = 0; i < veclen(mir->decls); i++) {
        ZMIRValueRef d = mir->decls[i];
        if (d->kind != ZMIRFunctionValueKind || vecempty(d->fn.blocks)) continue;
        lowerBody(&l, d);
    }
    fillPhis(&l);

    LLVMDisposeBuilder(l.builder);
    vecfree(l.phis);
    mapfree(l.types);
    mapfree(l.values);
    mapfree(l.blocks);

    if (l.failed) {
        LLVMDisposeModule(l.mod);
        return NULL;
    }
    return l.mod;
}

/* ========== Target ========== */

static LLVMCodeGenOptLevel toCodeGenLevel(char level) {
    switch (level) {
    case '0': return LLVMCodeGenLevelNone;
    case '1': return LLVMCodeGenLevelLess;
    case '3': return LLVMCodeGenLevelAggressive;
    case 's':
    case 'z': return LLVMCodeGenLevelAggressive;
    default:  return LLVMCodeGenLevelDefault;
    }
}

static void buildOptPipeline(char level, char *buf, usize bufsize) {
    char lvl = level ? level : '2';
    if      (lvl == 's') snprintf(buf, bufsize, "default<Os>");
    else if (lvl == 'z') snprintf(buf, bufsize, "default<Oz>");
    else                 snprintf(buf, bufsize, "default<O%c>", lvl);
}

static inline char optLevel(ZState *state) {
    return state->cli.optimizationLevel ? state->cli.optimizationLevel : '2';
}

/* The target triple, always copied into the state arena so callers never free it. */
static char *targetTriple(ZState *state) {
    if (state->cli.targetTriple) return zstrdup(state->allocator, state->cli.targetTriple);

    char *host   = LLVMGetDefaultTargetTriple();
    char *triple = zstrdup(state->allocator, host);
    LLVMDisposeMessage(host);
    return triple;
}

bool initTargetMachine(ZState *state) {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeAllAsmPrinters();

    bool hostTriple = (state->cli.targetTriple == NULL);
    char *triple    = targetTriple(state);

    LLVMTargetRef target;
    char *errmsg = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &errmsg)) {
        zlog(state, NULL, Z4014, errmsg);
        LLVMDisposeMessage(errmsg);
        return false;
    }

    const char *cpu         = state->cli.targetCPU      ? state->cli.targetCPU      : "generic";
    const char *features    = state->cli.targetFeatures ? state->cli.targetFeatures : "";
    LLVMRelocMode reloc     = hostTriple                ? LLVMRelocPIC          : LLVMRelocStatic;

    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
        target, triple, cpu, features,
        toCodeGenLevel(optLevel(state)),
        reloc,
        LLVMCodeModelDefault
    );

    LLVMTargetDataRef layout    = LLVMCreateTargetDataLayout(machine);
    state->pointerSize          = LLVMPointerSize(layout);
    state->targetMachine        = machine;
    state->resolvedTriple       = triple;

    /* Copied into the state arena, like the triple, so only the machine needs disposing. */
    char *dataLayout            = LLVMCopyStringRepOfTargetData(layout);
    state->dataLayout           = zstrdup(state->allocator, dataLayout);
    LLVMDisposeMessage(dataLayout);

    LLVMDisposeTargetData(layout);
    return true;
}

void disposeTargetMachine(ZState *state) {
    if (!state->targetMachine) return;
    LLVMDisposeTargetMachine((LLVMTargetMachineRef)state->targetMachine);
    state->targetMachine = NULL;
}

/* ========== Emission ========== */

static inline void printPhase(ZState *state, const char *phase) {
    if (!state->cli.verbose) return;
    const char *format;
    float elapsed = timer_elapsed(state->phaseTime, &format);
    printf(COLOR_BOLD COLOR_CYAN "  %-11s " COLOR_RESET "%.2f%s\n", phase, elapsed, format);
}

static bool emitObjectFile(ZState *state, LLVMModuleRef mod,
        const char *filename, LLVMCodeGenFileType fileType) {
    if (state->cli.verbose) timer_start(&state->phaseTime);

    LLVMTargetMachineRef machine = (LLVMTargetMachineRef)state->targetMachine;
    char optlvl = optLevel(state);

    if (optlvl != '0') {
        char pipeline[32];
        buildOptPipeline(optlvl, pipeline, sizeof(pipeline));
        LLVMPassBuilderOptionsRef pb_opts = LLVMCreatePassBuilderOptions();
        LLVMErrorRef err = LLVMRunPasses(mod, pipeline, machine, pb_opts);
        LLVMDisposePassBuilderOptions(pb_opts);
        if (err) {
            char *msg = LLVMGetErrorMessage(err);
            zlog(state, NULL, Z4015, msg);
            LLVMDisposeErrorMessage(msg);
            return false;
        }
    }

    bool ok;
    if (state->cli.ltoMode != Z_LTO_OFF) {
        ok = (LLVMWriteBitcodeToFile(mod, filename) == 0);
        if (!ok) zlog(state, NULL, Z4016, filename);
    } else {
        char *errmsg = NULL;
        ok = !LLVMTargetMachineEmitToFile(
            machine, mod, (char *)filename, fileType, &errmsg);
        if (!ok) {
            zlog(state, NULL, Z4017, errmsg);
            LLVMDisposeMessage(errmsg);
        }
    }
    printPhase(state, "Optimizer:");
    return ok;
}

/* ========== Linking ========== */

static bool verifyModule(ZState *state, LLVMModuleRef mod) {
    if (state->cli.skipLLVMValidation) return true;

    char *errmsg = NULL;
    bool invalid = LLVMVerifyModule(mod, LLVMReturnStatusAction, &errmsg);
    if (invalid) {
        usize namelen;
        zlog(state, NULL, Z4018, LLVMGetModuleIdentifier(mod, &namelen), errmsg);
    }
    LLVMDisposeMessage(errmsg);
    return !invalid;
}

/* Lower one ZMIR module, verify it and link it into `dest`. */
static void linkModule(ZState *state, LLVMModuleRef dest, LLVMContextRef ctx, ZMIRModuleRef mir) {
    if (!mir) return;

    LLVMModuleRef mod = zmir2llvm(ctx, mir);
    if (!mod) return;

    LLVMSetTarget(mod, state->resolvedTriple);
    LLVMSetDataLayout(mod, state->dataLayout);

    if (!verifyModule(state, mod)) {
        LLVMDisposeModule(mod);
        return;
    }

    /* LLVMLinkModules2 takes ownership of `mod`, even on failure. */
    usize namelen;
    char *name = zstrdup(state->allocator, (char *)LLVMGetModuleIdentifier(mod, &namelen));
    if (LLVMLinkModules2(dest, mod)) {
        zlog(state, NULL, Z401A, name);
    }
}

static LLVMModuleRef mergeModules(ZState *state, LLVMContextRef ctx,
        ZMIRModuleRef *modules, const char *output) {
    LLVMModuleRef dest = LLVMModuleCreateWithNameInContext(
        output ? output : "a.out", ctx);
    LLVMSetTarget(dest, state->resolvedTriple);
    LLVMSetDataLayout(dest, state->dataLayout);

    for (usize i = 0; i < veclen(modules); i++) {
        linkModule(state, dest, ctx, modules[i]);
    }
    return dest;
}

static int linkExecutable(ZState *state, LLVMModuleRef mod, const char *output) {
    const char *objext = (state->cli.ltoMode != Z_LTO_OFF) ? ".tmp.bc" : ".tmp.o";
    size_t objnameLen = strlen(output) + strlen(objext) + 1;
    char *objfile = malloc(objnameLen);
    if (!objfile) {
        zlog(state, NULL, Z401E);
        return -1;
    }
    snprintf(objfile, objnameLen, "%s%s", output, objext);

    if (!emitObjectFile(state, mod, objfile, LLVMObjectFile)) {
        free(objfile);
        return -1;
    }

#ifdef _WIN32
    /* The PE linker uses the exact name given; ensure .exe extension. */
    char *outname_buf = NULL;
    {
        size_t n = strlen(output);
        if (n < 4 || strcmp(output + n - 4, ".exe") != 0) {
            outname_buf = (char *)malloc(n + 5);
            memcpy(outname_buf, output, n);
            memcpy(outname_buf + n, ".exe", 5);
            output = outname_buf;
        }
    }
#endif

    if (state->cli.verbose) timer_start(&state->phaseTime);

    int ret = zinc_lld_link(state->cli.nostdlib, objfile, output,
            (const char**)state->cli.extraArgs, veclen(state->cli.extraArgs));

    printPhase(state, "LLD Linker:");

    remove(objfile);
    free(objfile);
#ifdef _WIN32
    free(outname_buf);
#endif

    if (ret != 0) zlog(state, NULL, Z401F, ret);
    return ret;
}

void zcompile(ZState *state, ZNode *root, const char *output) {
    (void)root;

    if (state->cli.verbose) timer_start(&state->phaseTime);
    ZMIRModuleRef *modules = zgenModules(state);
    printPhase(state, "ZMIR IR:");

    if (!canAdvance(state)) return;
    if (state->cli.verbose) timer_start(&state->phaseTime);
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod  = mergeModules(state, ctx, modules, state->cli.output);
    printPhase(state, "LLVM IR:");

    if (!canAdvance(state)) goto end;

    if (state->cli.emit == Z_EMIT_EXE && !state->cli.nostdlib &&
        !LLVMGetNamedFunction(mod, "main")) {
        zlog(state, NULL, Z401B);
    }

    if (state->cli.verbose) timer_start(&state->phaseTime);
    char *errmsg = NULL;

    if (!state->cli.skipLLVMValidation && LLVMVerifyModule(mod, LLVMReturnStatusAction, &errmsg)) {
        zlog(state, NULL, Z401C, errmsg);
        LLVMDisposeMessage(errmsg);
        goto end;
    }
    LLVMDisposeMessage(errmsg);
    printPhase(state, "Verify:");

    switch (state->cli.emit) {
    case Z_EMIT_IR:
        if (!output) output = "output.ll";
        if (LLVMPrintModuleToFile(mod, output, &errmsg)) {
            zlog(state, NULL, Z401D, errmsg);
            LLVMDisposeMessage(errmsg);
            goto end;
        }
        goto success;
    case Z_EMIT_ASM:
        if (!output) output = "output.s";
        if (emitObjectFile(state, mod, output, LLVMAssemblyFile)) goto success;
        goto end;
    case Z_EMIT_OBJ:
        if (!output) output = "output.o";
        if (emitObjectFile(state, mod, output, LLVMObjectFile)) goto success;
        goto end;
    default: break;
    }

    if (!output) output = "a.out";
    if (linkExecutable(state, mod, output) != 0) goto end;

success:
    printf(COLOR_BLUE COLOR_BOLD "  Generated   " COLOR_RESET "%s\n", output);

end:
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
}
