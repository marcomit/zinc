#include "zparse.h"
#include "zlex.h"
#include "zmem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define expect(l, t)  if (!match(l, t)) return NULL

#define error(fmt, ...) do {												\
	fprintf(stderr, "%s:%d", __FILE__, __LINE__);			\
	fprintf(stderr, fmt, __VA_ARGS__);								\
} while (0)


static ZNode *parse(ZTokens *);

// static void error(char *fmt, ...) {
// 	va_list args;
// 	va_start(args, fmt);
// 	fprintf(stderr, "%s:%d ", __FILE__, __LINE__);
// 	vfprintf(stderr, fmt, args);
// 	va_end(args);
// }

static ZNode *makenode(ZNodeType type) {
	ZNode *self = allocator.alloc(sizeof(ZNode));
	self->type = type;
	return self;
}

static inline bool canPeek(ZTokens *tokens) {
	return (tokens->current < tokens->len);
}

static ZToken *peek(ZTokens *tokens) {
	if (!canPeek(tokens)) {
		error("Iterator tokens out of bound! %d", tokens->current);
		return NULL;
	}
	return tokens->ptr[tokens->current];
}

static ZToken *peekAhead(ZTokens *tokens, int32_t offset) {
	int32_t index = tokens->current + offset;
	if (index < 0 || index >= tokens->len) {
		error("Index out of range %d", index);
	}
	return tokens->ptr[index];
}

static ZToken *consume(ZTokens *tokens) {
	if (!canPeek(tokens)) return NULL;
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

static ZNode *parsePrimary(ZTokens *tokens) {
	if (!canPeek(tokens)) return NULL;
	printf("parse primary\n");
	if (check(tokens, TOK_LPAREN)) {
		consume(tokens);
		ZNode *node = parse(tokens);
		expect(tokens, TOK_RPAREN);
		return node;
	}
	ZNode *node = makenode(NODE_IDENTIFIER);
	if (check(tokens, TOK_IDENT)) {
		node->identTok = consume(tokens);
		return node;
	} else if (check(tokens, TOK_INT_LIT)) {
		printf("literal\n");
		node->type = NODE_LITERAL;
		node->literalTok = consume(tokens);
		return node;
	}
	return NULL;
}

static ZNode *parseUnary(ZTokens *tokens) {
	if (!canPeek(tokens)) return NULL;
	printf("parse unary\n");

	ZNode *node = makenode(NODE_UNARY);
	if (check(tokens, TOK_NOT) || check(tokens, TOK_MINUS)) {
		node->unary.op = consume(tokens);
		node->unary.operand = parseUnary(tokens);
		return node;
	}
	return parsePrimary(tokens);
}

static ZNode *parseFactor(ZTokens *tokens) {
	ZNode *node = NULL;
	printf("parse factor\n");

	ZNode *left = parseUnary(tokens);
	if (!left) return NULL;

	while (canPeek(tokens) && (check(tokens, TOK_STAR) ||
					check(tokens, TOK_DIV))) {
		node = makenode(NODE_BINARY);
		ZToken *op = consume(tokens);
		ZNode *right = parseUnary(tokens);
		if (!right) return NULL;
		node->binary.op = op;
		node->binary.left = left;
		node->binary.right = right;
		left = node;
	}

	if (!node) return left;

	return node;
}

static ZNode *parseTerm(ZTokens *tokens) {
	ZNode *node = NULL;

	printf("parse term\n");
	ZNode *left = parseFactor(tokens);

	if (!left) return NULL;

	while (canPeek(tokens) && (
					check(tokens, TOK_PLUS) ||
					check(tokens, TOK_MINUS))) {
		node = makenode(NODE_BINARY);
		ZToken *op = consume(tokens);
		
		ZNode *right = parseFactor(tokens);
		if (!right) return NULL;
		node->binary.op = op;
		node->binary.left = left;
		node->binary.right = right;
		left = node;
	}

	if (!node) return left;

	return node;
} 

static ZType *parseType(ZTokens *tokens) {
	return NULL;
}

static ZNode *parseStmt(ZTokens *tokens) {
	return NULL;
}

static ZNode *parseBlock(ZTokens *tokens) {
	ZNode *block = makenode(NODE_BLOCK);

	vec_init(&block->block, 8);
	
	ZNode *stmt;
	do {
		stmt = parseStmt(tokens);
		if (stmt) vec_push(&block->block, stmt);
	} while (stmt);

	return block;
}

static ZField *makefield(ZType *type, ZToken *name) {
	ZField *field = allocator.alloc(sizeof(ZField));
	field->type = type;
	field->field = name;
	return field;
}

static ZNode *parseStructDecl(ZTokens *tokens) {
	expect(tokens, TOK_STRUCT);
	expect(tokens, TOK_IDENT);
	ZToken *name = peekAhead(tokens, -1);
	if (!name) return NULL;

	expect(tokens, TOK_LBRACKET);

	ZFields *fields = allocator.alloc(sizeof(ZFields));
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
	return NULL;
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

static ZNode *parse(ZTokens *tokens) {
	if (!canPeek(tokens)) return NULL;


	switch (peek(tokens)->type) {
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
