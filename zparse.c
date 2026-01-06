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

typedef struct ZParserError {
	const char *message;
	ZToken *token;
} ZParserError;

typedef struct ZParser {
	ZToken **tokens;
	ZParserError **errors;
	u64 current;
	u8 depth;
} ZParser;

typedef ZNode *(*ParseFunction)(ZParser *);

static ZType *parseType		(ZParser *);
static ZNode *parse				(ZParser *);
static ZNode *parseIf			(ZParser *);
static ZNode *parseWhile	(ZParser *);
static ZNode *parseReturn	(ZParser *);
static ZNode *parseBlock	(ZParser *);
static ZNode *parseExpr		(ZParser *);

static ZParser *makeparser(ZToken **tokens) {
	ZParser *self = zalloc(ZParser);
	self->current = 0;
	self->tokens = tokens;
	self->errors = NULL;
	self->depth = 0;
	return self;
}

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

static inline bool canPeek(ZParser *parser) {
	return (usize)parser->current < veclen(parser->tokens);
}

static inline ZToken *peek(ZParser *parser) {
	return canPeek(parser) ? parser->tokens[parser->current] : NULL;
}

static ZToken *peekAhead(ZParser *parser, i32 offset) {
	i32 index = parser->current + offset;
	ensure(index >= 0 && (usize)index < veclen(parser->tokens));
	return parser->tokens[index];
}

static ZToken *consume(ZParser *parser) {
	ensure(canPeek(parser));

	ZToken *curr = peek(parser);
	parser->current++;
	return curr;
}

static inline bool check(ZParser *parser, ZTokenType type) {
	return canPeek(parser) && peek(parser)->type == type;
}
static inline bool checkMask(ZParser *tokens, u32 mask) {
	return canPeek(tokens) && peek(tokens)->type & mask;
}

static bool match(ZParser *parser, ZTokenType type) {
	bool res = check(parser, type);
	if (res) consume(parser);
	return res;
}

static bool matchMask(ZParser *parser, u32 mask) {
	bool res = checkMask(parser, mask);
	if (res) consume(parser);
	return res;
}

static void reportError(ZParser *parser, const char *message) {
	ZParserError *error = zalloc(ZParserError);
	error->message = message;
	error->token = peek(parser);
	vecpush(parser->errors, error);
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
		for (usize i = 0; i < veclen(type->func.args); i++) {
			printType(type->func.args[i]);
			if (i < veclen(type->func.args) - 1) printf(", ");
		}
		printf(")");
		break;
	case Z_TYPE_STRUCT:
		printf("struct %s {", type->strct.name->str);
		for (usize i = 0; i < veclen(type->strct.fields); i++) {
			printType(type->strct.fields[i]->type);
			printf("%s\n", type->strct.fields[i]->field->str);
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
	if (node == NULL) {
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
		printf("Value: ");
		printToken(node->literalTok);
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
		for (usize i = 0; i < veclen(node->block); i++) {
			printNode(node->block[i], indent + 1);
		}

		return;

	case NODE_FUNC:
		// if (node->funcDef.receiver) {
		// 	printf("Receiver: ");
		// 	printType(node->funcDef.receiver);
		// 	printf(" ");
		// }
		printf("Name: %s\n", node->funcDef.ident->str);
		printNode(node->funcDef.body, indent + 1);
		return;

	case NODE_CALL:
			printf("Callee: %s\n", node->call.callee->identTok->str);
			for (usize i = 0; i < veclen(node->call.args); i++){
				printNode(node->call.args[i], indent + 1);
			}
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

static bool isValidToken(ZParser *parser, ZTokenType *validTokens, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (check(parser, validTokens[i])) return true;
	}
	return false;
}

static ZNode *wrapNode(ZParser *parser, ParseFunction parse) {
	usize saved = parser->current;
	ZNode *res = parse(parser);
	if (!res) parser->current = saved;
	return res;
}

static ZType *wrapType(ZParser *parser, ZType *(*parse)(ZParser *)) {
	usize saved = parser->current;
	ZType *res = parse(parser);
	if (!res) parser->current = saved;
	return res;
}

static ZNode *parseGenericBinary(ZParser *parser,
																ParseFunction getChild,
																ZTokenType *validTokens,
																size_t validTokensLen) {
	ZNode *node = NULL;

	ZNode *left = wrapNode(parser, getChild);

	ensure(left);

	while (canPeek(parser) &&
					isValidToken(parser, validTokens, validTokensLen)) {
		node = makenode(NODE_BINARY);
		ZToken *op = consume(parser);
		
		ZNode *right = wrapNode(parser, getChild);

		if (!right) break;


		node->binary.op = op;
		node->binary.left = left;
		node->binary.right = right;
		left = node;
	}

	return node ? node : left;
}

static ZNode *parsePrimary(ZParser *parser) {
	if (!canPeek(parser)) {
		return NULL;
	}

	if (check(parser, TOK_LPAREN)) {
		consume(parser);
		ZNode *node = parse(parser);
		expect(parser, TOK_RPAREN);
		return node;
	}
	if (check(parser, TOK_IDENT)) {
		ZNode *node = makenode(NODE_IDENTIFIER);
		node->identTok = consume(parser);
		return node;
	} else if (check(parser, TOK_INT_LIT)) {
		ZNode *node = makenode(NODE_LITERAL);
		node->literalTok = consume(parser);
		return node;
	}

	return NULL;
}

static ZNode *parseUnary(ZParser *parser) {
	ensure(canPeek(parser));


	if (check(parser, TOK_NOT) || check(parser, TOK_MINUS)) {
		ZNode *node = makenode(NODE_UNARY);
		node->unary.op = consume(parser);
		node->unary.operand = parseUnary(parser);
		if (!node->unary.operand) {
			printf("Error parsing the operand\n");
			return NULL;
		}
		return node;
	}
	ZNode *res = parsePrimary(parser);

	return res;
}

static ZNode *parseFactor(ZParser *parser) {
	ZTokenType valids[] = {TOK_STAR, TOK_DIV};
	return parseGenericBinary(parser, parseUnary, valids, 2);
}

static ZNode *parseTerm(ZParser *parser) {
	ZTokenType valids[] = {TOK_PLUS, TOK_MINUS};
	return parseGenericBinary(parser, parseFactor, valids, 2);
}

static ZNode *parseComparison(ZParser *parser) {
	ZTokenType valids[] = {TOK_LT, TOK_GT, TOK_LTE, TOK_GTE};
	return parseGenericBinary(parser, parseTerm, valids, 4);
}

static ZNode *parseEquality(ZParser *parser) {
	ZTokenType valids[] = {TOK_EQEQ, TOK_NOTEQ};
	return parseGenericBinary(parser, parseComparison, valids, 2);
}

static ZNode *parseLogicalAnd(ZParser *parser) {
	ZTokenType valids[] = {TOK_AND};
	return parseGenericBinary(parser, parseEquality, valids, 1);
}

static ZNode *parseLogicalOr(ZParser *parser) {
	ZTokenType valids[] = {TOK_OR};
	return parseGenericBinary(parser, parseLogicalAnd, valids, 1);
}

static ZNode *parseBinary(ZParser *parser) {
	return parseTerm(parser);
}

static ZType **parseTypeArgs(ZParser *parser) {
	ZType **args = NULL;

	do {
		ZType *curr = parseType(parser);
		if (!curr) return args;

		vecpush(args, curr);
		if (!match(parser, TOK_COMMA)) break;
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

static ZType *parseType(ZParser *parser) {
	ensure(canPeek(parser));
	usize saved = parser->current;

	u8 stars = 0;
	while (match(parser, TOK_STAR)) stars++;

	// After stars expected token is a primitive type
	if (!checkMask(parser, TOK_TYPES_MASK)) {
		parser->current = saved;
		return NULL;
	}

	ZType *base = maketype(Z_TYPE_PRIMITIVE);
	base->token = consume(parser);
	base = applyStarsToType(base, stars);


	if (match(parser, TOK_LPAREN)) {
		// printf("Try to get argument types\n");
		ZType **args = parseTypeArgs(parser);
		expect(parser, TOK_RPAREN);

		ZType *type = maketype(Z_TYPE_FUNCTION);
		type->func.ret = base;
		type->func.args = args;

		return type;
	}

	return base;
}

static ZNode *parseStmt(ZParser *parser) {
	ParseFunction toTry[] = {
		parseIf,
		parseExpr,
		parseWhile,
		parseReturn,
		parseBlock,
	};

	usize saved = parser->current;
	ZNode *node = NULL;
	for (u8 i = 0; i < (u8)(sizeof(toTry) / sizeof(ParseFunction)); i++) {
		node = toTry[i](parser);

		if (node) return node;

		parser->current = saved;
	}
	return NULL;
}

static ZNode *parseBlock(ZParser *parser) {
	ensure(canPeek(parser) && match(parser, TOK_LBRACKET));

	ZNode *block = makenode(NODE_BLOCK);
	ZNode *stmt;
	do {
		stmt = parseStmt(parser);
		if (stmt) vecpush(block->block, stmt);
	} while (stmt);

	ensure(match(parser, TOK_RBRACKET));

	return block;
}

static ZNode *parseStructDecl(ZParser *parser) {
	expect(parser, TOK_STRUCT);
	expect(parser, TOK_IDENT);
	ZToken *name = peekAhead(parser, -1);
	if (!name) return NULL;

	expect(parser, TOK_LBRACKET);

	ZField **fields = NULL;

	while (canPeek(parser) && peek(parser)->type != TOK_RBRACKET) {
		ZType *type = parseType(parser);

		if (!type) {
			return NULL;
		}

		expect(parser, TOK_IDENT);
		ZToken *ident = peekAhead(parser, -1);

		vecpush(fields, makefield(type, ident));
	}

	ZNode *node = makenode(NODE_STRUCT);
	node->structDef.fields = fields;
	node->structDef.ident = name;
	return node;
}

static ZNode *parseExpr(ZParser *parser) {
	return parseBinary(parser);
}

static ZNode *parseReturn(ZParser *parser) {
	ensure(canPeek(parser) && match(parser, TOK_RETURN));
	return parseExpr(parser);
}

static ZNode *parseIf(ZParser *parser) {
	expect(parser, TOK_IF);
	
	ZNode *cond = parseExpr(parser);

	ZNode *body = parseBlock(parser);
	if (!body) return NULL;

	ZNode *node = makenode(NODE_IF);

	if (canPeek(parser) && match(parser, TOK_ELSE)) {
		ZNode *elseBody = parseBlock(parser);
		if (!elseBody) return NULL;
		node->ifStmt.elseBranch = elseBody;
	}

	node->ifStmt.cond = cond;
	node->ifStmt.body = body;
	return node;
}

static ZNode *parseWhile(ZParser *parser) {
	expect(parser, TOK_WHILE);

	ZNode *cond = parseExpr(parser);
	ZNode *body = parseBlock(parser);

	ZNode *node = makenode(NODE_WHILE);
	node->whileStmt.branch = body;
	node->whileStmt.cond = cond;
	return node;
}


static ZNode *parseFuncDecl(ZParser *parser) {
	ZType *ret = wrapType(parser, parseType);
	if (!ret) {
		printf("Function declaration failed to get return type\n");
	}
	if (!ret || !canPeek(parser) || !check(parser, TOK_IDENT)) return NULL;

	ZToken *name = consume(parser);

	printToken(name);
	printf("\n");
	ensure(canPeek(parser) && match(parser, TOK_LPAREN));

	ZField **args = NULL;

	while (canPeek(parser)) {
		ZType *argType = wrapType(parser, parseType);
		if (!argType) {
			return NULL;
		}
		ZToken *argName = consume(parser);
	
		if (!argName) {
			printf("Error parsing argument function\n");
			return NULL;
		}

		ZField *arg = makefield(argType, argName);
		vecpush(args, arg);

		if (match(parser, TOK_RPAREN)) break;

		if (!match(parser, TOK_COMMA)) {
			printf("Expected a comma here, given ");
			printToken(peek(parser));
			printf("\n");
			return NULL;
		}
	}

	ZNode *body = parseBlock(parser);
	if (!body) return NULL;

	ZNode *node = makenode(NODE_FUNC);
	node->funcDef.ret = ret;
	node->funcDef.ident = name;
	node->funcDef.args = args;
	node->funcDef.body = body;

	return node;
}

static ZNode *parse(ZParser *parser) {
	if (!canPeek(parser)) return NULL;

	ZToken *curr = peek(parser);
	
	if (check(parser, TOK_STAR) || checkMask(parser, TOK_TYPES_MASK)) {
		printf("try to parse function declaration\n");
		return parseFuncDecl(parser);
	}

	switch (curr->type) {
		case TOK_STRUCT:
		return parseStructDecl(parser);
		case TOK_IF:
		return parseIf(parser);
		case TOK_WHILE:
		return parseWhile(parser);
		case TOK_INT_LIT:
		return parseBinary(parser);
		default:
			error("Unhandled node type %d", peek(parser)->type);
			return NULL;
		break;
	}
}

ZNode *zparse(ZToken **tokens) {
	ZParser *parser = makeparser(tokens);
	return parse(parser);
}
