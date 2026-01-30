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
#define expect(l, t) do {																												\
	if (!match(l, t)) {																														\
		error((l)->state, peek(l), "Expected %s, got %s", tokname(t), stoken(peek(parser)));			\
		return NULL;																																\
	}																																							\
} while (0)

typedef ZNode *(*ParseFunction)(ZParser *);

ZType *parseType												(ZParser *);
ZNode	*parseExpr												(ZParser *);
static ZNode *parse											(ZParser *);
static ZNode *parseIf										(ZParser *);
static ZNode *parseWhile								(ZParser *);
static ZNode *parseFor									(ZParser *);
static ZNode *parseReturn								(ZParser *);
static ZNode *parseBlock								(ZParser *);
static ZNode *parseArrayLit							(ZParser *);
static ZNode *parseTupleLit							(ZParser *);
static ZNode *parseStructLit						(ZParser *);
static ZNode *parseVarDecl							(ZParser *);
static ZNode *parseVarDef								(ZParser *);
static ZNode *parseMatch								(ZParser *);
static ZNode *parseDefer								(ZParser *);
static ZNode *parseGoto									(ZParser *);
static ZNode *parseLabel								(ZParser *);
static ZNode *parseBinary								(ZParser *);
static ZToken **parseGenericsDecl				(ZParser *);
static ZMacroPattern *parseMacroPattern	(ZParser *, ZNode *);

static ParseFunction stmtFunc[] = {
	expandMacro,
	parseIf,
	parseWhile,
	parseFor,
	parseMatch,
	parseReturn,
	parseBlock,
	parseVarDecl,
	parseVarDef,
	parseDefer,
	parseExpr,
	parseGoto,
	parseLabel,
};

static ParseFunction exprFunc[] = {
	parseStructLit,
	parseBinary,
	parseArrayLit,
	parseTupleLit
};

static ZParser *makeparser(ZState *state, ZToken **tokens) {
	ZParser *self = zalloc(ZParser);
	self->current = 0;
	self->tokens = tokens;
	self->errstack = NULL;
	self->depth = 0;
	self->state = state;
	self->macros = NULL;
	self->currentMacro = NULL;
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

// ZMacro *makemacro(ZMacroType kind) {
// 	ZMacro *self = zalloc(ZMacro);
// 	self->kind = kind;
// 	return self;
// }

static ZNode *makenodevar(ZToken *ident, ZType *type, ZNode *expr) {
	ZNode *node = makenode(NODE_VAR_DECL);
	node->varDecl.ident = ident;
	node->varDecl.type = type;
	node->varDecl.rvalue = expr;
	return node;
}

inline bool canPeek(ZParser *parser) {
	return (usize)parser->current < veclen(parser->tokens);
}

inline ZToken *peek(ZParser *parser) {
	return canPeek(parser) ? parser->tokens[parser->current] : NULL;
}

ZToken *consume(ZParser *parser) {
	ensure(canPeek(parser));

	ZToken *curr = peek(parser);
	parser->current++;
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
	usize errlen = veclen(parser->errstack);
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

static ZNode *wrapNode(ZParser *parser, ParseFunction parse) {
	usize saved = parser->current;
	ZNode *res = parse(parser);

	pushErrorCheckpoint(parser);

	if (!res) {
		parser->current = saved;
		commitErrors(parser);
	} else {
		rollbackErrors(parser);
	}
	return res;
}

static ZType *wrapType(ZParser *parser, ZType *(*parse)(ZParser *)) {
	usize saved = parser->current;
	ZType *res = parse(parser);

	pushErrorCheckpoint(parser);

	if (!res) {
		parser->current = saved;
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
		usize saved = parser->current;
		node = makenode(NODE_BINARY);
		ZToken *op = consume(parser);

		ZNode *right = wrapNode(parser, parseRight);

		if (!right) {
			parser->current = saved;
			node = NULL;
			break;
		}

		node->binary.op = op;
		node->binary.left = left;
		node->binary.right = right;
		left = node;
	}

	return node ? node : left;
}

static ZNode *parsePrimary(ZParser *parser) {
	ensure(canPeek(parser));

	if (check(parser, TOK_LPAREN)) {
		consume(parser);
		ZNode *node = wrapNode(parser, parseExpr);
		expect(parser, TOK_RPAREN);
		return node;
	} else if (check(parser, TOK_IDENT)) {
		ZNode *node = makenode(NODE_IDENTIFIER);
		node->identTok = consume(parser);
		return node;
	} else if (checkMask(parser, TOK_LITERAL)) {
		ZNode *node = makenode(NODE_LITERAL);
		node->literalTok = consume(parser);
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
	return node;
}

static ZNode *parsePostfixOper(ZParser *parser, ZNode *previous) {
	usize saved = parser->current;
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
		parser->current = saved;
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
	ZTokenType valids[] = {TOK_PLUS, TOK_MINUS, TOK_NOT, TOK_STAR, TOK_REF};
	usize len = sizeof(valids) / sizeof(valids[0]);

	if (!isValidToken(parser, valids, len)) {
		error(parser->state, peek(parser), "Failed to parse unary expression");
		return NULL;
	}

	node = makenode(NODE_UNARY);
	node->unary.operat = consume(parser);
	node->unary.operand = wrapNode(parser, parseUnary);

	ensure(node->unary.operand);

	return node;
}

static ZNode *parseFactor(ZParser *parser) {
	ZTokenType valids[] = {TOK_STAR, TOK_DIV};
	return parseGenericBinary(parser, parseUnary, parseUnary, valids, 2);
}

static ZNode *parseTerm(ZParser *parser) {
	ZTokenType valids[] = {TOK_PLUS, TOK_MINUS};
	return parseGenericBinary(parser, parseFactor, parseFactor, valids, 2);
}

static ZNode *parseComparison(ZParser *parser) {
	ZTokenType valids[] = {TOK_LT, TOK_GT, TOK_LTE, TOK_GTE};
	return parseGenericBinary(parser, parseTerm, parseTerm, valids, 4);
}

static ZNode *parseEquality(ZParser *parser) {
	ZTokenType valids[] = {TOK_EQEQ, TOK_NOTEQ};
	return parseGenericBinary(parser, parseComparison, parseComparison, valids, 2);
}

static ZNode *parseLogicalAnd(ZParser *parser) {
	ZTokenType valids[] = {TOK_AND};
	return parseGenericBinary(parser, parseEquality, parseEquality, valids, 1);
}

static ZNode *parseLogicalOr(ZParser *parser) {
	ZTokenType valids[] = {TOK_OR};
	return parseGenericBinary(parser, parseLogicalAnd, parseLogicalAnd, valids, 1);
}

static ZNode *parseAssignment(ZParser *parser) {
	ZTokenType valids[] = {TOK_EQ};
	return parseGenericBinary(parser, parseLogicalOr, parseAssignment, valids, 1);
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
			printf("Failed parsing a type list\n");
			return args;
		}
		vecpush(args, curr);
		if (!match(parser, TOK_COMMA)) break;
		if (check(parser, right)) break;
	} while (true);
	if (!match(parser, right)) {
		printf("parseTypeList failed: missing %d token\n", right);
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
	expect(parser, TOK_RSBRACKET);

	ZType *arr = maketype(Z_TYPE_ARRAY);
	arr->array.base = type;
	arr->array.size = 0;
	return arr;
}

/* A tuple must have at least 2 types */
static ZType *parseTypeTuple(ZParser *parser) {
	ZType **types = parseTypeList(parser, TOK_LPAREN, TOK_RPAREN);

	if (!types) {
		printf("Failed to parse the tuple type\n");
		return NULL;
	} else if (veclen(types) < 2) {
		printf("Tuple must have at least 2 types\n");
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
			printf("Error parsing generics function type");
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
	ZNode *expr = wrapNode(parser, parseExpr);
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

static ZNode *parseStmt(ZParser *parser) {
	usize len = sizeof(stmtFunc) / sizeof(stmtFunc[0]);
	return parseOrGrammar(parser, stmtFunc, len);
}

static ZNode *parseBlock(ZParser *parser) {
	expect(parser, TOK_LBRACKET);

	ZNode *block = makenode(NODE_BLOCK);
	ZNode *stmt = NULL;
	do {
		stmt = parseStmt(parser);
		if (stmt) vecpush(block->block, stmt);
	} while (stmt);

	if (!check(parser, TOK_RBRACKET)) {
		printf("Error parsing block statement\n");
	}

	expect(parser, TOK_RBRACKET);


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

static ZNode *parseUnionDecl(ZParser *parser) {
	expect(parser, TOK_UNION);
	expect(parser, TOK_RBRACKET);

	ZNode **fields = NULL;
	ZNode *field = NULL;
	while (( field = parseField(parser) )) vecpush(fields, field);

	expect(parser, TOK_RBRACKET);
	
	ZNode *node = makenode(NODE_UNION);

	node->unionDef.fields = fields;
	node->unionDef.ident = NULL;

	return node;
}

static ZNode *parseStructDecl(ZParser *parser) {
	ensure(match(parser, TOK_STRUCT));
	ZToken *name = NULL;
	ZToken **generics = NULL;

	if (check(parser, TOK_IDENT)) name = consume(parser);

	if (!name) {
		error(parser->state, peek(parser), "Expected an identifier after 'struct'");
		return NULL;
	}

	if (check(parser, TOK_LSBRACKET)) {
		generics = parseGenericsDecl(parser);
		if (!generics) {
			printf("Generics not parsed correctly\n");
		}
		printf("Generics parsed\n");
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
	return node;
}

ZNode *parseExpr(ZParser *parser) {
	usize len = sizeof(exprFunc) / sizeof(exprFunc[0]);

	if (parser->currentMacro && match(parser, TOK_MACRO_EXPR)) {
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

	return parseOrGrammar(parser, exprFunc, len);
	// return parseBinary(parser);
}

static ZNode *parseReturn(ZParser *parser) {
	ensure(canPeek(parser) && match(parser, TOK_RETURN));
	ZNode *ret = makenode(NODE_RETURN);

	ret->returnStmt.expr = parseExpr(parser);
	return ret;
}

static ZNode *parseIf(ZParser *parser) {
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
	return node;
}

static ZNode *parseFor(ZParser *parser) {
	expect(parser, TOK_FOR);

	ensure(check(parser, TOK_IDENT));
	ZToken *ident = consume(parser);

	expect(parser, TOK_IN);
	
	ZNode *node = makenode(NODE_FOR);

	ZNode *expr = wrapNode(parser, parseExpr);
	if (!expr) {
		printf("Error parsing for loop, expected an expression here!\n");
	}

	ZNode *block = wrapNode(parser, parseBlock);
	if (!block) {
		printf("Error parsing for loop, expected an expression here!\n");
	}
	node->forStmt.ident = ident;
	node->forStmt.iterator = expr;
	node->forStmt.block = block;

	if (!node->forStmt.iterator || !node->forStmt.block) {
		printf("For parsing failed %s\n", stoken(peek(parser)));
	}

	return node;
}

static ZNode *parseWhile(ZParser *parser) {
	expect(parser, TOK_WHILE);

	ZNode *cond = wrapNode(parser, parseExpr);
	ZNode *body = wrapNode(parser, parseBlock);

	ZNode *node = makenode(NODE_WHILE);
	node->whileStmt.branch = body;
	node->whileStmt.cond = cond;
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
	ZType *ret = wrapType(parser, parseType);

	if (!ret || !canPeek(parser) || !check(parser, TOK_IDENT)) return NULL;

	ZToken *name = consume(parser);
	ZToken **generics = NULL;

	if (check(parser, TOK_LSBRACKET)) {
		generics = parseGenericsDecl(parser);
		if (!generics) {
			error(parser->state, peek(parser), "Error parsing generics");
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
			printf("Error parsing argument function\n");
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

	usize saved = parser->current;
	ZNode *receiver = wrapNode(parser, parseField);

	if (!receiver) parser->current = saved;

	ZNode *body = wrapNode(parser, parseBlock);

	if (!body) {
		error(parser->state, peek(parser), "Error parsing block");
	}

	ensure(body);

	ZNode *node = makenode(NODE_FUNC);
	node->funcDef.ret = ret;
	node->funcDef.ident = name;
	node->funcDef.args = args;
	node->funcDef.body = body;
	node->funcDef.receiver = receiver;
	node->funcDef.generics = generics;

	return node;
}

static ZNode *parseVarInferred(ZParser *parser) {
	ensure(check(parser, TOK_IDENT));

	ZToken *ident = consume(parser);
	expect(parser, TOK_ASSIGN);

	ZNode *expr = wrapNode(parser, parseExpr);

	if (!expr) {
		error(parser->state, peek(parser), "Error parsing expression");
	}

	return makenodevar(ident, NULL, expr);
}

static ZNode *parseVarDefTyped(ZParser *parser) {
	ZType *type = wrapType(parser, parseType);

	ensure(type && check(parser, TOK_IDENT));

	ZToken *ident = consume(parser);
	ZNode *expr = NULL;

	// expect(parser, TOK_EQ);

	if (match(parser, TOK_EQ)) {
		expr = wrapNode(parser, parseExpr);
		if (!expr) {
			error(parser->state, peek(parser), "Error parsing the expression");
			return NULL;
		}
	}

	return makenodevar(ident, type, expr);
}

static ZNode *parseVarDef(ZParser *parser) {
	ParseFunction func[] = {
		parseVarInferred, parseVarDefTyped
	};
	return parseOrGrammar(parser, func, 2);
}

static ZNode *parseVarDecl(ZParser *parser) {
	ZType *type = wrapType(parser, parseType);

	ensure(type && check(parser, TOK_IDENT));

	ZToken *ident = consume(parser);

	expect(parser, TOK_EQ);

	ZNode *node = makenode(NODE_VAR_DECL);

	ZNode *rvalue = wrapNode(parser, parseExpr);

	node->varDecl.ident = ident;
	node->varDecl.rvalue = rvalue;
	node->varDecl.type = type;

	return node;
}

static ZNode *parseTupleLit(ZParser *parser) {
	ensure(check(parser, TOK_LPAREN));
	ZNode *expr = NULL;
	ZNode **fields = NULL;
	while (( expr = parseExpr(parser) )) vecpush(fields, expr);
	ensure(check(parser, TOK_RPAREN));

	ZNode *node = makenode(NODE_TUPLE_LIT);
	node->tuplelit.fields = fields;
	return node;
}

static ZNode *parseArrayLit(ZParser *parser) {
	expect(parser, TOK_LSBRACKET);

	ZNode **values = NULL;
	ZNode *expr = NULL;

	while (( expr = wrapNode(parser, parseExpr) )) {
		vecpush(values, expr);
		if (check(parser, TOK_RSBRACKET)) break;
		if (!match(parser, TOK_COMMA)) {
			error(parser->state, peek(parser), "Expected ',', got %s", stoken(peek(parser)));
		}
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
			printf("Error parsing generics\n");
			return NULL;
		}
	}

	expect(parser, TOK_LBRACKET);

	ZNode *node = makenode(NODE_STRUCT_LIT);
	node->structlit.ident = ident;
	node->structlit.generics = generics;

	while (true) {
		if (!check(parser, TOK_IDENT)) break;
		ZToken *ident = consume(parser);
		if (!match(parser, TOK_COLON)) {
			error(parser->state, peek(parser), "Expected a ':', got %s", stoken(peek(parser)));
			return NULL;
		}
		ZNode *expr = wrapNode(parser, parseExpr);
		if (!expr) return NULL;
		ZNode *var = makenodevar(ident, NULL, expr);
		vecpush(node->structlit.fields, var);
		if (check(parser, TOK_RBRACKET)) break;
		if (!match(parser, TOK_COMMA)) {
			error(parser->state, peek(parser), "Expected a ',', got %s", stoken(peek(parser)));
			return NULL;
		}
	}

	expect(parser, TOK_RBRACKET);

	return node;
}

static ZNode *getModuleByName(ZParser *parser, ZToken *name) {
	bool canVisit = visit(parser->state, name->str);
	ZNode *node = makenode(NODE_MODULE);

	if (!canVisit) {
		node->module.name = name;
		node->module.root = NULL;
		return node;
	}

	ZToken **tokens = ztokenize(parser->state);



	node->module.root = zparse(parser->state, tokens);
	node->module.name = name;

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
	return node;
}


static ZMacroPattern *macroPatternElement(ZParser *parser, ZNode *macro) {
	ZNode *node = zalloc(ZNode);
	ZMacroPattern *self = zalloc(ZMacroPattern);
	ZMacroVar *var = zalloc(ZMacroVar);

	// Arrows break the pattern parsing
	if (match(parser, TOK_ARROW)) return NULL;

	if (match(parser, TOK_MACRO_IDENT)) {
		if (!check(parser, TOK_IDENT)) {
			printf("Unexpected token\n");
			return NULL;
		}
		self->kind = Z_MACRO_IDENT;
		self->ident = consume(parser);
		var->name = self->ident;
		var->captured = node;
		vecpush(macro->macro.captured, var);
		return self;

	} else if (match(parser, TOK_MACRO_TYPE)) {
		if (!check(parser, TOK_IDENT)) {
			return NULL;
		}
		self->kind = Z_MACRO_TYPE;
		self->ident = consume(parser);
		var->name = self->ident;
		var->captured = node;
		vecpush(macro->macro.captured, var);
		return self;
	} else if (match(parser, TOK_MACRO_EXPR)) {
		if (match(parser, TOK_LPAREN)) {
			//TODO: handle properly sequences
			ZMacroPattern *seq = parseMacroPattern(parser, macro);
			if (!match(parser, TOK_RPAREN)) {
				printf("Unterminated expression\n");
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
			var->captured = node;
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

static ZNode *parseMacro(ZParser *parser) {
	ZToken *start = peek(parser);
	expect(parser, TOK_MACRO);

	if (!checkMask(parser, TOK_OVERRIDABLE)) {
		printf("Unexpected token");
		printToken(peek(parser));
		printf("\n");
		return NULL;
	}

	ZNode *node = makenode(NODE_MACRO);
	node->macro.captured = NULL;
	node->macro.start = start;

	usize saved = parser->current;
	ZMacroPattern *pattern = parseMacroPattern(parser, node);

	if (!pattern || veclen(pattern->sequence) < 1) {
		printf("Invalid macro pattern\n");
		return NULL;
	}

	parser->currentMacro = node;
	ZNode *block = parseBlock(parser);
	printNode(block, 2);
	printf("Macro parsed\n");
	parser->currentMacro = NULL;

	node->macro.pattern = pattern;
	node->macro.block = block;
	node->macro.consumed = parser->current - saved;

	vecpush(parser->macros, node);

	return node;
}

/* Macros captured the start token. */
static ZNode *skipMacro(ZParser *parser) {
	ZToken *curr = peek(parser);

	expect(parser, TOK_MACRO);
	ZNode *macro = NULL;

	for (usize i = 0; i < veclen(parser->macros) && !macro; i++) {
		if (parser->macros[i]->macro.start == curr) macro = parser->macros[i];
	}

	if (!macro) return NULL;

	usize toConsume = macro->macro.consumed;
	while (toConsume --> 0) {
		consume(parser);
	}
	printf("Tok: %s\n", stoken(peek(parser)));

	return macro;
}

static ZNode *parse(ZParser *parser) {
	ParseFunction pf[] = {
		parseImport,
		parseForeignDecl,
		parseTypedef,
		parseFuncDecl,
		parseStructDecl,
		parseUnionDecl,
		parseVarDef,
		skipMacro
	};
	usize len = sizeof(pf) / sizeof(pf[0]);
	return parseOrGrammar(parser, pf, len);
}

static void discoverMacros(ZParser *parser) {
	usize saved = parser->current;
	while (canPeek(parser)) {
		if (check(parser, TOK_MACRO)) {
			ZNode *macro = parseMacro(parser);
			printNode(macro, 2);
		} else {
			consume(parser);
		}
	}
	parser->current = saved;
}

static ZNode *parseProgram(ZParser *parser) {
	ZNode *root = makenode(NODE_PROGRAM);
	root->program = NULL;


	while (canPeek(parser)) {
		ZNode *child = parse(parser);
		if (!child) break;
		vecpush(root->program, child);
	}
	return root;
}

ZNode *zparse(ZState *state, ZToken **tokens) {
	state->currentPhase = Z_PHASE_SYNTAX;
	ZParser *parser = makeparser(state, tokens);

	discoverMacros(parser);

	ZNode *root = parseProgram(parser);
	if (canPeek(parser)) {
		error(state, peek(parser), "Compilation failed");
	}

	return root;
}
