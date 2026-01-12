#include "zsem.h"
#include "zparse.h"

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
	self->current = root;
	self->root = root;

	self->table = makesymtable();
	return self;
}

static void putFunc(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol = makesymbol(Z_SYM_FUNC);

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

static ZType *analyzeExpr(ZSemantic *semantic, ZNode *curr) {
	if (curr->type == NODE_LITERAL) {
		return curr->resolved;
	} else if (curr->type != NODE_BINARY) {
		return NULL;
	}

	ZType *left = analyzeExpr(semantic, curr->binary.left);
	ZType *right = analyzeExpr(semantic, curr->binary.right);

	if (!typesEqual(left, right)) {
		printf("Mismatch type");
	}
	
	switch (curr->binary.op->type) {
	case TOK_EQEQ:
		break;
	default:
		printf("Invalid operator\n");
		return NULL;
		break;
	}

	return NULL;
}

static void analyzeIf(ZSemantic *semantic, ZNode *curr) {
	analyzeExpr(semantic, curr->ifStmt.cond);
	analyzeBlock(semantic, curr->ifStmt.body);

	if (curr->ifStmt.elseBranch) {
		analyzeBlock(semantic, curr->ifStmt.elseBranch);
	}
}

static void analyzeStmt(ZSemantic *semantic, ZNode *curr) {
	switch (semantic->current->type) {
	case NODE_IF: 
		analyzeIf(semantic, curr);
		break;
	case NODE_WHILE:
		break;
	default:
		printf("(not yet implemented)\n");
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

static void analyzeFunc(ZSemantic *semantic, ZNode *curr) {
	if (semantic->root->type != NODE_PROGRAM) return;
	putFunc(semantic, curr);
	analyzeBlock(semantic, curr->funcDef.body);
}

static void analyzeVar(ZSemantic *semantic, ZNode *curr) {
	if (semantic->root->type != NODE_PROGRAM) return;
	putVar(semantic, curr);
}

static void analyzeStruct(ZSemantic *semantic, ZNode *curr) {
	if (semantic->root->type != NODE_PROGRAM) return;
	putStruct(semantic, curr);
}

void zanalyze(ZNode *root) {
	ZSemantic *semantic = makesemantic(root);
	for (usize i = 0; i < veclen(root->program); i++) {
		ZNode *child = root->program[i];
		switch(child->type) {
		case NODE_FUNC:
			analyzeFunc(semantic, root);
			break;
		case NODE_VAR_DECL:
			analyzeVar(semantic, root);
			break;
		case NODE_STRUCT:
			analyzeStruct(semantic, root);
			break;
		default:
			fprintf(stderr, "Unexpected node\n");
			break;
		}
	}
}
