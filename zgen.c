#include "base.h"
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

	/* Struct type cache — parallel arrays keyed by name */
	char        **structNames;
	LLVMTypeRef  *structTypes;
} ZCodegen;

static LLVMValueRef genExpr(ZCodegen *, ZNode *);

/* ========== Native types ==========*/
static LLVMTypeRef i0Type 	= NULL;
static LLVMTypeRef i1Type 	= NULL;
static LLVMTypeRef i8Type 	= NULL;
static LLVMTypeRef i16Type 	= NULL;
static LLVMTypeRef i32Type 	= NULL;
static LLVMTypeRef i64Type 	= NULL;

static LLVMTypeRef f32Type 	= NULL;
static LLVMTypeRef f64Type 	= NULL;

static void initNativeTypes(ZCodegen *ctx) {
	if (i8Type) return;
	i0Type	= LLVMVoidTypeInContext(ctx->ctx);
	i1Type 	= LLVMInt1TypeInContext(ctx->ctx);
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
	self->structNames = NULL;
	self->structTypes = NULL;
	beginModule(self, "main");
	self->builder = LLVMCreateBuilderInContext(self->ctx);
	self->state = state;
	self->semantic = semantic;
	return self;
}

usize typeSize(ZType *type) {
	usize res = 0;
	switch (type->kind) {
	case Z_TYPE_PRIMITIVE:
		switch (type->primitive.token->type) {
		case TOK_VOID:  return 0;
		case TOK_BOOL:
		case TOK_I8:
		case TOK_U8:
		case TOK_CHAR:  return 1;
		case TOK_I16:
		case TOK_U16:   return 2;
		case TOK_I32:
		case TOK_U32:
		case TOK_F32:   return 4;
		case TOK_I64:
		case TOK_U64:
		case TOK_F64:   return 8;
		default:        return 0;
		}
	case Z_TYPE_POINTER:
		return 8; /* 64-bit pointer */
	case Z_TYPE_ARRAY:
		return typeSize(type->array.base) * type->array.size;
	case Z_TYPE_FUNCTION:
		return 8; /* function pointer */
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
	if (!type) return LLVMVoidTypeInContext(ctx->ctx);

	switch (type->kind) {

	case Z_TYPE_PRIMITIVE: {
		const ZToken *name = type->primitive.token;
		switch (name->type) {
		case TOK_VOID:  return i0Type;
		case TOK_BOOL:  return i0Type;
		case TOK_CHAR:
		case TOK_I8:
		case TOK_U8: 	return i0Type;
		case TOK_I16:
		case TOK_U16: 	return i16Type;
		case TOK_I32:
		case TOK_U32:	return i32Type;
		case TOK_I64:
		case TOK_U64: 	return i64Type;
		case TOK_F32:	return f32Type;
		case TOK_F64:	return f64Type;
		default:
			error(ctx->state,
						type->primitive.token,
						"unknown primitive type '%s'",
						name->str);
			return NULL;
		}
	}

	case Z_TYPE_POINTER: {
		/* void* → i8* (LLVM doesn't allow pointer-to-void) */
		ZType *base = type->base;
		if (base && base->kind == Z_TYPE_PRIMITIVE &&
		    strcmp(base->primitive.token->str, "void") == 0) {
			return LLVMPointerType(i8Type, 0);
		}
		return LLVMPointerType(genType(ctx, base), 0);
	}

	case Z_TYPE_ARRAY: {
		LLVMTypeRef base = genType(ctx, type->array.base);
		if (!base) return NULL;
		return LLVMArrayType(base, (unsigned)type->array.size);
	}

	case Z_TYPE_FUNCTION: {
		usize argc = veclen(type->func.args);
		LLVMTypeRef *params = znalloc(LLVMTypeRef, argc ? argc : 1);
		for (usize i = 0; i < argc; i++) {
			params[i] = genType(ctx, type->func.args[i]);
			if (!params[i]) return NULL;
		}
		LLVMTypeRef ret = genType(ctx, type->func.ret);
		return LLVMFunctionType(ret, params, (unsigned)argc, 0);
	}

	case Z_TYPE_STRUCT: {
		const char *name = type->strct.name->str;

		/* Check cache */
		for (usize i = 0; i < veclen(ctx->structNames); i++) {
			if (strcmp(ctx->structNames[i], name) == 0)
				return ctx->structTypes[i];
		}

		/* Create opaque named struct first (handles self-referential types) */
		LLVMTypeRef strct = LLVMStructCreateNamed(ctx->ctx, name);
		vecpush(ctx->structNames, (char *)name);
		vecpush(ctx->structTypes, strct);

		usize nfields = veclen(type->strct.fields);
		LLVMTypeRef *ftypes = znalloc(LLVMTypeRef, nfields ? nfields : 1);
		for (usize i = 0; i < nfields; i++) {
			ftypes[i] = genType(ctx, type->strct.fields[i]->field.type);
			if (!ftypes[i]) return NULL;
		}
		LLVMStructSetBody(strct, ftypes, (unsigned)nfields, /*packed=*/0);
		return strct;
	}

	case Z_TYPE_TUPLE: {
		usize len = veclen(type->tuple);
		LLVMTypeRef *elems = znalloc(LLVMTypeRef, len ? len : 1);
		for (usize i = 0; i < len; i++) {
			elems[i] = genType(ctx, type->tuple[i]);
			if (!elems[i]) return NULL;
		}
		return LLVMStructTypeInContext(ctx->ctx, elems, (unsigned)len, 0);
	}

	case Z_TYPE_NONE:
		/* none literal — represent as i8* null */
		return LLVMPointerType(i8Type, 0);

	default:
		error(ctx->state, NULL, "genType: unhandled type kind %d", type->kind);
		return NULL;
	}
}

static LLVMTypeRef genStruct(ZCodegen *ctx, ZNode *node) {
	/* Delegate to genType via the resolved struct type on the node */
	if (node->resolved) return genType(ctx, node->resolved);

	/* Fallback: build directly from the AST node */
	const char *name = node->structDef.ident->str;
	for (usize i = 0; i < veclen(ctx->structNames); i++) {
		if (strcmp(ctx->structNames[i], name) == 0)
			return ctx->structTypes[i];
	}

	LLVMTypeRef strct = LLVMStructCreateNamed(ctx->ctx, name);
	vecpush(ctx->structNames, (char *)name);
	vecpush(ctx->structTypes, strct);

	usize nfields = veclen(node->structDef.fields);
	LLVMTypeRef *ftypes = znalloc(LLVMTypeRef, nfields ? nfields : 1);
	for (usize i = 0; i < nfields; i++) {
		ftypes[i] = genType(ctx, node->structDef.fields[i]->resolved);
	}
	LLVMStructSetBody(strct, ftypes, (unsigned)nfields, 0);
	return strct;
}

static LLVMValueRef genIntLit(ZCodegen *ctx, ZNode *node) {
    return LLVMConstInt(i32Type, node->tok->integer, true);
    return NULL;
}

static LLVMValueRef genBoolLit(ZCodegen *ctx, ZNode *node) {
    return LLVMConstInt(i1Type, node->tok->boolean, false);
    return NULL;
}

static LLVMValueRef genStrLit(ZCodegen *ctx, ZNode *node) {
    return LLVMConstStringInContext(ctx->ctx, "", 1, false);
    return NULL;
}

static LLVMValueRef genLit(ZCodegen *ctx, ZToken *tok) {
    switch (tok->type) {
    case TOK_STR_LIT:
        return LLVMConstStringInContext(ctx->ctx, tok->str, strlen(tok->str), false);
    case TOK_INT_LIT:
        return LLVMConstInt(i32Type, tok->integer, true);
    case TOK_BOOL_LIT:
        return LLVMConstInt(i1Type, tok->boolean, false);
    case TOK_FLOAT_LIT:
        return LLVMConstReal(f64Type, tok->floating);
    default: return NULL;
    }
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

static LLVMValueRef genCast(ZCodegen *ctx, LLVMValueRef val, ZType *from, ZType *to) {
	LLVMTypeRef toType = genType(ctx, to);
	if (!toType) return NULL;

	bool fromIsFloat = false, toIsFloat = false;
	bool fromIsPtr   = (from->kind == Z_TYPE_POINTER);
	bool toIsPtr     = (to->kind   == Z_TYPE_POINTER);
	bool fromIsSigned = false;

	if (from->kind == Z_TYPE_PRIMITIVE) {
		ZTokenType tt = from->primitive.token->type;
		fromIsFloat   = (tt == TOK_F32 || tt == TOK_F64);
		fromIsSigned  = (tt & TOK_SIGNED) != 0;
	}
	if (to->kind == Z_TYPE_PRIMITIVE) {
		ZTokenType tt = to->primitive.token->type;
		toIsFloat = (tt == TOK_F32 || tt == TOK_F64);
	}

	if (fromIsPtr && toIsPtr)
		return LLVMBuildBitCast(ctx->builder, val, toType, "ptrtmp");

	if (fromIsPtr && !toIsFloat)
		return LLVMBuildPtrToInt(ctx->builder, val, toType, "ptrtmp");

	if (!fromIsFloat && toIsPtr)
		return LLVMBuildIntToPtr(ctx->builder, val, toType, "ptrtmp");

	if (fromIsFloat && toIsFloat) {
		/* f64 -> f32: truncate; f32 -> f64: extend */
		if (from->primitive.token->type == TOK_F64 &&
		    to->primitive.token->type   == TOK_F32)
			return LLVMBuildFPTrunc(ctx->builder, val, toType, "fptmp");
		return LLVMBuildFPExt(ctx->builder, val, toType, "fptmp");
	}

	if (fromIsFloat)
		return fromIsSigned
			? LLVMBuildFPToSI(ctx->builder, val, toType, "fptmp")
			: LLVMBuildFPToUI(ctx->builder, val, toType, "fptmp");

	if (toIsFloat)
		return fromIsSigned
			? LLVMBuildSIToFP(ctx->builder, val, toType, "fptmp")
			: LLVMBuildUIToFP(ctx->builder, val, toType, "fptmp");

	/* Both integers: trunc or extend */
	LLVMTypeRef fromType = LLVMTypeOf(val);
	unsigned fromBits = LLVMGetIntTypeWidth(fromType);
	unsigned toBits   = LLVMGetIntTypeWidth(toType);
	if (fromBits > toBits)
		return LLVMBuildTrunc(ctx->builder, val, toType, "trunctmp");
	if (fromBits < toBits)
		return fromIsSigned
			? LLVMBuildSExt(ctx->builder, val, toType, "exttmp")
			: LLVMBuildZExt(ctx->builder, val, toType, "exttmp");

	return LLVMBuildBitCast(ctx->builder, val, toType, "casttmp");
}

static LLVMValueRef genExpr(ZCodegen *ctx, ZNode *node) {
	switch (node->type) {
		case NODE_BINARY:
			return genBinary(ctx, node);

        case NODE_LITERAL:
            return genLit(ctx, node->tok);

		case NODE_CAST: {
			LLVMValueRef val = genExpr(ctx, node->castExpr.expr);
			if (!val) return NULL;
			ZType *from = node->castExpr.expr->resolved;
			ZType *to   = node->castExpr.toType;
			return genCast(ctx, val, from, to);
		}

		case NODE_SIZEOF: {
			usize size = typeSize(node->sizeofExpr.type);
			return LLVMConstInt(i64Type, (unsigned long long)size, /*sign_extend=*/0);
		}

		default: break;
	}
	return NULL;
}

static LLVMValueRef genForeign(ZCodegen *ctx, ZNode *node) {
	LLVMTypeRef ret = genType(ctx, node->foreignFunc.ret);
	usize argc = veclen(node->foreignFunc.args);

	LLVMTypeRef *paramTypes = znalloc(LLVMTypeRef, argc ? argc : 1);
	for (usize i = 0; i < argc; i++) {
		paramTypes[i] = genType(ctx, node->foreignFunc.args[i]);
	}
	LLVMTypeRef funcType = LLVMFunctionType(ret, paramTypes, (unsigned)argc, 0);

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

	if (state->emitLLVM) {
		const char *llfile = output ? output : "output.ll";
		if (LLVMPrintModuleToFile(ctx->mod, llfile, &errmsg)) {
			error(state, NULL, "Failed to write IR file: %s", errmsg);
			LLVMDisposeMessage(errmsg);
		} else {
			printf("LLVM IR written to %s\n", llfile);
		}
		freeCodegen(ctx);
		return;
	}

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
