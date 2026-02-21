#include "zinc.h"

static ZType *copytype(ZType *type);

static ZType **copytypevec(ZType **types) {
	if (!types) return NULL;
	ZType **copy = NULL;
	for (usize i = 0; i < veclen(types); i++) {
		vecpush(copy, copytype(types[i]));
	}
	return copy;
}

static ZType *copytype(ZType *type) {
	if (!type) return NULL;
	ZType *copy = maketype(type->kind);
	copy->constant = type->constant;

	switch (type->kind) {
		case Z_TYPE_PRIMITIVE:
			copy->primitive.token = type->primitive.token;
			copy->primitive.base = copytype(type->primitive.base);
			copy->primitive.generics = copytypevec(type->primitive.generics);
			break;
		case Z_TYPE_POINTER:
			copy->base = copytype(type->base);
			break;
		case Z_TYPE_STRUCT:
			copy->strct.name = type->strct.name;
			copy->strct.fields = NULL; // Fields are ZNode**, handled separately if needed
			copy->strct.generics = copytypevec(type->strct.generics);
			break;
		case Z_TYPE_FUNCTION:
			copy->func.ret = copytype(type->func.ret);
			copy->func.args = copytypevec(type->func.args);
			copy->func.generics = copytypevec(type->func.generics);
			break;
		case Z_TYPE_ARRAY:
			copy->array.base = copytype(type->array.base);
			copy->array.size = type->array.size;
			break;
		case Z_TYPE_TUPLE:
			copy->tuple = copytypevec(type->tuple);
			break;
		case Z_TYPE_GENERIC:
			copy->generic.name = type->generic.name;
			copy->generic.args = copytypevec(type->generic.args);
			break;
		default: break;
	}
	return copy;
}

bool macropatterneq(ZMacroPattern *p1, ZMacroPattern *p2) {
	if (!p1 || !p2) return false;
	if (p1->kind != p2->kind) return false;

	switch (p1->kind) {
	case Z_MACRO_SEQ:
		if (veclen(p1->sequence) != veclen(p2->sequence)) return false;
		for (usize i = 0; i < veclen(p1->sequence); i++) {
			if (!macropatterneq(p1->sequence[i], p2->sequence[i])) return false;
		}
		return true;
	case Z_MACRO_OM: return macropatterneq(p1->oneOrMore, p2->oneOrMore);
	case Z_MACRO_ZM: return macropatterneq(p1->zeroOrMore, p2->zeroOrMore);
	default: return true;
	}
}

bool macroeq(ZNode *m1, ZNode *m2) {
	if (!m1 || !m2) return false;
	if (!m1->macro.pattern || !m2->macro.pattern) return false;

	return macropatterneq(m1->macro.pattern, m2->macro.pattern);
}

ZNode *getMacroVar(ZNode *macro, ZToken *tok) {
	ZMacroVar **capt = macro->macro.captured;
	for (usize i = 0; i < veclen(capt); i++) {
		if (tokeneq(capt[i]->name, tok)) {
			return capt[i]->captured;
		}
	}

	return NULL;
}

static ZMacroVar *findCapturedVar(ZNode *macro, ZToken *name) {
	ZMacroVar **vars = macro->macro.captured;
	for (usize i = 0; i < veclen(vars); i++) {
		if (tokeneq(vars[i]->name, name)) {
			return vars[i];
		}
	}
	return NULL;
}

static bool matchMacroPattern(ZParser *parser,
																ZNode *macro,
																ZMacroPattern *pattern) {
	ZNode *node = NULL;
	ZType *type = NULL;
	ZMacroVar *macrovar = NULL;
	usize startIdx;

	switch (pattern->kind) {
		case Z_MACRO_EXPR:
			startIdx = parser->source->current;
			node = parseExpr(parser);
			if (!node) {
				return false;
			}
			macrovar = findCapturedVar(macro, pattern->ident);
			if (macrovar) {
				macrovar->startIndex = startIdx;
				macrovar->endIndex = parser->source->current;
				macrovar->captured = node;
			}
			return true;

		case Z_MACRO_IDENT:
			if (!check(parser, TOK_IDENT)) return false;
			startIdx = parser->source->current;
			node = makenode(NODE_IDENTIFIER);
			node->identTok = consume(parser);
			macrovar = findCapturedVar(macro, pattern->ident);
			if (macrovar) {
				macrovar->startIndex = startIdx;
				macrovar->endIndex = parser->source->current;
				macrovar->captured = node;
			}
			return true;

		case Z_MACRO_TYPE:
			startIdx = parser->source->current;
			type = parseType(parser);
			if (!type) return false;
			macrovar = findCapturedVar(macro, pattern->ident);
			if (macrovar) {
				macrovar->startIndex = startIdx;
				macrovar->endIndex = parser->source->current;
				node = makenode(NODE_TYPE);
				node->resolved = type;
				macrovar->captured = node;
			}
			return true;

		case Z_MACRO_KEY:
			// Match literal keyword - must match exactly
			if (!canPeek(parser)) return false;
			if (!tokeneq(peek(parser), pattern->ident)) {
				return false;
			}
			consume(parser);
			return true;

		case Z_MACRO_SEQ:
			for (usize i = 0; i < veclen(pattern->sequence); i++) {
				if (!matchMacroPattern(parser, macro, pattern->sequence[i])) {
					return false;
				}
			}
			return true;

		default:
			return false;
	}
}

ZToken **copytokens(ZToken **source, usize start, usize end) {
	ZToken **copy = NULL;
	for (usize i = start; i < end; i++) {
		vecpush(copy, source[i]);
	}
	return copy;
}

ZNode *expandMacro(ZParser *parser) {
	ZNode **macros = parser->macros;
	if (!macros || veclen(macros) == 0) return NULL;

	for (usize i = 0; i < veclen(macros); i++) {
		// Skip macros that are currently being expanded (avoid clobbering captured vars)
		if (macros[i] == parser->currentMacro) continue;

		usize saved = parser->source->current;
		ZTokenStream *savedStream = parser->source;

		// Reset captured vars for fresh matching
		for (usize j = 0; j < veclen(macros[i]->macro.captured); j++) {
			macros[i]->macro.captured[j]->captured = NULL;
			macros[i]->macro.captured[j]->startIndex = 0;
			macros[i]->macro.captured[j]->endIndex = 0;
		}

		bool valid = matchMacroPattern(parser, macros[i], macros[i]->macro.pattern);
		if (!valid) {
			parser->source = savedStream;
			parser->source->current = saved;
			continue;
		}
		ZNode *macro = macros[i];

		ZToken **bodyTokens = NULL;
		bodyTokens = copytokens(macro->macro.sourceTokens, macro->macro.startBody, macro->macro.endBody);

		if (!bodyTokens) return NULL;

		// Create an isolated token stream for body parsing
		ZTokenStream *bodyStream = maketokstream(bodyTokens, NULL);

		// Save parser state
		ZTokenStream *savedSource = parser->source;
		ZNode *savedCurrentMacro = parser->currentMacro;

		// Switch to body token stream
		parser->source = bodyStream;
		parser->currentMacro = macro;

		// Parse the body as a sequence of statements
		ZNode *block = makenode(NODE_BLOCK);
		block->block = NULL;
		ZNode *stmt = NULL;
		while (parser->source->current < parser->source->end) {
			stmt = parseStmt(parser);
			if (stmt) vecpush(block->block, stmt);
			else break;
		}

		// Restore parser state
		parser->source = savedSource;
		parser->currentMacro = savedCurrentMacro;

		return block;
	}
	return NULL;
}

ZNode *getMacroCapturedVar(ZNode *macro, ZToken *name) {
	ZMacroVar **vars = macro->macro.captured;
	for (usize i = 0; i < veclen(vars); i++) {
		if (strcmp(vars[i]->name->str, name->str) == 0) return vars[i]->captured;
	}
	return NULL;
}
