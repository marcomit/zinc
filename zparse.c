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

typedef struct ZParserError {
	const char *message;
	ZToken *token;
} ZParserError;

typedef struct ZParserContext {
	ZTokens *iterator;
	vec(ZParserError *) errors;
} ZParserContext;

static ZType *parseType		(ZTokens *);
static ZNode *parse				(ZTokens *);
static ZNode *parseIf			(ZTokens *);
static ZNode *parseWhile	(ZTokens *);
static ZNode *parseReturn	(ZTokens *);
static ZNode *parseBlock	(ZTokens *);
static ZNode *parseExpr		(ZTokens *);

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
	return canPeek(tokens) && peek(tokens)->type == type;
}
static inline bool checkMask(ZTokens *tokens, u32 mask) {
	return canPeek(tokens) && peek(tokens)->type & mask;
}

static bool match(ZTokens *tokens, ZTokenType type) {
	bool res = check(tokens, type);
	if (res) consume(tokens);
	return res;
}

static bool matchMask(ZTokens *tokens, u32 mask) {
	bool res = checkMask(tokens, mask);
	if (res) consume(tokens);
	return res;
}

void printType(ZType *type) {
	if (!type) {
		printf("unknown");
		return;
	}

	if (type->constant) printf("const ");

	switch(type->kind) {
	case Z_TYPE_POINTER:
		printf("*");
		printType(type->base);
		break;
	case Z_TYPE_PRIMITIVE:
		printToken(type->token);
		break;
	case Z_TYPE_FUNCTION:
		printType(type->func.ret);
		printf("(");
		for (usize i = 0; i < type->func.args->len; i++) {
			printType(type->func.args->ptr[i]);
			if (i < type->func.args->len - 1) printf(", ");
		}
		printf(")");
		break;
	case Z_TYPE_STRUCT:
		printf("struct %s {", type->strct.name->str);
		foreach(field, type->strct.fields){
			printType(field->type);
			printf("%s\n", field->field->str);
		}
		printf("}");
		break;
	case Z_TYPE_ARRAY:
		printf("[%zu]", type->array.size);
		printType(type->array.base);
		break;
	}
}

void printNode(ZNode *node, u8 indent) {
	if (!node) {
		printf("unknown");
		return;
	}

	// Helper to print indentation
	for (int i = 0; i < indent; i++) printf("  ");

	printf("[%s] ", (char*[]){
			"BLOCK", "IF", "WHILE", "RETURN", "VAR_DECL", "ASSIGN", 
			"BINARY", "UNARY", "CALL", "FUNC", "LITERAL", "IDENTIFIER", 
			"CAST", "STRUCT", "SUBSCRIPT", "MEMBER"
	}[node->type]);

	switch (node->type) {
	case NODE_LITERAL:
		printf("Value: %s", node->literalTok->str);
		break;

	case NODE_IDENTIFIER:
		printf("Name: %s", node->identTok->str);
		break;

	case NODE_BINARY:
		printf("Op: ");
		printToken(node->binary.op);
		printf("\n");
		printNode(node->binary.left, indent + 1);
		printNode(node->binary.right, indent + 1);
		return; // Return early to avoid the double newline

	case NODE_VAR_DECL:
		printf("Var: %s Type: ", node->varDecl.lvalue->identTok->str);
		printType(node->varDecl.type);
		if (node->varDecl.rvalue) {
			printf("\n");
			printNode(node->varDecl.rvalue, indent + 1);
		}
		break;

	case NODE_BLOCK:
		printf("\n");
		foreach(stmt, &node->block) printNode(stmt, indent + 1);

		return;

	case NODE_FUNC:
		if (node->funcDef.receiver) {
			printf("Receiver: ");
			printType(node->funcDef.receiver);
			printf(" ");
		}
		printf("Name: %s\n", node->funcDef.ident->str);
		printNode(node->funcDef.body, indent + 1);
		return;

	case NODE_CALL:
			printf("Callee: %s\n", node->call.callee->identTok->str);
			foreach(arg, &node->call.args) printNode(arg, indent + 1);

			return;

	case NODE_RETURN:
			printf("\n");
			if (node->returnStmt.expr) printNode(node->returnStmt.expr, indent + 1);
			return;

	// Add cases for IF, WHILE, MEMBER, etc., following the same pattern
	default:
			printf("(details not implemented in printer)");
			break;
	}
	printf("\n");
}

static bool isValidToken(ZTokens *tokens, ZTokenType *validTokens, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (check(tokens, validTokens[i])) return true;
	}
	return false;
}

static ZNode *wrapNode(ZTokens *tokens, parseFunction parser) {
	usize saved = tokens->current;
	ZNode *res = parser(tokens);
	if (!res) tokens->current = saved;
	return res;
}

static ZType *wrapType(ZTokens *tokens, ZType *(*parser)(ZTokens *)) {
	usize saved = tokens->current;
	ZType *res = parser(tokens);
	if (!res) tokens->current = saved;
	return res;
}

static ZNode *parseGenericBinary(ZTokens *tokens,
																parseFunction getChild,
																ZTokenType *validTokens,
																size_t validTokensLen) {
	ZNode *node = NULL;

	ZNode *left = wrapNode(tokens, getChild);

	ensure(left);


	while (canPeek(tokens) &&
					isValidToken(tokens, validTokens, validTokensLen)) {
		node = makenode(NODE_BINARY);
		ZToken *op = consume(tokens);
		
		ZNode *right = wrapNode(tokens, getChild);

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

	return res;
}

static ZNode *parseFactor(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_STAR, TOK_DIV};
	return parseGenericBinary(tokens, parseUnary, valids, 2);
}

static ZNode *parseTerm(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_PLUS, TOK_MINUS};
	return parseGenericBinary(tokens, parseFactor, valids, 2);
}

static ZNode *parseComparison(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_LT, TOK_GT, TOK_LTE, TOK_GTE};
	return parseGenericBinary(tokens, parseTerm, valids, 4);
}

static ZNode *parseEquality(ZTokens *tokens) {
	ZTokenType valids[] = {TOK_EQEQ, TOK_NOTEQ};
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
	return parseLogicalOr(tokens);
}

static ZTypes *parseTypeArgs(ZTokens *tokens) {
	ZTypes *args = zalloc(ZTypes);

	vec_init(args, 4);

	do {
		ZType *curr = parseType(tokens);
		if (!curr) return args;

		vec_push(args, curr);
		if (!match(tokens, TOK_COMMA)) break;
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
	usize saved = tokens->current;

	u8 stars = 0;
	while (match(tokens, TOK_STAR)) stars++;

	// After stars expected token is a primitive type
	if (!checkMask(tokens, TOK_TYPES_MASK)) {
		tokens->current = saved;
		return NULL;
	}

	ZType *base = maketype(Z_TYPE_PRIMITIVE);
	base->token = consume(tokens);
	base = applyStarsToType(base, stars);


	if (match(tokens, TOK_LPAREN)) {
		printf("Try to get argument types\n");
		ZTypes *args = parseTypeArgs(tokens);
		expect(tokens, TOK_RPAREN);

		ZType *type = maketype(Z_TYPE_FUNCTION);
		type->func.ret = base;
		type->func.args = args;

		return type;
	}

	return base;
}

static ZNode *parseStmt(ZTokens *tokens) {
	parseFunction toTry[] = {
		parseIf,
		parseExpr,
		parseWhile,
		parseReturn,
		parseBlock,
	};

	usize saved = tokens->current;
	ZNode *node = NULL;
	for (u8 i = 0; i < (u8)(sizeof(toTry) / sizeof(parseFunction)); i++) {
		node = toTry[i](tokens);

		if (node) return node;

		tokens->current = saved;
	}
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

static ZNode *parseReturn(ZTokens *tokens) {
	ensure(canPeek(tokens) && match(tokens, TOK_RETURN));
	return parseExpr(tokens);
}

static ZNode *parseIf(ZTokens *tokens) {
	expect(tokens, TOK_IF);
	
	ZNode *cond = parseExpr(tokens);

	ZNode *body = parseBlock(tokens);
	if (!body) return NULL;

	ZNode *node = makenode(NODE_IF);

	if (canPeek(tokens) && match(tokens, TOK_ELSE)) {
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

	ZNode *cond = parseExpr(tokens);
	ZNode *body = parseBlock(tokens);

	ZNode *node = makenode(NODE_WHILE);
	node->whileStmt.branch = body;
	node->whileStmt.cond = cond;
	return node;
}


static ZNode *parseFuncDecl(ZTokens *tokens) {
	ZType *ret = wrapType(tokens, parseType);
	if (!ret) {
		printf("Function declaration failed to get return type\n");
	}
	printType(ret);
	if (!ret || !canPeek(tokens) || !check(tokens, TOK_IDENT)) return NULL;

	ZToken *name = consume(tokens);

	ensure(canPeek(tokens) && match(tokens, TOK_LPAREN));
	printToken(peek(tokens));

	ZFields *args = zalloc(ZFields);
	vec_init(args, 4);

	while (canPeek(tokens)) {
		ZType *argType = wrapType(tokens, parseType);
		if (!argType) {
			if (match(tokens, TOK_RPAREN)) break;
			return NULL;
		}
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
	
	if (check(tokens, TOK_STAR) || checkMask(tokens, TOK_TYPES_MASK)) {
		printf("try to parse function declaration\n");
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
		return parseBinary(tokens);
		default:
			error("Unhandled node type %d", peek(tokens)->type);
			return NULL;
		break;
	}
}

ZNode *zparse(ZTokens *tokens) {
	tokens->current = 0;
	return parse(tokens);
}
