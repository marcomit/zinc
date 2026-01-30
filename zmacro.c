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
	}
	return copy;
}

static ZNode **copynodevec(ZNode **nodes) {
	if (!nodes) return NULL;
	ZNode **copy = NULL;
	for (usize i = 0; i < veclen(nodes); i++) {
		vecpush(copy, copynode(nodes[i]));
	}
	return copy;
}

ZNode *changenode(ZNode *node, ZNode *copy) {
	if (!node || !copy) return NULL;
	copy->type = node->type;
	copy->tok = node->tok;
	copy->resolved = copytype(node->resolved);

	switch (node->type) {
		case NODE_BLOCK:
			copy->block = copynodevec(node->block);
			break;
		case NODE_IF:
			copy->ifStmt.cond = copynode(node->ifStmt.cond);
			copy->ifStmt.body = copynode(node->ifStmt.body);
			copy->ifStmt.elseBranch = copynode(node->ifStmt.elseBranch);
			break;
		case NODE_WHILE:
			copy->whileStmt.cond = copynode(node->whileStmt.cond);
			copy->whileStmt.branch = copynode(node->whileStmt.branch);
			break;
		case NODE_FOR:
			copy->forStmt.ident = node->forStmt.ident;
			copy->forStmt.iterator = copynode(node->forStmt.iterator);
			copy->forStmt.block = copynode(node->forStmt.block);
			break;
		case NODE_RETURN:
			copy->returnStmt.expr = copynode(node->returnStmt.expr);
			break;
		case NODE_VAR_DECL:
			copy->varDecl.type = copytype(node->varDecl.type);
			copy->varDecl.ident = node->varDecl.ident;
			copy->varDecl.rvalue = copynode(node->varDecl.rvalue);
			break;
		case NODE_BINARY:
			copy->binary.op = node->binary.op;
			copy->binary.left = copynode(node->binary.left);
			copy->binary.right = copynode(node->binary.right);
			break;
		case NODE_UNARY:
			copy->unary.operat = node->unary.operat;
			copy->unary.operand = copynode(node->unary.operand);
			break;
		case NODE_CALL:
			copy->call.callee = copynode(node->call.callee);
			copy->call.args = copynodevec(node->call.args);
			break;
		case NODE_FUNC:
			copy->funcDef.ret = copytype(node->funcDef.ret);
			copy->funcDef.ident = node->funcDef.ident;
			copy->funcDef.args = copynodevec(node->funcDef.args);
			copy->funcDef.body = copynode(node->funcDef.body);
			copy->funcDef.receiver = copynode(node->funcDef.receiver);
			copy->funcDef.generics = node->funcDef.generics; // Shallow copy tokens
			break;
		case NODE_LITERAL:
			copy->literalTok = node->literalTok;
			break;
		case NODE_IDENTIFIER:
			copy->identTok = node->identTok;
			break;
		case NODE_STRUCT:
			copy->structDef.ident = node->structDef.ident;
			copy->structDef.fields = copynodevec(node->structDef.fields);
			copy->structDef.generics = node->structDef.generics;
			break;
		case NODE_SUBSCRIPT:
			copy->subscript.arr = copynode(node->subscript.arr);
			copy->subscript.index = copynode(node->subscript.index);
			break;
		case NODE_MEMBER:
			copy->memberAccess.object = copynode(node->memberAccess.object);
			copy->memberAccess.field = node->memberAccess.field;
			break;
		case NODE_MODULE:
			copy->module.name = node->module.name;
			copy->module.root = copynode(node->module.root);
			break;
		case NODE_PROGRAM:
			copy->program = copynodevec(node->program);
			break;
		case NODE_UNION:
			copy->unionDef.ident = node->unionDef.ident;
			copy->unionDef.fields = copynodevec(node->unionDef.fields);
			break;
		case NODE_FIELD:
			copy->field.type = copytype(node->field.type);
			copy->field.identifier = node->field.identifier;
			break;
		case NODE_TYPEDEF:
			copy->typeDef.alias = node->typeDef.alias;
			copy->typeDef.type = copytype(node->typeDef.type);
			break;
		case NODE_FOREIGN:
			copy->foreignFunc.ret = copytype(node->foreignFunc.ret);
			copy->foreignFunc.tok = node->foreignFunc.tok;
			copy->foreignFunc.args = copytypevec(node->foreignFunc.args);
			break;
		case NODE_DEFER:
			copy->deferStmt.expr = copynode(node->deferStmt.expr);
			break;
		case NODE_STRUCT_LIT:
			copy->structlit.ident = node->structlit.ident;
			copy->structlit.fields = copynodevec(node->structlit.fields);
			copy->structlit.generics = copytypevec(node->structlit.generics);
			break;
		case NODE_TUPLE_LIT:
			copy->tuplelit.fields = copynodevec(node->tuplelit.fields);
			break;
		case NODE_ARRAY_LIT:
			copy->arraylit.fields = copynodevec(node->arraylit.fields);
			break;
		case NODE_MACRO:
			// Macros typically shouldn't be copied during expansion
			copy->macro.pattern = node->macro.pattern;
			copy->macro.block = copynode(node->macro.block);
			copy->macro.captured = node->macro.captured;
			copy->macro.consumed = node->macro.consumed;
			break;
		case NODE_GOTO:
		case NODE_LABEL:
			copy->gotoLabel = node->gotoLabel;
			break;
		case NODE_TYPE:
			// resolved is already copied above
			break;
	}

	return copy;
}

ZNode *copynode(ZNode *node) {
	if (!node) return NULL;
	ZNode *copy = makenode(node->type);
	return changenode(node, copy);
}

static bool macropatterneq(ZMacroPattern *p1, ZMacroPattern *p2) {
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

static bool macroeq(ZNode *m1, ZNode *m2) {
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

	switch (pattern->kind) {
		case Z_MACRO_EXPR:
			printf("Trying match expression\n");
			node = parseExpr(parser);
			if (!node) {
				printf("failed to parse expression\n");
				return false;
			}
			macrovar = findCapturedVar(macro, pattern->ident);
			if (macrovar && macrovar->captured) {
				changenode(node, macrovar->captured);
			}
			return true;

		case Z_MACRO_IDENT:
			if (!check(parser, TOK_IDENT)) return false;
			node = makenode(NODE_IDENTIFIER);
			node->tok = consume(parser);
			macrovar = findCapturedVar(macro, pattern->ident);
			if (macrovar && macrovar->captured) {
				changenode(node, macrovar->captured);
			}
			return true;

		case Z_MACRO_TYPE:
			type = parseType(parser);
			if (!type) return false;
			node = makenode(NODE_TYPE);
			node->resolved = type;
			macrovar = findCapturedVar(macro, pattern->ident);
			if (macrovar && macrovar->captured) {
				changenode(node, macrovar->captured);
			}
			return true;

		case Z_MACRO_KEY:
			// Match literal keyword - must match exactly
			if (!canPeek(parser)) return false;
			if (!tokeneq(peek(parser), pattern->ident)) {
				printf("Key mismatch: expected %s, got %s\n",
					stoken(pattern->ident), stoken(peek(parser)));
				return false;
			}
			consume(parser);
			return true;

		case Z_MACRO_SEQ:
			printf("Trying match sequence\n");
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

ZNode *expandMacro(ZParser *parser) {
	ZNode **macros = parser->macros;
	if (!macros || veclen(macros) == 0) return NULL;

	printf("expand macro %zu %s\n", veclen(parser->macros), stoken(peek(parser)));
	ZNode *expanded = NULL;

	for (usize i = 0; i < veclen(parser->macros); i++) {
		usize saved = parser->current;
		bool valid = matchMacroPattern(parser, macros[i], macros[i]->macro.pattern);
		if (!valid) {
			parser->current = saved;
			printf("Invalid macro\n");
			continue;
		}

		if (expanded) {
			error(parser->state, peek(parser), "Two macros follows the same pattern");
		}
		ZNode *copiedBody = copynode(macros[i]->macro.block);
		expanded = copiedBody;
	}
	if (expanded) {
		printf("Macro expanded\n");
	}
	return expanded;
}

ZNode *getMacroCapturedVar(ZNode *macro, ZToken *name) {
	ZMacroVar **vars = macro->macro.captured;
	for (usize i = 0; i < veclen(vars); i++) {
		if (strcmp(vars[i]->name->str, name->str) == 0) return vars[i]->captured;
	}
	return NULL;
}
