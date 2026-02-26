#include "zinc.h"
#include "zvec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define ensure(c) if (!(c)) return NULL
#define expect(l, t) if (!match(l, t)) {																									\
		error((l)->state, peek(l), "Expected %s, got %s", tokname(t), stoken(peek(parser)));	\
		return NULL;																																					\
	}

#define invalid(...) {																																		\
		error(parser->state, peek(parser), __VA_ARGS__);																			\
		return NULL;																																					\
}

typedef ZNode *(*ParseFunction)(ZParser *);

ZType *parseType												(ZParser *);
ZNode	*parseExpr												(ZParser *);
static ZNode *parse											(ZParser *);
static ZNode *parseIf										(ZParser *);
static ZNode *parseFor									(ZParser *);
static ZNode *parseGoto									(ZParser *);
static ZNode *skipMacro									(ZParser *);
static ZNode *parseWhile								(ZParser *);
static ZNode *parseBreak								(ZParser *);
static ZNode *parseBlock								(ZParser *);
static ZNode *parseMatch								(ZParser *);
static ZNode *parseDefer								(ZParser *);
static ZNode *parseLabel								(ZParser *);
static ZNode *parseReturn								(ZParser *);
static ZNode *parseVarDef								(ZParser *);
static ZNode *parseBinary								(ZParser *);
static ZNode *parseImport								(ZParser *);
static ZNode *parseTypedef							(ZParser *);
static ZNode *parseVarDecl							(ZParser *);
static ZNode *parseContinue							(ZParser *);
static ZNode *parseArrayLit							(ZParser *);
static ZNode *parseTupleLit							(ZParser *);
static ZNode *parseFuncDecl							(ZParser *);
static ZNode *parseEnumDecl							(ZParser *);
static ZNode *parseUnionDecl						(ZParser *);
static ZNode *parseStructLit						(ZParser *);
static ZNode *parseStructDecl						(ZParser *);
static ZNode *parseForeignDecl					(ZParser *);
static ZToken **parseGenericsDecl				(ZParser *);
static ZMacroPattern *parseMacroPattern	(ZParser *, ZNode *);

static ParseFunction stmtFunc[] = {
	parseBlock,
	parseIf,
	parseFor,
	parseGoto,
	parseWhile,
	parseMatch,
	parseDefer,
	parseLabel,
	parseReturn,
	expandMacro,
	parseVarDef,
	parseExpr,
	parseVarDecl,
	parseBreak,
	parseContinue
};

static ParseFunction exprFunc[] = {
	// parseBlock, // Block parsed as expression for now.
	parseBinary,
	parseStructLit,
	parseArrayLit,
	parseTupleLit,
};

static ParseFunction progFunc[] = {
	parseImport,
	parseForeignDecl,
	parseTypedef,
	parseFuncDecl,
	parseStructDecl,
	parseEnumDecl,
	parseUnionDecl,
	expandMacro,
	parseVarDef,
	skipMacro
};

static ZParser *makeparser(ZState *state, ZToken **tokens) {
	ZParser *self = zalloc(ZParser);
	self->source = maketokstream(tokens, NULL);
	self->errstack = NULL;
	self->depth = 0;
	self->state = state;
	self->macros = NULL;
	self->currentMacro = NULL;
	self->expandingMacros = NULL;
	return self;
}

ZNode *makenode(ZNodeType type) {
	ZNode *self = zalloc(ZNode);
	self->type = type;
	return self;
}

ZType *maketype(ZTypeKind kind) {
	ZType *self = zalloc(ZType);
	self->kind = kind;
	return self;
}

static ZNode *makenodevar(ZNode *ident, ZType *type, ZNode *expr) {
	ZNode *node = makenode(NODE_VAR_DECL);
	node->tok = ident->identTok;
	node->varDecl.ident = ident;
	node->varDecl.type = type;
	node->varDecl.rvalue = expr;
	return node;
}

bool canPeek(ZParser *p) {
	while (p->source->current >= p->source->end && p->source->prev) {
		p->source = p->source->prev;
	}
	return p->source->current < p->source->end;
}

ZToken *peek(ZParser *parser) {
	while (parser->source->current >= parser->source->end && parser->source->prev) {
		parser->source = parser->source->prev;
	}
	if (!canPeek(parser)) return NULL;

	return parser->source->list[parser->source->current];
}

ZToken *consume(ZParser *parser) {
	ensure(canPeek(parser));

	ZToken *curr = peek(parser);
	
	parser->source->current++;
	return curr;
}

bool check(ZParser *parser, ZTokenType type) {
	return canPeek(parser) && peek(parser)->type == type;
}

bool checkMask(ZParser *tokens, u32 mask) {
	return canPeek(tokens) && peek(tokens)->type & mask;
}

bool match(ZParser *parser, ZTokenType type) {
	bool res = check(parser, type);
	if (res) consume(parser);
	return res;
}

static void pushErrorCheckpoint(ZParser *parser) {
	usize errlen = veclen(parser->state->errors);
	vecpush(parser->errstack, errlen);
	parser->depth++;
}

static void commitErrors(ZParser *parser) {
	if (parser->depth > 0) {
		vecpop(parser->errstack);
		parser->depth--;
	}
}

static void rollbackErrors(ZParser *parser) {
	if (parser->depth > 0) {
		usize checkpoint = vecpop(parser->errstack);
		while (veclen(parser->state->errors) > checkpoint) vecpop(parser->state->errors);
		parser->depth--;
	}
}

static bool isValidToken(ZParser *parser, ZTokenType *validTokens, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (check(parser, validTokens[i])) return true;
	}
	return false;
}

/* Since this function tries to parse macros for now i save the current source of tokens
 * such that after the execution of the function i can undo the modification. */
static ZNode *wrapNode(ZParser *parser, ParseFunction parse) {
	ZTokenStream *savedStream = parser->source;
	usize savedIndex = savedStream->current;

	pushErrorCheckpoint(parser);
	ZNode *res = parse(parser);

	if (!res) {
		parser->source = savedStream;
		parser->source->current = savedIndex;
		rollbackErrors(parser);
	} else {
		commitErrors(parser);
	}
	return res;
}

static ZType *wrapType(ZParser *parser, ZType *(*parse)(ZParser *)) {
	ZTokenStream *savedStream = parser->source;
	usize savedIndex = savedStream->current;

	pushErrorCheckpoint(parser);
	ZType *res = parse(parser);

	if (!res) {
		parser->source = savedStream;
		parser->source->current = savedIndex;
		rollbackErrors(parser);
	} else {
		commitErrors(parser);
	}
	return res;
}

static ZNode *parseOrGrammar(ZParser *parser, ParseFunction *pf, usize len) {
	for (usize i = 0; i < len; i++) {
		ZNode *parsed = wrapNode(parser, pf[i]);
		if (parsed) return parsed;
	}
	return NULL;
}

static ZNode *parseGenericBinary(ZParser *parser,
																ParseFunction parseLeft,
																ParseFunction parseRight,
																ZTokenType *validTokens,
																size_t validTokensLen) {
	ZNode *node = NULL;

	ZNode *left = wrapNode(parser, parseLeft);

	ensure(left);

	while (canPeek(parser) &&
					isValidToken(parser, validTokens, validTokensLen) &&
					!peek(parser)->newlineBefore) {
		node = makenode(NODE_BINARY);
		ZToken *op = consume(parser);

		ZNode *right = wrapNode(parser, parseRight);

		if (!right) {
			node = NULL;
			break;
		}

		node->binary.op = op;
		node->binary.left = left;
		node->binary.right = right;
		node->tok = op;
		left = node;
	}

	return node ? node : left;
}

static ZNode *parsePrimary(ZParser *parser) {
	ensure(canPeek(parser));

	if (parser->currentMacro && match(parser, TOK_MACRO_IDENT)) {
		if (!check(parser, TOK_IDENT)) {
			error(parser->state, peek(parser), "Expected an identifier after @");
			return NULL;
		}
		ZToken *tok = consume(parser);
		return getMacroCapturedVar(parser->currentMacro, tok);
	} else if (match(parser, TOK_LPAREN)) {
		ZNode *node 			= wrapNode(parser, parseExpr);
		expect(parser, TOK_RPAREN);
		return node;
	} else if (check(parser, TOK_IDENT)) {
		ZNode *node 			= makenode(NODE_IDENTIFIER);
		node->identTok 		= consume(parser);
		node->tok 				= node->identTok;
		return node;
	} else if (checkMask(parser, TOK_LITERAL)) {
		ZNode *node 			= makenode(NODE_LITERAL);
		node->literalTok 	= consume(parser);
		node->tok					= node->literalTok;
		return node;
	} else if (check(parser, TOK_NONE)) {
		ZNode *node 			= makenode(NODE_LITERAL);
		node->literalTok 	= consume(parser);
		node->tok 				= node->literalTok;
		return node;
	}

	return NULL;
}

static ZNode *parseArrSubscript(ZParser *parser, ZNode *previous) {
	expect(parser, TOK_LSBRACKET);
	ZNode *index = wrapNode(parser, parseExpr);
	expect(parser, TOK_RSBRACKET);

	ZNode *node = makenode(NODE_SUBSCRIPT);
	node->subscript.index = index;
	node->subscript.arr = previous;
	return node;
}

static ZNode **parseArgs(ZParser *parser) {
	ZNode **args = NULL;
	
	ZNode *expr = wrapNode(parser, parseExpr);
	if (!expr) return args;
	
	vecpush(args, expr);
	
	while (match(parser, TOK_COMMA)) {
		expr = wrapNode(parser, parseExpr);
		if (!expr) break;
		vecpush(args, expr);
	}
	
	return args;
}

static ZNode *parseMemberAccess(ZParser *parser, ZNode *previous) {
	expect(parser, TOK_DOT);

	ZToken *member = consume(parser);
	ZNode *node = makenode(NODE_MEMBER);

	node->memberAccess.field = member;
	node->memberAccess.object = previous;
	return node;
}

static ZNode *parseFuncCall(ZParser *parser, ZNode *previous) {
	expect(parser, TOK_LPAREN);
	ZNode **args = parseArgs(parser);
	expect(parser, TOK_RPAREN);

	ZNode *node = makenode(NODE_CALL);
	node->call.args = args;
	node->call.callee = previous;
	node->tok = previous->tok;
	return node;
}

static ZNode *parsePostfixOper(ZParser *parser, ZNode *previous) {
	usize saved = parser->source->current;
	pushErrorCheckpoint(parser);
	ZNode *res = NULL;
	if (check(parser, TOK_LSBRACKET)) {
		res = parseArrSubscript(parser, previous);
	} else if (check(parser, TOK_LPAREN)) {
		res = parseFuncCall(parser, previous);
	} else {
		res = parseMemberAccess(parser, previous);
	}

	if (res) {
		commitErrors(parser);
	} else {
		rollbackErrors(parser);
		parser->source->current = saved;
	}
	return res;
}

static ZNode *parsePostfixExpr(ZParser *parser) {
	ZNode *left = wrapNode(parser, parsePrimary);

	ensure(left);

	ZNode *node = NULL;
	do {

		node = parsePostfixOper(parser, left);
		if (!node) break;

		left = node;
	} while (node);
	return left;
}

static ZNode *parseUnary(ZParser *parser) {
	ensure(canPeek(parser));

	ZNode *node = wrapNode(parser, parsePostfixExpr);

	if (node) return node;

	ZTokenType valids[] = {
		TOK_PLUS, TOK_MINUS, TOK_NOT,
		TOK_SNOT, TOK_STAR, TOK_REF
	};

	usize len = sizeof(valids) / sizeof(valids[0]);

	if (!isValidToken(parser, valids, len)) {
		error(parser->state, peek(parser), "Expected expression, got '%s'", stoken(peek(parser)));
		return NULL;
	}

	node = makenode(NODE_UNARY);
	node->unary.operat = consume(parser);
	node->unary.operand = wrapNode(parser, parseUnary);

	ensure(node->unary.operand);

	return node;
}

#define arrlen(arr) (sizeof(arr) / sizeof((arr)[0]))
static ZNode *parseFactor(ZParser *parser) {
	ZTokenType valids[] = {TOK_STAR, TOK_DIV};
	return parseGenericBinary(parser, parseUnary, parseUnary, valids, arrlen(valids));
}

static ZNode *parseTerm(ZParser *parser) {
	ZTokenType valids[] = {TOK_PLUS, TOK_MINUS};
	return parseGenericBinary(parser, parseFactor, parseFactor, valids, arrlen(valids));
}

static ZNode *parseComparison(ZParser *parser) {
	ZTokenType valids[] = {TOK_LT, TOK_GT, TOK_LTE, TOK_GTE};
	return parseGenericBinary(parser, parseTerm, parseTerm, valids, arrlen(valids));
}

static ZNode *parseEquality(ZParser *parser) {
	ZTokenType valids[] = {TOK_EQEQ, TOK_NOTEQ};
	return parseGenericBinary(parser, parseComparison, parseComparison, valids, arrlen(valids));
}

static ZNode *parseLogicalAnd(ZParser *parser) {
	ZTokenType valids[] = {TOK_AND, TOK_SAND};
	return parseGenericBinary(parser, parseEquality, parseEquality, valids, arrlen(valids));
}

static ZNode *parseLogicalOr(ZParser *parser) {
	ZTokenType valids[] = {TOK_OR, TOK_SOR};
	return parseGenericBinary(parser, parseLogicalAnd, parseLogicalAnd, valids, arrlen(valids));
}

static ZNode *parseAssignment(ZParser *parser) {
	ZTokenType valids[] = {TOK_EQ};
	return parseGenericBinary(parser, parseLogicalOr, parseAssignment, valids, arrlen(valids));
}

static ZNode *parseBinary(ZParser *parser) {
	return parseAssignment(parser);
}

/* Parses a list of types delimited by left and right.
 * It assumes that the elements are separated by a TOK_COMMA.
 *
 * Example: [u8, char, [u8]]
 * You can parse this by using this function
 * by calling with TOK_LSBRACKET and TOK_RSBRACKET.
 * */
static ZType **parseTypeList(ZParser *parser, ZTokenType left, ZTokenType right) {
	expect(parser, left);
	ZType **args = NULL;
	do {
		ZType *curr = wrapType(parser, parseType);
		if (!curr) {
			error(parser->state, peek(parser), "Expected a type in type list");
			return args;
		}
		vecpush(args, curr);
		if (!match(parser, TOK_COMMA)) break;
		if (check(parser, right)) break;
	} while (true);
	if (!match(parser, right)) {
		error(parser->state, peek(parser), "Expected '%s' to close type list, got '%s'", tokname(right), stoken(peek(parser)));
		return NULL;
	}
	return args;
}

static ZType *applyStarsToType(ZType *base, u8 stars) {
	for (u8 i = 0; i < stars; i++) {
		ZType *node = maketype(Z_TYPE_POINTER);
		node->base = base;
		base = node;
	}
	return base;
}

static ZType *parseTypeArray(ZParser *parser) {
	expect(parser, TOK_LSBRACKET);
	ZType *type = wrapType(parser, parseType);
	ensure(type);
	usize size = 0;
	
	if (match(parser, TOK_SEMICOLON)) {
		if (!check(parser, TOK_INT_LIT)) invalid("Expected the array size");
		size = consume(parser)->integer;
	}

	expect(parser, TOK_RSBRACKET);

	ZType *arr = maketype(Z_TYPE_ARRAY);
	arr->array.base = type;
	arr->array.size = size;
	return arr;
}

/* A tuple must have at least 2 types */
static ZType *parseTypeTuple(ZParser *parser) {
	ZType **types = parseTypeList(parser, TOK_LPAREN, TOK_RPAREN);

	if (!types) {
		error(parser->state, peek(parser), "Expected types in tuple type");
		return NULL;
	} else if (veclen(types) < 2) {
		error(parser->state, peek(parser), "Tuple type requires at least 2 elements, got %zu", veclen(types));
		return NULL;
	}

	ZType *type = maketype(Z_TYPE_TUPLE);
	type->tuple = types;
	return type;
}

static ZType *parseTypeFunc(ZParser *parser, ZType *previous) {
	ZType **args = parseTypeList(parser, TOK_LPAREN, TOK_RPAREN);
	ZType **generics = NULL;

	if (check(parser, TOK_LSBRACKET)) {
		generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
		if (!generics) {
			error(parser->state, peek(parser), "Expected generic type parameters in '[...]'");
			return NULL;
		}
	}

	ZType *type = maketype(Z_TYPE_FUNCTION);
	type->func.ret = previous;
	type->func.args = args;
	type->func.generics = generics;
	return type;
}

static ZType *parseAtom(ZParser *parser) {
	if (check(parser, TOK_LSBRACKET)) {
		return parseTypeArray(parser);
	} else if (check(parser, TOK_LPAREN)) {
		return parseTypeTuple(parser);
	}

	if (checkMask(parser, TOK_TYPES_MASK) || check(parser, TOK_IDENT)) {
		ZType *base = maketype(Z_TYPE_PRIMITIVE);
		base->primitive.token = consume(parser);
		base->primitive.base 	= NULL;
		return base;
	}
	return NULL;
}

ZType *parseType(ZParser *parser) {
	bool constant = match(parser, TOK_CONST);

	u8 stars = 0;
	while (match(parser, TOK_STAR)) stars++;

	ZType *base = parseAtom(parser);
	ensure(base);
	base = applyStarsToType(base, stars);

	base->constant = constant;

	if (check(parser, TOK_LPAREN)) {
		return parseTypeFunc(parser, base);
	} else if (check(parser, TOK_LSBRACKET)) {
		// Generic type instantiation like List[int] or Map[str, int]
		ZType **generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
		ZType *type = maketype(Z_TYPE_GENERIC);
		type->generic.name = base->primitive.token;
		type->generic.args = generics;
		return type;
	}
	return base;
}

static ZNode *parseDefer(ZParser *parser) {
	expect(parser, TOK_DEFER);
	ZNode *expr = parseExpr(parser);

	if (!expr) invalid("Expected an expression after 'defer' keyword")

	ZNode *node = makenode(NODE_DEFER);
	node->deferStmt.expr = expr;
	return node;
}

static ZNode *parseMatch(ZParser *parser) {
	expect(parser, TOK_MATCH);

	ZNode *expr = wrapNode(parser, parseExpr);

	ensure(expr);

	expect(parser, TOK_LBRACKET);
	expect(parser, TOK_RBRACKET);

	return NULL;
}

static ZNode *parseGoto(ZParser *parser) {
	expect(parser, TOK_GOTO);
	ensure(check(parser, TOK_IDENT));
	ZNode *node = makenode(NODE_GOTO);
	node->tok = consume(parser);
	return node;
}

static ZNode *parseLabel(ZParser *parser) {
	ensure(check(parser, TOK_IDENT));
	ZToken *tok = consume(parser);
	expect(parser, TOK_COLON);
	ZNode *node = makenode(NODE_LABEL);
	node->tok = tok;
	return node;
}

ZNode *parseStmt(ZParser *parser) {
	usize len = sizeof(stmtFunc) / sizeof(stmtFunc[0]);
	return parseOrGrammar(parser, stmtFunc, len);
}

static ZNode *parseBlock(ZParser *parser) {
	let start = peek(parser);
	expect(parser, TOK_LBRACKET);

	ZNode *block = makenode(NODE_BLOCK);
	ZNode *stmt = NULL;
	do {
		stmt = parseStmt(parser);
		if (stmt) vecpush(block->block, stmt);
	} while (stmt);

	if (!check(parser, TOK_RBRACKET)) {
		error(parser->state, peek(parser), "A statement cannot be parsed");
		while (canPeek(parser) && !check(parser, TOK_RBRACKET)) consume(parser);
		if (!canPeek(parser)) invalid("Expected a '}' to close the block")
	}

	expect(parser, TOK_RBRACKET);

	if (veclen(block->block) == 0) {
		warning(parser->state, start, "A block cannot be empty");
	}

	return block;
}

static ZNode *parseField(ZParser *parser) {
	ZType *type = wrapType(parser, parseType);

	if (!type) {
		return NULL;
	}
	ensure(check(parser, TOK_IDENT));

	ZToken *ident = consume(parser);

	ZNode *node = makenode(NODE_FIELD);
	node->field.type = type;
	node->field.identifier = ident;
	return node;
}

// TODO: To implement
static ZNode *parseEnumDecl(ZParser *parser) {
	// bool isPublic = match(parser, TOK_PUB);
	//
	expect(parser, TOK_ENUM);
	// ensure(check(parser, TOK_IDENT));
	// let name = consume(parser);
	//
	// let node = makenode(NODE_ENUM);
	//
	return NULL;
}

static ZNode *parseUnionDecl(ZParser *parser) {
	expect(parser, TOK_UNION);
	ensure(check(parser, TOK_IDENT));

	let name = consume(parser);

	expect(parser, TOK_LBRACKET);

	ZNode **fields = NULL;
	ZNode *field = NULL;
	while (( field = parseField(parser) )) vecpush(fields, field);

	expect(parser, TOK_RBRACKET);
	
	ZNode *node = makenode(NODE_UNION);

	node->unionDef.fields = fields;
	node->unionDef.ident = name;

	return node;
}

static ZNode *parseStructDecl(ZParser *parser) {
	bool public = match(parser, TOK_PUB);
	expect(parser, TOK_STRUCT);
	ensure(check(parser, TOK_IDENT));

	ZToken *name = consume(parser);
	ZToken **generics = NULL;

	if (check(parser, TOK_LSBRACKET)) {
		generics = parseGenericsDecl(parser);
		if (!generics) {
			error(parser->state, peek(parser), "Expected generic parameters after struct name");
		}
	}

	expect(parser, TOK_LBRACKET);

	ZNode **fields = NULL;
	ZNode 	*field = NULL;

	while (( field = parseField(parser) )) {
		vecpush(fields, field);
	}

	expect(parser, TOK_RBRACKET);

	ZNode *node = makenode(NODE_STRUCT);
	node->structDef.fields = fields;
	node->structDef.generics = generics;
	node->structDef.ident = name;
	node->structDef.pub = public;
	return node;
}

ZNode *parseExpr(ZParser *parser) {
	if (!parser->currentMacro || !match(parser, TOK_MACRO_EXPR)) {
		return parseOrGrammar(parser, exprFunc, arrlen(exprFunc));
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser->state, peek(parser), "Expected an identifier");
		return NULL;
	}

	ZToken *var = consume(parser);
	ZNode *placeholder = getMacroCapturedVar(parser->currentMacro, var);
	if (!placeholder) {
		error(parser->state, peek(parser), "%s is not a valid macro variable", var->str);
		return NULL;
	}
	return placeholder;
}

static ZNode *parseReturn(ZParser *parser) {
	ZToken *start = peek(parser);
	ensure(canPeek(parser) && match(parser, TOK_RETURN));
	ZNode *ret = makenode(NODE_RETURN);

	ret->returnStmt.expr = parseExpr(parser);
	ret->tok = start;
	return ret;
}

static ZNode *parseIf(ZParser *parser) {
	ZToken *start = peek(parser);
	expect(parser, TOK_IF);
	
	ZNode *cond = wrapNode(parser, parseExpr);

	ParseFunction elseBranch[] = {
		parseIf, parseBlock
	};

	ZNode *body = wrapNode(parser, parseBlock);
	if (!body) return NULL;

	ZNode *node = makenode(NODE_IF);

	if (canPeek(parser) && match(parser, TOK_ELSE)) {
		ZNode *elseBody = parseOrGrammar(parser, elseBranch, 2);
		if (!elseBody) return NULL;
		node->ifStmt.elseBranch = elseBody;
	}

	node->ifStmt.cond = cond;
	node->ifStmt.body = body;
	node->tok 				= start;
	return node;
}

static ZNode *parseFor(ZParser *parser) {
	ZToken *start = peek(parser);
	expect(parser, TOK_FOR);

	ZNode *var = parseVarDef(parser);

	if (!var) invalid("Expected a variable declaration")

	expect(parser, TOK_SEMICOLON);

	ZNode *cond = parseExpr(parser);

	if (!cond) invalid("Expected an expression")

	expect(parser, TOK_SEMICOLON);

	ZNode *incr = parseExpr(parser);

	if (!incr) invalid("Expected an expression")

	ZNode *block = parseBlock(parser);

	if (!block) invalid("Expected a block")

	ZNode *node = makenode(NODE_FOR);
	node->forStmt.var 	= var;
	node->forStmt.cond 	= cond;
	node->forStmt.incr 	= incr;
	node->forStmt.block = block;
	node->tok 					= start;
	return node;
}

/* While parsed with 'for' token instead of standard while. */
static ZNode *parseWhile(ZParser *parser) {
	ZToken *start = peek(parser);
	expect(parser, TOK_FOR);

	ZNode *cond = wrapNode(parser, parseExpr);
	if (!cond) {
		error(parser->state, peek(parser), "Expected condition expression after 'while'");
	}
	ZNode *body = wrapNode(parser, parseBlock);

	ZNode *node = makenode(NODE_WHILE);
	node->whileStmt.branch 	= body;
	node->whileStmt.cond 		= cond;
	node->tok								= start;
	return node;
}

static ZToken **parseGenericsDecl(ZParser *parser) {
	ZToken **generics = NULL;
	
	expect(parser, TOK_LSBRACKET);

	while (!check(parser, TOK_RSBRACKET)) {
		ensure(check(parser, TOK_IDENT));
		vecpush(generics, consume(parser));
		if (!match(parser, TOK_COMMA)) break;
	}

	expect(parser, TOK_RSBRACKET);

	return generics;
}

static ZNode *parseFuncDecl(ZParser *parser) {
	bool public = match(parser, TOK_PUB);
	ZType *ret = wrapType(parser, parseType);

	if (!ret || !canPeek(parser) || !check(parser, TOK_IDENT)) return NULL;

	ZToken *name = consume(parser);
	ZToken **generics = NULL;

	if (check(parser, TOK_LSBRACKET)) {
		generics = parseGenericsDecl(parser);
		if (!generics) {
			error(parser->state, peek(parser), "Expected generic type parameters after function name");
		}
	}

	ensure(match(parser, TOK_LPAREN));

	ZNode **args = NULL;

	while (!match(parser, TOK_RPAREN)) {
		ZType *argType = wrapType(parser, parseType);
		if (!argType) {
			return NULL;
		}
		ZToken *argName = consume(parser);
	
		if (!argName) {
			error(parser->state, peek(parser), "Expected parameter name after type");
			return NULL;
		}

		ZNode *arg = makenode(NODE_FIELD);
		arg->field.type = argType;
		arg->field.identifier = argName;
		vecpush(args, arg);

		if (match(parser, TOK_RPAREN)) break;

		if (!match(parser, TOK_COMMA)) {
			return NULL;
		}
	}

	ZNode *receiver = NULL;
	if (!check(parser, TOK_LBRACKET)) {
		receiver = wrapNode(parser, parseField);
		if (!receiver) {
			error(parser->state, peek(parser), "Expected receiver declaration or function body");
		}
	}

	ZNode *body = wrapNode(parser, parseBlock);

	if (!body) {
		error(parser->state, peek(parser), "Expected function body '{...}' after declaration");
	}

	ensure(body);

	ZNode *node = makenode(NODE_FUNC);
	node->funcDef.ret = ret;
	node->funcDef.ident = name;
	node->funcDef.args = args;
	node->funcDef.body = body;
	node->funcDef.pub = public;
	node->funcDef.receiver = receiver;
	node->funcDef.generics = generics;

	ZType *func = maketype(Z_TYPE_FUNCTION);
	func->func.ret = ret;
	func->func.args = NULL;

	for (usize i = 0; i < veclen(args); i++) {
		vecpush(func->func.args, args[i]->field.type);
	}

	node->resolved = func;
	return node;
}

static ZNode *parseVarInferred(ZParser *parser) {
	ZNode *identNode = NULL;
	
	if (parser->currentMacro && match(parser, TOK_MACRO_IDENT)) {
		identNode = getMacroCapturedVar(parser->currentMacro, consume(parser));
		if (!identNode) {
			error(parser->state, peek(parser), "Unknown var");	
		}
	} else {
		ZToken *ident = consume(parser);
		identNode = makenode(NODE_IDENTIFIER);
		identNode->identTok = ident;
	}

	expect(parser, TOK_ASSIGN);
	ZNode *expr = wrapNode(parser, parseExpr);

	if (!expr) {
		error(parser->state, peek(parser), "Expected expression after ':='");
	}

	return makenodevar(identNode, NULL, expr);
}

static ZNode *parseVarDefTyped(ZParser *parser) {
	ZType *type = wrapType(parser, parseType);

	ensure(type && check(parser, TOK_IDENT) && !peek(parser)->newlineBefore);

	ZToken *ident = consume(parser);
	ZNode *node = makenode(NODE_IDENTIFIER);
	node->identTok = ident;
	ZNode *expr = NULL;

	if (match(parser, TOK_EQ)) {
		expr = wrapNode(parser, parseExpr);
		if (!expr) {
			printf("Expression not parsed\n");
			error(parser->state, peek(parser), "Expected expression after '='");
			return NULL;
		}
	}

	return makenodevar(node, type, expr);
}

static ZNode *parseVarDef(ZParser *parser) {
	ParseFunction func[] = {
		parseVarInferred, parseVarDefTyped
	};
	return parseOrGrammar(parser, func, 2);
}

static ZNode *parseBreak(ZParser *parser) {
	ensure(check(parser, TOK_BREAK));

	ZNode *node = makenode(NODE_BREAK);
	node->tok = consume(parser);
	return node;
}

static ZNode *parseContinue(ZParser *parser) {
	ensure(check(parser, TOK_CONTINUE));

	ZNode *node = makenode(NODE_CONTINUE);
	node->tok = consume(parser);

	return node;
}

static ZNode *parseVarDecl(ZParser *parser) {
	ZNode *identNode = NULL;
	ZType *type = wrapType(parser, parseType);

	ensure(type && check(parser, TOK_IDENT) && !peek(parser)->newlineBefore);

	ZToken *ident = consume(parser);
	identNode = makenode(NODE_IDENTIFIER);
	identNode->identTok = ident;

	expect(parser, TOK_EQ);

	ZNode *rvalue = wrapNode(parser, parseExpr);

	return makenodevar(identNode, type, rvalue);
}

static ZNode *parseTupleLit(ZParser *parser) {
	ZToken *start = peek(parser);

	expect(parser, TOK_LPAREN);
	ZNode *expr = NULL;
	ZNode **fields = NULL;
	while (( expr = wrapNode(parser, parseExpr) )) {
		vecpush(fields, expr);
		if (check(parser, TOK_RPAREN)) break;
		if (!match(parser, TOK_COMMA)) break;
	}
	expect(parser, TOK_RPAREN);

	ZNode *node = makenode(NODE_TUPLE_LIT);
	node->tuplelit.fields = fields;

	if (veclen(fields) < 2) {
		error(parser->state, start, "Expected at least 2 itesm");
	}

	return node;
}

static ZNode *parseArrayLit(ZParser *parser) {
	expect(parser, TOK_LSBRACKET);

	ZNode **values = NULL;
	ZNode *expr = NULL;

	while (( expr = wrapNode(parser, parseExpr) )) {
		vecpush(values, expr);
		if (check(parser, TOK_RSBRACKET)) break;
		if (!match(parser, TOK_COMMA)) break;
	}

	expect(parser, TOK_RSBRACKET);

	ZNode *node = makenode(NODE_ARRAY_LIT);

	node->arraylit.fields = values;

	return node;
}

static ZNode *parseStructLit(ZParser *parser) {
	if (!check(parser, TOK_IDENT)) {
		return NULL;
	}
	ZToken *ident = consume(parser);
	ZType **generics = NULL;

	if (check(parser, TOK_LSBRACKET)) {
		generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
		if (!generics) {
			error(parser->state, peek(parser), "Expected generic type arguments in '[...]'");
			return NULL;
		}
	}

	expect(parser, TOK_LBRACKET);

	ZNode *structlit = makenode(NODE_STRUCT_LIT);
	structlit->structlit.ident = ident;
	structlit->structlit.generics = generics;

	while (true) {
		if (!check(parser, TOK_IDENT)) break;
		ZNode *node = makenode(NODE_IDENTIFIER);
		node->identTok = consume(parser);
		if (!match(parser, TOK_COLON)) {
			error(parser->state, peek(parser), "Expected a ':', got %s", stoken(peek(parser)));
			return NULL;
		}
		ZNode *expr = wrapNode(parser, parseExpr);
		if (!expr) return NULL;
		ZNode *var = makenodevar(node, NULL, expr);
		vecpush(structlit->structlit.fields, var);
		if (check(parser, TOK_RBRACKET)) break;
		if (!match(parser, TOK_COMMA)) {
			error(parser->state, peek(parser), "Expected a ',', got %s", stoken(peek(parser)));
			return NULL;
		}
	}

	expect(parser, TOK_RBRACKET);

	return structlit;
}

static ZNode *getModuleByName(ZParser *parser, ZToken *name) {
	usize len = strlen(name->str);
	char *filename = znalloc(char, len + 4);
	// memcpy(filename, name->str);
	memcpy(filename, name->str, len);
	filename[len] = '.';
	filename[len+1] = 'z';
	filename[len+2] = 'n';
	filename[len+3] = '\0';
	bool canVisit = visit(parser->state, filename);
	ZNode *node = makenode(NODE_MODULE);

	if (!canVisit) {
		node->module.name = name->str;
		node->module.root = NULL;
		return node;
	}

	ZToken **tokens = ztokenize(parser->state);

	node = zparse(parser->state, tokens);

	undoVisit(parser->state);
	return node;
}

static ZNode *parseImport(ZParser *parser) {
	expect(parser, TOK_MODULE);

	ensure(check(parser, TOK_STR_LIT));

	return getModuleByName(parser, consume(parser));
}

static ZNode *parseTypedef(ZParser *parser) {
	expect(parser, TOK_TYPEDEF);
	ensure(check(parser, TOK_IDENT));

	ZToken *alias = consume(parser);

	ZType *type = wrapType(parser, parseType);
	
	ensure(type);

	ZNode *node = makenode(NODE_TYPEDEF);
	node->typeDef.alias = alias;
	node->typeDef.type  = type;
	return node;
}

static ZNode *parseForeignDecl(ZParser *parser) {
	expect(parser, TOK_FOREIGN);

	ZType *ret = wrapType(parser, parseType);

	ensure(check(parser, TOK_IDENT));
	ZToken *name = consume(parser);

	expect(parser, TOK_LPAREN);

	ZType **args = NULL;
	while (true) {
		ZType *type = wrapType(parser, parseType);
		if (type) vecpush(args, type);
		if (check(parser, TOK_RPAREN)) break;
		if (!match(parser, TOK_COMMA)) break;
	}

	expect(parser, TOK_RPAREN);

	ZNode *node = makenode(NODE_FOREIGN);
	node->foreignFunc.ret = ret;
	node->foreignFunc.tok = name;
	node->foreignFunc.args = args;
	
	ZType *type = maketype(Z_TYPE_FUNCTION);
	type->func.ret = ret;
	type->func.args = args;

	node->resolved = type;
	return node;
}


static ZMacroPattern *macroPatternElement(ZParser *parser, ZNode *macro) {
	ZMacroPattern *self = zalloc(ZMacroPattern);
	ZMacroVar *var = zalloc(ZMacroVar);

	// Arrows break the pattern parsing
	if (match(parser, TOK_ARROW)) return NULL;

	if (match(parser, TOK_MACRO_IDENT)) {
		if (!check(parser, TOK_IDENT)) {
			error(parser->state, peek(parser), "Expected identifier after '$'");
			return NULL;
		}
		self->kind = Z_MACRO_IDENT;
		self->ident = consume(parser);
		var->name = self->ident;
		var->startIndex = 0;
		var->endIndex = 0;
		var->useCount = 0;
		vecpush(macro->macro.captured, var);
		return self;

	} else if (match(parser, TOK_MACRO_TYPE)) {
		if (!check(parser, TOK_IDENT)) {
			return NULL;
		}
		self->kind = Z_MACRO_TYPE;
		self->ident = consume(parser);
		var->name = self->ident;
		var->startIndex = 0;
		var->endIndex = 0;
		vecpush(macro->macro.captured, var);
		return self;
	} else if (match(parser, TOK_MACRO_EXPR)) {
		if (match(parser, TOK_LPAREN)) {
			//TODO: handle properly sequences
			ZMacroPattern *seq = parseMacroPattern(parser, macro);
			if (!match(parser, TOK_RPAREN)) {
				error(parser->state, peek(parser), "Expected ')' to close macro sequence");
				return NULL;
			}

			if (match(parser, TOK_PLUS)) {
				self->kind = Z_MACRO_OM;
				self->oneOrMore = seq;
			} else if (match(parser, TOK_STAR)) {
				self->kind = Z_MACRO_ZM;
				self->zeroOrMore = seq;
			}
			return self;
		} else if (check(parser, TOK_IDENT)) {
			self->kind = Z_MACRO_EXPR;
			self->ident = consume(parser);
			var->name = self->ident;
			var->startIndex = 0;
			var->endIndex = 0;
			vecpush(macro->macro.captured, var);
			return self;
		}
	} else if (match(parser, TOK_QUOTE)) {
		// Treat next token as a literal keyword regardless of type
		if (!canPeek(parser)) return NULL;
		self->kind = Z_MACRO_KEY;
		self->ident = consume(parser);
		return self;
	} else if (checkMask(parser, TOK_OVERRIDABLE)) {
		self->kind = Z_MACRO_KEY;
		self->ident = consume(parser);
		return self;
	}
	return NULL;
}

static ZMacroPattern *parseMacroPattern(ZParser *parser, ZNode *macro) {
	ZMacroPattern *seq = zalloc(ZMacroPattern);
	seq->kind = Z_MACRO_SEQ;
	seq->sequence = NULL;
	ZMacroPattern *curr = NULL;
	while (( curr = macroPatternElement(parser, macro) )) {
		vecpush(seq->sequence, curr);
	}
	return seq;
}

static void skipBlock(ZParser *parser) {
	i64 level = 1;  // Already inside { from caller

	while (level > 0) {
		if (check(parser, TOK_LBRACKET)) level++;
		else if (check(parser, TOK_RBRACKET)) level--;
		consume(parser);
	}
}

static ZNode *parseMacro(ZParser *parser) {
	ZToken *start = peek(parser);
	bool public = match(parser, TOK_PUB);
	expect(parser, TOK_MACRO);

	if (!checkMask(parser, TOK_OVERRIDABLE)) {
		error(parser->state, peek(parser), "Expected overridable keyword after 'macro', got '%s'", stoken(peek(parser)));
		return NULL;
	}

	ZNode *node = makenode(NODE_MACRO);
	node->macro.captured = NULL;
	node->macro.start = start;
	node->macro.pub = public;

	usize saved = parser->source->current;
	ZMacroPattern *pattern = parseMacroPattern(parser, node);

	if (!pattern || veclen(pattern->sequence) < 1) {
		error(parser->state, peek(parser), "Macro pattern is empty or invalid");
		return NULL;
	}

	node->macro.pattern = pattern;
	node->macro.sourceTokens = parser->source->list;

	// Arrow (->) is already consumed by macroPatternElement as the sentinel
	expect(parser, TOK_LBRACKET);  // Consume opening '{'

	node->macro.startBody = parser->source->current;  // First token after {
	skipBlock(parser);
	node->macro.endBody = parser->source->current - 1;  // Exclude closing }

	if (node->macro.endBody - node->macro.startBody == 0) {
		error(parser->state, start, "Body's macro cannot be empty");
		return NULL;
	}

	node->macro.consumed = parser->source->current - saved;

	
	vecpush(parser->macros, node);

	return node;
}

/* Macros captured the start token. Skip over the macro declaration in pass 2. */
static ZNode *skipMacro(ZParser *parser) {
	ZToken *curr = peek(parser);

	match(parser, TOK_PUB);
	if (!match(parser, TOK_MACRO)) return NULL;

	ZNode *macro = NULL;

	for (usize i = 0; i < veclen(parser->macros) && !macro; i++) {
		if (parser->macros[i]->macro.start == curr) macro = parser->macros[i];
	}

	if (!macro) return NULL;

	usize toConsume = macro->macro.consumed;
	while (toConsume --> 0) {
		consume(parser);
	}
	return macro;
}

static ZNode *parse(ZParser *parser) {
	usize len = sizeof(progFunc) / sizeof(progFunc[0]);
	return parseOrGrammar(parser, progFunc, len);
}

static bool checkPubMacro(ZParser *parser) {
	if (!check(parser, TOK_PUB)) return false;
	if (parser->source->current + 1 >= parser->source->end) return false;
	return parser->source->list[parser->source->current + 1]->type == TOK_MACRO;
}

static void discoverMacros(ZParser *parser) {
	usize saved = parser->source->current;
	while (canPeek(parser)) {
		if (check(parser, TOK_MACRO) || checkPubMacro(parser)) {
			parseMacro(parser);
		} else {
			consume(parser);
		}
	}
	parser->source->current = saved;
}

static ZNode *parseModule(ZParser *parser) {
	ZNode *root = makenode(NODE_MODULE);

	root->module.root = NULL;
	root->module.name = parser->state->filename;

	while (canPeek(parser)) {
		ZNode *child = parse(parser);
		if (!child) break;
		vecpush(root->module.root, child);
	}
	return root;
}

ZNode *zparse(ZState *state, ZToken **tokens) {
	state->currentPhase = Z_PHASE_SYNTAX;
	ZParser *parser = makeparser(state, tokens);

	discoverMacros(parser);

	ZNode *root = parseModule(parser);

	if (canPeek(parser)) {
		error(state, peek(parser), "Unexpected token '%s', expected a top-level declaration", stoken(peek(parser)));
	}

	return root;
}
