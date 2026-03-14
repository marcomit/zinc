#include "zinc.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>

typedef struct {
	LLVMContextRef ctx;

	LLVMModuleRef *modules;
	LLVMModuleRef mod;

	LLVMBuilderRef builder;
	ZSemantic *semantic;
	ZState *state;
} ZCodegen;

static LLVMValueRef genExpr(ZCodegen *, ZNode *);

/* ========== Native types ==========*/
static LLVMTypeRef i8Type 	= NULL;
static LLVMTypeRef i16Type 	= NULL;
static LLVMTypeRef i32Type 	= NULL;
static LLVMTypeRef i64Type 	= NULL;

static LLVMTypeRef f32Type 	= NULL;
static LLVMTypeRef f64Type 	= NULL;

static void initNativeTypes(ZCodegen *ctx) {
	if (i8Type) return;
	i8Type 	= LLVMInt8TypeInContext(ctx->ctx);
	i16Type = LLVMInt16TypeInContext(ctx->ctx);
	i32Type = LLVMInt32TypeInContext(ctx->ctx);
	i64Type = LLVMInt64TypeInContext(ctx->ctx);

	f32Type = LLVMFloatType();
	f64Type = LLVMDoubleType();
}

static void beginModule(ZCodegen *ctx, char *name) {
	LLVMModuleRef mod = LLVMModuleCreateWithNameInContext(name, ctx->ctx);
	vecpush(ctx->modules, mod);
	ctx->mod = mod;
}

static void endModule(ZCodegen *ctx) {
	if (veclen(ctx->modules) == 0) return;
	ctx->mod = vecpop(ctx->modules);
}

ZCodegen *makecodegen(ZState *state, ZSemantic *semantic) {
	ZCodegen *self = zalloc(ZCodegen);
	self->ctx = LLVMContextCreate();

	self->modules = NULL;
	beginModule(self, "main");
	self->builder = LLVMCreateBuilderInContext(self->ctx);
	self->state = state;
	self->semantic = semantic;
	return self;
}

usize typeSize(ZType *type) {
	usize res = 0;
	switch (type->kind) {
	case Z_TYPE_ARRAY:
		return typeSize(type->array.base) * type->array.size;
	case Z_TYPE_FUNCTION:
		return 64;
	case Z_TYPE_STRUCT: {
		for (usize i = 0; i < veclen(type->strct.fields); i++) {
			res += typeSize(type->strct.fields[i]->field.type);
		}
		return res;
	}
	case Z_TYPE_TUPLE:
		for (usize i = 0; i < veclen(type->tuple); i++) {
			res += typeSize(type->tuple[i]);
		}
		return res;
	default: return 0;
	}
}

static LLVMTypeRef genType(ZCodegen *ctx, ZType *type) {
	switch (type->kind) {
	case Z_TYPE_FUNCTION:
	case Z_TYPE_POINTER:
		return LLVMPointerType(genType(ctx, type->base), 0);
	case Z_TYPE_ARRAY: {
		LLVMTypeRef base = genType(ctx, type->array.base);
		return LLVMArrayType(base, type->array.size);
	}
	default: return NULL;
	}
	return NULL;
}

static LLVMTypeRef genStruct(ZCodegen *ctx, ZNode *node) {
	LLVMTypeRef strct = LLVMStructCreateNamed(ctx->ctx, node->structDef.ident->str);

	usize fieldsLen = veclen(node->structDef.fields);

	LLVMTypeRef fieldTypes[fieldsLen];
	ZNode **fields = node->structDef.fields;

	for (usize i = 0; i < fieldsLen; i++) {
		fieldTypes[i] = genType(ctx, fields[i]->resolved);
	}

	return strct;
}

static LLVMValueRef genCall(ZCodegen *ctx, ZNode *node) {
	(void)ctx;
	(void)node;
	return NULL;
}

static LLVMValueRef genMemberAccess(ZCodegen *ctx, ZNode *node) {
	return NULL;
}

static LLVMValueRef genBinary(ZCodegen *ctx, ZNode *root) {
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

static LLVMTypeRef convertType(ZType *type) {
	(void)type;
	return NULL;
}

static LLVMValueRef genForeign(ZCodegen *ctx, ZNode *node) {
	LLVMTypeRef ret = convertType(node->foreignFunc.ret);
	usize argc = veclen(node->foreignFunc.args);

	LLVMTypeRef *paramTypes = znalloc(LLVMTypeRef, argc);
	LLVMTypeRef funcType = LLVMFunctionType(ret, paramTypes, argc, 0);


	LLVMValueRef func = LLVMAddFunction(
		ctx->mod,
		node->foreignFunc.tok->str,
		funcType
	);
	return func;
}

static void compile(ZCodegen *ctx, ZNode *root) {
	switch (root->type) {
	case NODE_BINARY:
		genExpr(ctx, root);
		break;
	case NODE_FOREIGN:
		genForeign(ctx, root); 
		break;
	
	case NODE_MODULE: {
		beginModule(ctx, root->module.name);
		for (usize i = 0; i < veclen(root->module.root); i++) {
			compile(ctx, root->module.root[i]);
		}
		endModule(ctx);
		break;
	}
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
}

void genHelloWorld(ZCodegen *ctx) {
	LLVMTypeRef i32Type = LLVMInt32TypeInContext(ctx->ctx);
	LLVMTypeRef i8Type = LLVMInt8TypeInContext(ctx->ctx);
	LLVMTypeRef i8PtrType = LLVMPointerType(i8Type, 0);

	/* Declare: int puts(const char *) */
	LLVMTypeRef putsArgs[] = { i8PtrType };
	LLVMTypeRef putsType = LLVMFunctionType(i32Type, putsArgs, 1, 0);
	LLVMValueRef putsFn = LLVMAddFunction(ctx->mod, "puts", putsType);

	/* Define: int main(void) { ... } */
	LLVMTypeRef mainType = LLVMFunctionType(i32Type, NULL, 0, 0);
	LLVMValueRef mainFn = LLVMAddFunction(ctx->mod, "main", mainType);

	LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx->ctx, mainFn, "entry");
	LLVMPositionBuilderAtEnd(ctx->builder, entry);

	/* Build global string "Hello, World!" and call puts */
	LLVMValueRef helloStr = LLVMBuildGlobalStringPtr(ctx->builder, "Hello, World!", "str");
	LLVMValueRef putsCallArgs[] = { helloStr };
	LLVMBuildCall2(ctx->builder, putsType, putsFn, putsCallArgs, 1, "");

	/* return 0 */
	LLVMBuildRet(ctx->builder, LLVMConstInt(i32Type, 0, 0));
}

void zhelloworld(ZState *state, const char *output) {
	ZCodegen *ctx = makecodegen(state, NULL);
	genHelloWorld(ctx);

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
	snprintf(cmd, sizeof(cmd), "cc %s -o %s", objfile, output ? output : "hello");
	int ret = system(cmd);
	if (ret != 0) {
		error(state, NULL, "Linker failed with code %d", ret);
	}

	remove(objfile);
	freeCodegen(ctx);
}

void zcompile(ZState *state, ZNode *root, const char *output) {
	return;
	ZCodegen *ctx = makecodegen(state, NULL);
	initNativeTypes(ctx);
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
