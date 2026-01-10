#include "zsem.h"
#include "zparse.h"
#include "zvec.h"
#include "zmem.h"

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
		return a->strct.name == b->strct.name;
	case Z_TYPE_FUNCTION:

	default:
		return false;
	}
}

static void analyzeFunc(ZSemantic *semantic) {

}

static void analyzeBinary(ZSemantic *semantic) {

}

void analyze(ZNode *root) {
	(void)root;
}
