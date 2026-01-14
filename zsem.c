#include "zsem.h"
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

#include "zparse.h"
#include "zdebug.h"

static void analyzeStmt(ZSemantic *, ZNode *);
static void analyzeBlock(ZSemantic *, ZNode *);

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
	return self;
}

static ZSymTable *makesymtable() {
	ZSymTable *self = zalloc(ZSymTable);
	self->global = makescope(NULL);
	self->current = self->global;
	return self;
}

static ZSemantic *makesemantic(ZNode *root) {
	ZSemantic *self = zalloc(ZSemantic);
	self->root = root;
	self->currentFuncRet = NULL;
	self->loopDepth = 0;

	self->table = makesymtable();
	return self;
}

static void putFunc(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol = makesymbol(Z_SYM_FUNC);

	if (!node) {
		printf("Some data missing\n");
		return;
	}
	symbol->name = node->funcDef.ident->str;
	symbol->type = node->funcDef.ret;
	symbol->node = node;

	vecpush(semantic->table->current->symbols, symbol);
}

static void putVar(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol = makesymbol(Z_SYM_FUNC);

	symbol->name = node->funcDef.ident->str;
	symbol->type = node->funcDef.ret;
	symbol->node = node;

	vecpush(semantic->table->current->symbols, symbol);
}

static void putStruct(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol = makesymbol(Z_SYM_FUNC);

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

bool typesEqual(ZType *a, ZType *b) {
	if (!a || !b) return false;

	if (a->kind != b->kind) return false;

	switch(a->kind) {
	case Z_TYPE_PRIMITIVE:
		return a->token->type == b->token->type;
	case Z_TYPE_POINTER:
		return typesEqual(a->base, b->base);
	case Z_TYPE_ARRAY:
		return typesEqual(a->array.base, b->array.base);
	case Z_TYPE_STRUCT:
		return a == b;
	case Z_TYPE_FUNCTION:
		return a == b;
	default:
		return false;
	}
}

static bool isLValue(ZNode *node) {
	if (node->type & TOK_LITERAL) return false;
	return true;
}

static bool isRValue(ZNode *node) {
	return true;
}

static ZType *analyzeExpr(ZSemantic *semantic, ZNode *curr) {
	if (curr->type == NODE_LITERAL) {
		return curr->resolved;
	} else if (curr->type != NODE_BINARY) {
		return NULL;
	}

	ZType *left = analyzeExpr(semantic, curr->binary.left);
	ZType *right = analyzeExpr(semantic, curr->binary.right);

	if (!typesEqual(left, right)) {
		printf("Mismatch type: \n");
		printType(left);
		printf("\n");
		printType(right);
		printf("\n");
	}


	if (curr->binary.op->type == TOK_EQ) {
		if (!isLValue(curr->binary.left)) {
			printf("Is not a value lvalue\n");
			return NULL;
		}
	}

	return left;
}

static void analyzeIf(ZSemantic *semantic, ZNode *curr) {
	analyzeExpr(semantic, curr->ifStmt.cond);
	analyzeBlock(semantic, curr->ifStmt.body);

	if (curr->ifStmt.elseBranch) {
		analyzeBlock(semantic, curr->ifStmt.elseBranch);
	}
}

static void analyzeWhile(ZSemantic *semantic, ZNode *curr) {
	analyzeExpr(semantic, curr->whileStmt.cond);
	analyzeBlock(semantic, curr->whileStmt.branch);
}

static void analyzeStmt(ZSemantic *semantic, ZNode *curr) {
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
	default:
		printf("(not yet implemented %d)\n", curr->type);
		break;
	}
}

static void analyzeBlock(ZSemantic *semantic, ZNode *block) {
	beginScope(semantic);
	ZNode **stmts = block->block;
	for (usize i = 0; i < veclen(stmts); i++) {
		analyzeStmt(semantic, stmts[i]);
	}
	endScope(semantic);
}

static void analyzeReturn(ZSemantic *semantic, ZNode *node) {
	ZType *ret = analyzeExpr(semantic, node->returnStmt.expr);

	if (!typesEqual(ret, semantic->currentFuncRet)) {
		printf("Invalid return statement for this function\n");
		printf("Given return type: ");
		printType(ret);
		printf("\nExpected return type: ");
		printType(semantic->currentFuncRet);
	}
}

static void analyzeFunc(ZSemantic *semantic, ZNode *curr) {
	printf("FUNC\n");
	putFunc(semantic, curr);

	semantic->currentFuncRet = curr->funcDef.ret;
	analyzeBlock(semantic, curr->funcDef.body);

	printf("END FUNC\n");
}

static void analyzeVar(ZSemantic *semantic, ZNode *curr) {
	putVar(semantic, curr);

	curr->varDecl.type;
}

static void analyzeStruct(ZSemantic *semantic, ZNode *curr) {
	putStruct(semantic, curr);
}

void zanalyze(ZNode *root) {
	ZSemantic *semantic = makesemantic(root);
	for (usize i = 0; i < veclen(root->program); i++) {
		ZNode *child = root->program[i];
		switch(child->type) {
		case NODE_FUNC:
			analyzeFunc(semantic, child);
			break;
		case NODE_VAR_DECL:
			analyzeVar(semantic, child);
			break;
		case NODE_STRUCT:
			analyzeStruct(semantic, child);
			break;
		case NODE_TYPEDEF:
			printf("aliase validated\n");
			break;
		default:
			fprintf(stderr, "Unexpected node\n");
			break;
		}
	}
}
