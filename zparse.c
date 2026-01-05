#include "zparse.h"
#include "zlex.h"
#include "zmem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define ensure(c) if (!(c)) return NULL
#define expect(l, t)  ensure(match(l, t))

#define error(fmt, ...) do {												\
	fprintf(stderr, "%s:%d", __FILE__, __LINE__);			\
	fprintf(stderr, fmt, __VA_ARGS__);								\
} while (0)


typedef ZNode *(*parseFunction)(ZTokens *);

// To add a parser context later to keep track of the error.
// typedef struct ZParserError {
// 	const char *message;
// 	ZToken *token;
// } ZParserError;
//
// typedef struct ZParserContext {
// 	ZTokens *iterator;
// 	vec(ZParserError *) errors;
// } ZParserContext;

static ZNode *parse(ZTokens *);
static ZType *parseType(ZTokens *);

static ZNode *makenode(ZNodeType type) {
	ZNode *self = zalloc(ZNode);
	self->type = type;
	return self;
}

static ZType *maketype(ZTypeKind kind) {
	ZType *self = zalloc(ZType);
	self->kind = kind;
	return self;
}

static ZField *makefield(ZType *type, ZToken *name) {
	ZField *field = zalloc(ZField);
	field->type = type;
	field->field = name;
	return field;
}

static inline bool canPeek(ZTokens *tokens) {
	return ((usize)tokens->current < tokens->len);
}

static inline ZToken *peek(ZTokens *tokens) {
	return canPeek(tokens) ? tokens->ptr[tokens->current] : NULL;
}

static ZToken *peekAhead(ZTokens *tokens, i32 offset) {
	i32 index = tokens->current + offset;
	ensure(index >= 0 && (usize)index < tokens->len);
	return tokens->ptr[index];
}

static ZToken *consume(ZTokens *tokens) {
	ensure(canPeek(tokens));

	ZToken *curr = peek(tokens);
	tokens->current++;
	return curr;
}

static inline bool check(ZTokens *tokens, ZTokenType type) {
	if (!canPeek(tokens)) return false;
	return peek(tokens)->type == type;
}

static bool match(ZTokens *tokens, ZTokenType type) {
	bool res = check(tokens, type);
	if (res) consume(tokens);
	return res;
}

// assignment   = identifier "=" assignment
//              | logic_or ;
//
// logic_or     = logic_and { "||" logic_and } ;
// logic_and    = equality { "&&" equality } ;
// equality     = comparison { ( "==" | "!=" ) comparison } ;
// comparison   = term { ( "<" | ">" | "<=" | ">=" ) term } ;
// term         = factor { ( "+" | "-" ) factor } ;
// factor       = unary { ( "*" | "/" ) unary } ;
//
// unary        = ( "!" | "-" ) unary
//              | primary ;
//
// primary      = number
//              | identifier
//              | "(" expression ")"

static bool isValidToken(ZTokens *tokens, ZTokenType *validTokens, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (check(tokens, validTokens[i])) return true;
	}
	return false;
}

static ZNode *wrap(ZTokens *tokens, parseFunction parser) {
	usize saved = tokens->current;
	ZNode *res = parser(tokens);
	if (!res) tokens->current = saved;
	return res;
}

static ZNode *parseGenericBinary(ZTokens *tokens,
																parseFunction getChild,
																ZTokenType *validTokens,
																size_t validTokensLen) {
	ZNode *node = NULL;

	ZNode *left = wrap(tokens, getChild);

	ensure(left);


	while (canPeek(tokens) &&
					isValidToken(tokens, validTokens, validTokensLen)) {
		node = makenode(NODE_BINARY);
		ZToken *op = consume(tokens);
		
		ZNode *right = wrap(tokens, getChild);

		if (!right) break;


		node->binary.op = op;
		node->binary.left = left;
		node->binary.right = right;
		left = node;
	}

	return node ? node : left;
}

static ZNode *parsePrimary(ZTokens *tokens) {
	if (!canPeek(tokens)) {
		return NULL;
	}

	if (check(tokens, TOK_LPAREN)) {
		consume(tokens);
		ZNode *node = parse(tokens);
		expect(tokens, TOK_RPAREN);
		return node;
	}
	if (check(tokens, TOK_IDENT)) {
		ZNode *node = makenode(NODE_IDENTIFIER);
		node->identTok = consume(tokens);
		return node;
	} else if (check(tokens, TOK_INT_LIT)) {
		ZNode *node = makenode(NODE_LITERAL);
		node->literalTok = consume(tokens);
		return node;
	}

	return NULL;
}

static ZNode *parseUnary(ZTokens *tokens) {
	ensure(canPeek(tokens));


	if (check(tokens, TOK_NOT) || check(tokens, TOK_MINUS)) {
		ZNode *node = makenode(NODE_UNARY);
		node->unary.op = consume(tokens);
		node->unary.operand = parseUnary(tokens);
		if (!node->unary.operand) {
			printf("Error parsing the operand\n");
			return NULL;
		}
		return node;
	}
	ZNode *res = parsePrimary(tokens);

	if (!res) {
		printf("returned primary is not valid\n");
	}

	return res;
}

static ZNode *parseFactor(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_STAR, TOK_DIV};
	printf("parse factor\n");
	return parseGenericBinary(tokens, parseUnary, valids, 2);
}

static ZNode *parseTerm(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_PLUS, TOK_MINUS};
	printf("parse term\n");
	return parseGenericBinary(tokens, parseFactor, valids, 2);
}

static ZNode *parseComparison(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_LT, TOK_GT, TOK_LTE, TOK_GTE};
	printf("parse comparison\n");
	return parseGenericBinary(tokens, parseTerm, valids, 4);
}

static ZNode *parseEquality(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_EQEQ, TOK_NOTEQ};
	printf("parse equality\n");
	return parseGenericBinary(tokens, parseComparison, valids, 2);
}

static ZNode *parseLogicalAnd(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_AND};
	return parseGenericBinary(tokens, parseEquality, valids, 1);
}

static ZNode *parseLogicalOr(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_OR};
	return parseGenericBinary(tokens, parseLogicalAnd, valids, 1);
}

static ZNode *parseBinary(ZTokens *tokens) {
	printf("parse binary\n");
	return parseLogicalOr(tokens);
}

static ZTypes *parseTypeArgs(ZTokens *tokens) {
	ZTypes *args = zalloc(ZTypes);

	vec_init(args, 4);

	do {
		ZType *curr = parseType(tokens);
		if (!curr) return args;

		vec_push(args, curr);
	} while (1);
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

static ZType *parseType(ZTokens *tokens) {
	ensure(canPeek(tokens));

	u8 stars = 0;
	while (match(tokens, TOK_STAR)) stars++;

	// After stars i must have a primitive type
	if (!canPeek(tokens) || !(peek(tokens)->type & TOK_TYPES_MASK)) {
		printf("Missing primitive type for this pointer");
		return NULL;
	}

	ZType *base = maketype(Z_TYPE_PRIMITIVE);
	base->token = peek(tokens);
	base = applyStarsToType(base, stars);


	if (match(tokens, TOK_LPAREN)) {
		ZTypes *args = parseTypeArgs(tokens);
		expect(tokens, TOK_RPAREN);

		ZType *type = maketype(Z_TYPE_FUNCTION);
		type->func.ret = base;
		type->func.args = args;
	}

	return base;
}

static ZNode *parseStmt(ZTokens *tokens) {
	(void)tokens;
	return NULL;
}

static ZNode *parseBlock(ZTokens *tokens) {
	ensure(canPeek(tokens) && match(tokens, TOK_LBRACKET));
	ZNode *block = makenode(NODE_BLOCK);

	vec_init(&block->block, 8);
	
	ZNode *stmt;
	do {
		stmt = parseStmt(tokens);
		if (stmt) vec_push(&block->block, stmt);
	} while (stmt);

	ensure(match(tokens, TOK_RBRACKET));

	return block;
}

static ZNode *parseStructDecl(ZTokens *tokens) {
	expect(tokens, TOK_STRUCT);
	expect(tokens, TOK_IDENT);
	ZToken *name = peekAhead(tokens, -1);
	if (!name) return NULL;

	expect(tokens, TOK_LBRACKET);

	ZFields *fields = zalloc(ZFields);
	vec_init(fields, 4);

	while (canPeek(tokens) && peek(tokens)->type != TOK_RBRACKET) {
		ZType *type = parseType(tokens);

		if (!type) {
			return NULL;
		}

		expect(tokens, TOK_IDENT);
		ZToken *ident = peekAhead(tokens, -1);

		vec_push(fields, makefield(type, ident));
	}

	ZNode *node = makenode(NODE_STRUCT);
	node->structDef.fields = fields;
	node->structDef.ident = name;
	return node;
}

static ZNode *parseExpr(ZTokens *tokens) {
	return parseBinary(tokens);
}

static ZNode *parseIf(ZTokens *tokens) {

	expect(tokens, TOK_IF);
	expect(tokens, TOK_LPAREN);
	
	ZNode *cond = parseExpr(tokens);

	expect(tokens, TOK_RPAREN);

	ZNode *body = parseBlock(tokens);
	if (!body) return NULL;

	ZNode *node = makenode(NODE_IF);

	if (match(tokens, TOK_ELSE)) {
		ZNode *elseBody = parseBlock(tokens);
		if (!elseBody) return NULL;
		node->ifStmt.elseBranch = elseBody;
	}

	node->ifStmt.cond = cond;
	node->ifStmt.body = body;
	return node;
}

static ZNode *parseWhile(ZTokens *tokens) {
	expect(tokens, TOK_WHILE);
	expect(tokens, TOK_LPAREN);
	ZNode *cond = parseExpr(tokens);
	expect(tokens, TOK_RPAREN);
	ZNode *body = parseBlock(tokens);

	ZNode *node = makenode(NODE_WHILE);
	node->whileStmt.branch = body;
	node->whileStmt.cond = cond;
	return node;
}


static ZNode *parseFuncDecl(ZTokens *tokens) {
	ZType *ret = parseType(tokens);
	if (!ret || !canPeek(tokens) || !check(tokens, TOK_IDENT)) return NULL;

	ZToken *name = consume(tokens);

	ensure(canPeek(tokens) && match(tokens, TOK_LPAREN));

	ZFields *args = zalloc(ZFields);
	vec_init(args, 4);

	while (canPeek(tokens)) {
		ZType *argType = parseType(tokens);
		if (!argType) break;
		ZToken *argName = consume(tokens);
	
		if (!argName) {
			printf("Error parsing argument function\n");
			return NULL;
		}

		if (!check(tokens, TOK_COMMA)) {
			printf("Expected a comma here\n");
			return NULL;
		}
		ZField *arg = makefield(argType, argName);
		vec_push(args, arg);
	}

	ZNode *body = parseBlock(tokens);
	if (!body) return NULL;

	ZNode *node = makenode(NODE_FUNC);
	node->funcDef.ret = ret;
	node->funcDef.ident = name;
	node->funcDef.args = args;
	node->funcDef.body = body;
	return node;
}

static ZNode *parse(ZTokens *tokens) {
	if (!canPeek(tokens)) return NULL;

	ZToken *curr = peek(tokens);
	
	if (curr->type & TOK_TYPES_MASK || curr->type == TOK_STAR) {
		return parseFuncDecl(tokens);
	}

	switch (curr->type) {
		case TOK_STRUCT:
		return parseStructDecl(tokens);
		case TOK_IF:
		return parseIf(tokens);
		case TOK_WHILE:
		return parseWhile(tokens);
		case TOK_INT_LIT:
		return parseTerm(tokens);
		default:
			error("Unhandled node type %d", peek(tokens)->type);
			return NULL;
		break;
	}
}

ZNode *zparse(ZTokens *tokens) {
	tokens->current = 0;
	printTokens(tokens);
	return parse(tokens);
}
