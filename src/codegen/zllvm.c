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
#include "zllvm.h"
#include "zcolors.h"
#include "zgen.h"
#include "zinc.h"
#include "zlink.h"

LLVMModuleRef zllvmLowerModule(LLVMContextRef ctx, ZMIRModuleRef mod) {
    (void)ctx; (void)mod;
    /* TODO: translate the ZMIR instructions to LLVM IR. */
    return NULL;
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

    LLVMModuleRef mod = zllvmLowerModule(ctx, mir);
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
