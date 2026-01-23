/* This file is the Semantic analyzer.
 *
 * Now what is a semantic analyzer?
 * In this part of the compiler/interpreter
 * we have already built the AST (Abstract Syntax Tree) and 
 * we want to make sure that all the types are correctly.
 * for example this expression 1230 + "hello"
 * is not valid because i cannot add a number to a string
 * but the parser parse it correctly. so we need to check every single expression/statement
 * that all types are correctly.
 * In this phase we also care about the scope of functions/variables.
 *
 * How do we handle the scope?
 *
 * We have a global scope for the entire project,
 * then a child scope for the current file and then a child for blocks like functions, loops etc..
 * */
#include "zinc.h"
#include <ctype.h>

static void analyzeStmt(ZSemantic *, ZNode *);
static void analyzeBlock(ZSemantic *, ZNode *, bool);

static ZScope *makescope(ZScope *parent) {
	ZScope *self = zalloc(ZScope);

	self->depth = parent ? parent->depth + 1 : 0;
	self->parent = parent;
	self->symbols = NULL;

	return self;
}

static ZSymbol *makesymbol(ZSymType kind) {
	ZSymbol *self = zalloc(ZSymbol);
	self->kind = kind;
	self->isPublic = false;
	return self;
}

static ZSymTable *makesymtable() {
	ZSymTable *self = zalloc(ZSymTable);
	self->global = makescope(NULL);
	self->current = self->global;
	return self;
}

static ZSemantic *makesemantic(ZState *state, ZNode *root) {
	ZSemantic *self = zalloc(ZSemantic);
	self->root = root;
	self->currentFuncRet = NULL;
	self->loopDepth = 0;
	self->state = state;

	self->table = makesymtable();
	return self;
}

static bool isPublic(char *name) {
	return isupper(*name);
}

static void putSymbol(ZSemantic *semantic, ZSymbol *symbol) {
	ZScope *scope = semantic->table->current;
	if (symbol->isPublic) scope = semantic->table->global;
	vecpush(scope->symbols, symbol);
}

static void putFunc(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol = makesymbol(Z_SYM_FUNC);

	symbol->name = node->funcDef.ident->str;
	symbol->type = node->funcDef.ret;
	symbol->node = node;
	symbol->isPublic = isPublic(symbol->name);

	putSymbol(semantic, symbol);
}

static void putGlobalVar(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol = makesymbol(Z_SYM_VAR);
	symbol->node = node;
	putSymbol(semantic, symbol);
}

static void putVar(ZSemantic *semantic, ZNode *node, bool canBeGlobal) {
	ZSymbol *symbol = makesymbol(Z_SYM_VAR);
	symbol->node = node;
	symbol->name = node->varDecl.ident->str;
	symbol->isPublic = canBeGlobal && isPublic(node->varDecl.ident->str);
	putSymbol(semantic, symbol);
}

static void putStruct(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol = makesymbol(Z_SYM_STRUCT);

	symbol->name = node->structDef.ident->str;
	symbol->node = node;

	vecpush(semantic->table->current->symbols, symbol);
}

static void beginScope(ZSemantic *semantic) {
	ZScope *scope = makescope(semantic->table->current);
	semantic->table->current = scope;
}

static void endScope(ZSemantic *semantic) {
	if (!semantic->table->current || !semantic->table->current->parent) {
		printf("Cannot escape this scope because it is already the highest scope\n");
		return;
	}
	semantic->table->current = semantic->table->current->parent;
}

static u8 typeRank(ZTokenType t) {
	switch (t) {
	case TOK_CHAR: return 0;
	case TOK_I8: 	case TOK_U8: 	return 1;
	case TOK_I16: case TOK_U16: return 2;
	case TOK_I32: case TOK_U32: return 3;
	case TOK_I64: case TOK_U64: return 4;
	case TOK_F32: return 5;
	case TOK_F64: return 6;
	default: 			return 0;
	}
}

static inline bool isUnsigned	(ZTokenType t) { return t & TOK_UNSIGNED; 	}
static inline bool isSigned		(ZTokenType t) { return t & TOK_SIGNED; 		}
static inline bool isFloat		(ZTokenType t) { return t & TOK_FLOAT;			}
static inline bool isInteger	(ZTokenType t) { return isSigned(t) || isUnsigned(t); }

static ZTokenType toUnsigned(u8 rank) {
	switch (rank) {
	case 1: return TOK_U8;
	case 2: return TOK_U16;
	case 3: return TOK_U32;
	case 4: return TOK_U64;
	default: return TOK_U32;
	}
}

static ZTokenType toSigned(u8 rank) {
	switch (rank) {
	case 1: return TOK_I8;
	case 2: return TOK_I16;
	case 3: return TOK_I32;
	case 4: return TOK_I64;
	default: return TOK_I32;
	}
}

/* An implementation note:
 * if a or b is a float the return type is always a float.
 * if a and b are both signed or unsigned return the type with the highest rank.
 * if they are unsigned vs signed and the unsigned is u64 can't promote to i128
 * so in this case the compiler shows a warning (explicit casting requested).
 * */
ZType *typesCompatible(ZState *state, ZType *a, ZType *b) {
	if (!a || !b) return NULL;
	if (typesEqual(a, b)) return a;

	if (a->kind != Z_TYPE_PRIMITIVE || b->kind != Z_TYPE_PRIMITIVE) {
		return NULL;
	}

	ZTokenType ta = a->primitive.token->type;
	ZTokenType tb = b->primitive.token->type;

	if (ta == TOK_VOID || tb == TOK_VOID) return NULL;

	u8 ra = typeRank(ta);
	u8 rb = typeRank(tb);

	// f32 + i32 -> f64
	// f32 + i64 -> f64
	if (isFloat(ta) || isFloat(tb)) {
		u8 minRank = min(ra, rb);
		if (minRank <= 2) {
			return ra > rb ? a : b;
		}

		ZType *promoted = maketype(Z_TYPE_PRIMITIVE);
		promoted->primitive.token = maketoken(TOK_F64);

		return promoted;
	}

	// Both integers

	// Both signed or both unsigned return the largest.
	if ((isSigned(ta) && isSigned(tb)) ||
			(isUnsigned(ta) && isUnsigned(tb))) {
		return ra > rb ? a : b;
	}

	// signed vs unsigned types
	u8 signedRank = isSigned(ta) ? ra : rb;
	u8 unsignedRank = isSigned(ta) ? rb : ra;

	ZType *signedType = isSigned(ta) ? a : b;
	if (signedRank > unsignedRank) return signedType;

	// Highest rank reached
	if (signedRank == 4) {
		warning(state, signedType->primitive.token, "Cannot promote an i64, try with an explicit casting");
	}

	ZType *promoted = maketype(Z_TYPE_PRIMITIVE);
	promoted->primitive.token = maketoken(toSigned(signedRank + 1));
	
	return promoted;
}

bool typesEqual(ZType *a, ZType *b) {
	if (!a || !b) return false;

	if (a->kind != b->kind) return false;

	switch(a->kind) {
	case Z_TYPE_PRIMITIVE:
		return a->primitive.token->type == b->primitive.token->type;
	case Z_TYPE_POINTER:
		return typesEqual(a->base, b->base);
	case Z_TYPE_ARRAY:
		return typesEqual(a->array.base, b->array.base);
	case Z_TYPE_STRUCT:
		return a == b;
	case Z_TYPE_FUNCTION:
		return a == b;
	case Z_TYPE_TUPLE:
		if (veclen(a->tuple) != veclen(b->tuple)) return false;
		for (usize i = 0; i < veclen(a->tuple); i++) {
			if (!typesEqual(a->tuple[i], b->tuple[i])) return false;
		}
		return true;
	case Z_TYPE_GENERIC:
		// Compare the base type name and all generic arguments
		if (strcmp(a->generic.name->str, b->generic.name->str) != 0) return false;
		if (veclen(a->generic.args) != veclen(b->generic.args)) return false;
		for (usize i = 0; i < veclen(a->generic.args); i++) {
			if (!typesEqual(a->generic.args[i], b->generic.args[i])) return false;
		}
		return true;
	default:
		return false;
	}
}

static ZSymbol *resolve(ZSemantic *semantic, ZToken *ident) {
	printf("Trying to resolve %s\n", ident->str);
	ZScope *curr = semantic->table->current;
	while (curr) {
		for (usize i = 0; i < veclen(curr->symbols); i++) {
			if (strcmp(curr->symbols[i]->name, ident->str) == 0) {
				return curr->symbols[i];
			}
		}
		curr = curr->parent;
	}

	return NULL;
}

static void analyzeMemberAccess(ZSemantic *semantic, ZNode *curr) {
	ZToken *type = curr->memberAccess.field;
	ZNode *obj = curr->memberAccess.object;
	if (obj->type == NODE_IDENTIFIER) {
		ZSymbol *sym = resolve(semantic, obj->identTok);
		if (!sym) {
			fprintf(stderr, "Local variable not found!");
		}
	}
}

static void validateUnary(ZSemantic *semantic, ZNode *curr) {
	switch(curr->type) {
	case NODE_MEMBER:
		break;
	case NODE_CALL:

		break;
	default:
		warning(semantic->state, curr->tok, "(not yet implemented %d)\n", curr->type);
		break;
	}
}

static bool isLValue(ZNode *node) {
	if (node->type & TOK_LITERAL) return false;
	return true;
}

static bool isRValue(ZNode *node) {
	return true;
}

static void analyzeExpr(ZSemantic *semantic, ZNode *curr) {
	if (curr->resolved) return;

	printNode(curr, 2);
	printf("\n");

	if (curr->type == NODE_IDENTIFIER) {
		printf("Trying to reslove\n");
		ZSymbol *resolved = resolve(semantic, curr->identTok);
		if (!resolved) {
			error(semantic->state, curr->identTok, "Undefined variable '%s'", curr->identTok);
			return;
		}
		printf("Resolved\n");
		curr->resolved = resolved->type;
		return;
	}
	if (curr->type != NODE_BINARY) {
		printf("Called analyzeExpr with an invalid node %d\n", curr->type);
		return;
	}

	if (curr->binary.op->type == TOK_EQ) {
		if (!isLValue(curr->binary.left)) {
		}
	}

	analyzeExpr(semantic, curr->binary.left);
	analyzeExpr(semantic, curr->binary.right);

	ZType *promoted = typesCompatible(semantic->state,
																	 	curr->binary.left->resolved,
																	 	curr->binary.right->resolved);
	if (!promoted) {
		error(semantic->state, curr->binary.op, "Incompatible pointer");
	}

	curr->resolved = promoted;
}

static void analyzeIf(ZSemantic *semantic, ZNode *curr) {
	analyzeExpr(semantic, curr->ifStmt.cond);
	analyzeBlock(semantic, curr->ifStmt.body, true);

	if (curr->ifStmt.elseBranch) {
		analyzeBlock(semantic, curr->ifStmt.elseBranch, true);
	}
}

static void analyzeWhile(ZSemantic *semantic, ZNode *curr) {
	analyzeExpr(semantic, curr->whileStmt.cond);
	analyzeBlock(semantic, curr->whileStmt.branch, true);
}

static void analyzeStmt(ZSemantic *semantic, ZNode *curr) {
	printf("AnalyzeStmt(%d)\n", curr->type);
	switch (curr->type) {
	case NODE_IF: 
		analyzeIf(semantic, curr);
		break;
	case NODE_WHILE:
		analyzeWhile(semantic, curr);
		break;
	case NODE_BINARY:
		analyzeExpr(semantic, curr);
		break;
	case NODE_IDENTIFIER:
		printNode(curr, 0);
		break;
	case NODE_RETURN:
		analyzeExpr(semantic, curr->returnStmt.expr);
			printf("Return expression analyzed\n");
		if (!typesEqual(curr->resolved, semantic->currentFuncRet)) {
			printType(curr->resolved);
			printf("\n");
			printType(semantic->currentFuncRet);
			printf("\n");
			error(semantic->state, NULL, "Invalid return type");
		}
		break;
	default:
		printf("not yet implemented %d\n", curr->type);
		// warning(semantic->state, NULL, "(not yet implemented %d)\n", curr->type);
		break;
	}
}

static void analyzeBlock(ZSemantic *semantic, ZNode *block, bool scoped) {
	if (scoped) beginScope(semantic);
	ZNode **stmts = block->block;
	for (usize i = 0; i < veclen(stmts); i++) {
		analyzeStmt(semantic, stmts[i]);
	}
	if (scoped) endScope(semantic);
}

static void analyzeReturn(ZSemantic *semantic, ZNode *node) {
	analyzeExpr(semantic, node->returnStmt.expr);

	if (!typesEqual(node->resolved, semantic->currentFuncRet)) {
		printf("Invalid return statement for this function\n");
		printf("Given return type: ");
		printType(node->resolved);
		printf("\nExpected return type: ");
		printType(semantic->currentFuncRet);
	}
}

static void analyzeFunc(ZSemantic *semantic, ZNode *curr) {
	printf("FUNC %s\n", curr->funcDef.ident->str);

	beginScope(semantic);
	for(usize i = 0; i < veclen(curr->funcDef.args); i++) {
		putVar(semantic, curr, false);
	}
	printf("Added %zu arguments to the local scope\n", veclen(curr->funcDef.args));

	semantic->currentFuncRet = curr->funcDef.ret;
	printScope(semantic->table->current);
	analyzeBlock(semantic, curr->funcDef.body, false);
	endScope(semantic);

	printf("END FUNC %s\n", curr->funcDef.ident->str);
}

static void analyzeVar(ZSemantic *semantic, ZNode *curr) {
	putVar(semantic, curr, false);
}

static void analyzeStruct(ZSemantic *semantic, ZNode *curr) {
	putStruct(semantic, curr);
}

/* Discover all global variables/functions/data
 * */
static void discoverGlobalScope(ZSemantic *semantic, ZNode *root) {
	for (usize i = 0; i < veclen(root->program); i++) {
		ZNode *child = root->program[i];

		switch (child->type) {
		case NODE_FUNC:
			putFunc(semantic, child);
			break;
		case NODE_STRUCT:
			putStruct(semantic, child);
			break;
		case NODE_VAR_DECL:
			putVar(semantic, child, true);
			break;
		case NODE_MODULE:
			beginScope(semantic);
			discoverGlobalScope(semantic, child->module.root);
			endScope(semantic);
			break;
		default:
			break;
		}
	}
}

static void analyze(ZSemantic *semantic, ZNode *root) {
	for (usize i = 0; i < veclen(root->program); i++) {
		ZNode *child = root->program[i];
		switch (child->type) {
		case NODE_VAR_DECL:
			analyzeFunc(semantic, child);
			break;
		case NODE_STRUCT:
			analyzeStruct(semantic, child);
			break;
		case NODE_FUNC:
			analyzeFunc(semantic, child);
			break;
		case NODE_MODULE:
			beginScope(semantic);
			analyze(semantic, child);
			endScope(semantic);
			break;
		default:
			break;
		}
	}
}

void zanalyze(ZState *state, ZNode *root) {
	ZSemantic *semantic = makesemantic(state, root);

	printf("Discover global variables\n");
	discoverGlobalScope(semantic, root);
	printf("Discovered global variables\n let's analyze\n");

	analyze(semantic, root);
}
