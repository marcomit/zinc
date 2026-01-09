#include "zsem.h"
#include "zparse.h"
#include "zvec.h"

typedef struct ZSemantic {

} ZSemantic;

// void checkStmt(ZSemantic *);
// void checkBlock(ZSemantic *);
// void checkExpr(ZSemantic *);
// void checkBinary(ZSemantic *);

static bool sameType(ZType *a, ZType *b) {
	if (!a && !b) return true;
	if (!a || !b) return false;
	if (a->kind != b->kind) return false;

	switch (a->kind) {
	case Z_TYPE_POINTER:
		return sameType(a->base, b->base);
	case Z_TYPE_FUNCTION:
		if (!sameType(a->func.ret, b->func.ret)) return false;
		if (veclen(a->func.args) != veclen(b->func.args)) return false;
		for (usize i = 0; i < veclen(a->func.args); i++) {
			if (!sameType(a->func.args[i], b->func.args[i])) return false;
		}
		return true;

	case Z_TYPE_PRIMITIVE:
		return a->token->type == b->token->type;
	case Z_TYPE_STRUCT:
		// TODO
		return true;
	case Z_TYPE_ARRAY:
		// TODO
		return false;
	default:
		break;
	}
	return true;
}

void analyze(ZNode *root) {
	(void)root;
}
