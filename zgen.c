#include "zinc.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>

typedef struct {
	LLVMContextRef ctx;
	LLVMModuleRef mod;
	LLVMBuilderRef builder;
	ZState *state;
} ZCodegen;

static LLVMValueRef genExpr(ZCodegen *, ZNode *);

ZCodegen *makecodegen(ZState *state) {
	ZCodegen *self = zalloc(ZCodegen);
	self->ctx = LLVMContextCreate();
	self->mod = LLVMModuleCreateWithNameInContext("Main", self->ctx);
	self->builder = LLVMCreateBuilderInContext(self->ctx);
	self->state = state;
	return self;
}

LLVMValueRef genBinary(ZCodegen *ctx, ZNode *root) {
	LLVMValueRef left = genExpr(ctx, root->binary.left);
	LLVMValueRef right = genExpr(ctx, root->binary.right);

	if (!left || !right) return NULL;

	ZTokenType op = root->binary.op->type;
	LLVMTypeRef left_type = LLVMTypeOf(left);
	bool is_float = (LLVMGetTypeKind(left_type) == LLVMFloatTypeKind ||
	                 LLVMGetTypeKind(left_type) == LLVMDoubleTypeKind);

	switch (op) {
	// Arithmetic operations
	case TOK_PLUS:
		return is_float
			? LLVMBuildFAdd(ctx->builder, left, right, "addtmp")
			: LLVMBuildAdd(ctx->builder, left, right, "addtmp");

	case TOK_MINUS:
		return is_float
			? LLVMBuildFSub(ctx->builder, left, right, "subtmp")
			: LLVMBuildSub(ctx->builder, left, right, "subtmp");

	case TOK_STAR:
		return is_float
			? LLVMBuildFMul(ctx->builder, left, right, "multmp")
			: LLVMBuildMul(ctx->builder, left, right, "multmp");

	case TOK_DIV:
		return is_float
			? LLVMBuildFDiv(ctx->builder, left, right, "divtmp")
			: LLVMBuildSDiv(ctx->builder, left, right, "divtmp");

	// Comparison operations
	case TOK_LT:
		return is_float
			? LLVMBuildFCmp(ctx->builder, LLVMRealOLT, left, right, "cmptmp")
			: LLVMBuildICmp(ctx->builder, LLVMIntSLT, left, right, "cmptmp");

	case TOK_GT:
		return is_float
			? LLVMBuildFCmp(ctx->builder, LLVMRealOGT, left, right, "cmptmp")
			: LLVMBuildICmp(ctx->builder, LLVMIntSGT, left, right, "cmptmp");

	case TOK_LTE:
		return is_float
			? LLVMBuildFCmp(ctx->builder, LLVMRealOLE, left, right, "cmptmp")
			: LLVMBuildICmp(ctx->builder, LLVMIntSLE, left, right, "cmptmp");

	case TOK_GTE:
		return is_float
			? LLVMBuildFCmp(ctx->builder, LLVMRealOGE, left, right, "cmptmp")
			: LLVMBuildICmp(ctx->builder, LLVMIntSGE, left, right, "cmptmp");

	case TOK_EQEQ:
		return is_float
			? LLVMBuildFCmp(ctx->builder, LLVMRealOEQ, left, right, "cmptmp")
			: LLVMBuildICmp(ctx->builder, LLVMIntEQ, left, right, "cmptmp");

	case TOK_NOTEQ:
		return is_float
			? LLVMBuildFCmp(ctx->builder, LLVMRealONE, left, right, "cmptmp")
			: LLVMBuildICmp(ctx->builder, LLVMIntNE, left, right, "cmptmp");

	default:
		fprintf(stderr, "Unknown binary operator\n");
		return NULL;
	}
}

static LLVMValueRef genExpr(ZCodegen *ctx, ZNode *node) {
	switch (node->type) {
		case NODE_BINARY:
			return genBinary(ctx, node);
			break;
		default: break;
	}
	return NULL;
}

static void compile(ZCodegen *ctx, ZNode *root) {
	switch (root->type) {
	case NODE_BINARY:
		genExpr(ctx, root);
		break;
	default:
		error(ctx->state, root->tok, "(compilation not yet implemented for %d)", root->type);
		break;
	}
}

static bool emitObjectFile(ZCodegen *ctx, const char *filename) {
	LLVMInitializeAllTargetInfos();
	LLVMInitializeAllTargets();
	LLVMInitializeAllTargetMCs();
	LLVMInitializeAllAsmParsers();
	LLVMInitializeAllAsmPrinters();

	char *triple = LLVMGetDefaultTargetTriple();
	LLVMSetTarget(ctx->mod, triple);

	LLVMTargetRef target;
	char *errmsg = NULL;
	if (LLVMGetTargetFromTriple(triple, &target, &errmsg)) {
		error(ctx->state, NULL, "Failed to get target: %s", errmsg);
		LLVMDisposeMessage(errmsg);
		LLVMDisposeMessage(triple);
		return false;
	}

	LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
		target, triple, "generic", "",
		LLVMCodeGenLevelDefault,
		LLVMRelocDefault,
		LLVMCodeModelDefault
	);

	if (LLVMTargetMachineEmitToFile(machine, ctx->mod, (char *)filename,
	                                 LLVMObjectFile, &errmsg)) {
		error(ctx->state, NULL, "Failed to emit object file: %s", errmsg);
		LLVMDisposeMessage(errmsg);
		LLVMDisposeTargetMachine(machine);
		LLVMDisposeMessage(triple);
		return false;
	}

	LLVMDisposeTargetMachine(machine);
	LLVMDisposeMessage(triple);
	return true;
}

static void freeCodegen(ZCodegen *ctx) {
	LLVMDisposeBuilder(ctx->builder);
	LLVMDisposeModule(ctx->mod);
	LLVMContextDispose(ctx->ctx);
	free(ctx);
}

void zcompile(ZState *state, ZNode *root, const char *output) {
	ZCodegen *ctx = makecodegen(state);
	compile(ctx, root);

	char *errmsg = NULL;
	if (LLVMVerifyModule(ctx->mod, LLVMReturnStatusAction, &errmsg)) {
		error(state, NULL, "Module verification failed: %s", errmsg);
		LLVMDisposeMessage(errmsg);
		freeCodegen(ctx);
		return;
	}
	LLVMDisposeMessage(errmsg);

	const char *objfile = "output.o";
	if (!emitObjectFile(ctx, objfile)) {
		freeCodegen(ctx);
		return;
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "cc %s -o %s", objfile, output ? output : "a.out");
	int ret = system(cmd);
	if (ret != 0) {
		error(state, NULL, "Linker failed with code %d", ret);
	}

	remove(objfile);

	freeCodegen(ctx);
}
