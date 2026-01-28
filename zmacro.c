#include "zinc.h"

ZNode *getMacroVar(ZNode *macro, ZToken *tok) {
	ZMacroVar **capt = macro->macro.captured;
	for (usize i = 0; i < veclen(capt); i++) {
		if (tokeneq(capt[i]->name, tok)) {
			return capt[i]->captured;
		}
	}

	return NULL;
}

static bool matchMacroPattern(ZParser *parser,
																ZNode *macro,
																ZMacroPattern *pattern) {
	ZNode *node = NULL;
	ZType *type = NULL;
	ZMacroVar *macrovar = zalloc(ZMacroVar);
	macrovar->name = macro->macro.ident;
	switch (pattern->kind) {
		case Z_MACRO_EXPR:
			node = parseExpr(parser);
			break;
		case Z_MACRO_IDENT:
			if (!check(parser, TOK_IDENT)) return NULL;
			node = makenode(NODE_IDENTIFIER);
			node->tok = consume(parser);
			break;
		case Z_MACRO_TYPE:
			type = parseType(parser);
			node = makenode(NODE_TYPE);
			node->resolved = type;
			break;
		case Z_MACRO_KEY:
			if (!checkMask(parser, TOK_OVERRIDABLE)) {
				node = makenode(NODE_IDENTIFIER);
				node->tok = consume(parser);
			}
			break;
		case Z_MACRO_SEQ:
			for (usize i = 0; i < veclen(pattern->sequence); i++) {
				matchMacroPattern(parser, macro, pattern->sequence[i]);
			}
			break;
		default: return false;
	}

	if (!node) return false;

	macrovar->captured = node;
	vecpush(macro->macro.captured, macrovar);
	return true;
}

ZNode *getMacroByName(ZParser *parser, usize *start) {
	ZToken *token = peek(parser);
	for (usize i = start ? *start : 0; i < veclen(parser->macros); i++) {
		if (strcmp(parser->macros[i]->macro.ident->str, token->str) == 0) {
			if (start) *start = i;
			return parser->macros[i];
		}
	}
	if (start) *start = veclen(parser->macros);
	return NULL;
}

ZNode *expandMacro(ZParser *parser) {
	usize i = 0;
	ZNode *macro = NULL;

	while (( macro = getMacroByName(parser, &i) )) {
		matchMacroPattern(parser, macro, macro->macro.pattern);
	}
	return NULL;
}
