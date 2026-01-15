#include "zparse.h"
#include "zlex.h"
#include "zmem.h"
#include "zdebug.h"
#include "zvec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define ensure(c) if (!(c)) return NULL
#define expect(l, t)  ensure(match(l, t))

typedef struct ZParser {
	ZToken **tokens;
	u64 current;

	/*
	 * Used to track temporary errors and find a valid path.
	 */
	usize *errstack;
	u8 depth;

	/* List of visited modules */
	struct {
		char *name;
		ZNode *root;
	} *modules;

	/* Module parsing */
	char *currentModule;
} ZParser;

typedef ZNode *(*ParseFunction)(ZParser *);

static ZType *parseType			(ZParser *);
static ZNode *parse					(ZParser *, char *);
static ZNode *parseIf				(ZParser *);
static ZNode *parseWhile		(ZParser *);
static ZNode *parseReturn		(ZParser *);
static ZNode *parseBlock		(ZParser *);
static ZNode *parseExpr			(ZParser *);
static ZNode *parseVarDecl	(ZParser *);

static ZParser *makeparser(ZToken **tokens) {
	ZParser *self = zalloc(ZParser);
	self->current = 0;
	self->tokens = tokens;
	self->errstack = NULL;
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

static inline bool canPeek(ZParser *parser) {
	return (usize)parser->current < veclen(parser->tokens);
}

static inline ZToken *peek(ZParser *parser) {
	return canPeek(parser) ? parser->tokens[parser->current] : NULL;
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
		while (veclen(parser->errors) > checkpoint) vecpop(parser->errors);
		parser->depth--;
	}
}

static void reportError(ZParser *parser, const char *fmt, ...) {
	ZParserError *error = zalloc(ZParserError);

	va_list args;
	va_start(args, fmt);

	va_list argsCopy;
	va_copy(argsCopy, args);
	int len = vsnprintf(NULL, 0, fmt, argsCopy);
	va_end(argsCopy);

	error->message = allocator.alloc(len + 1);
	vsnprintf(error->message, len + 1, fmt, args);
	va_end(args);

	error->token = peek(parser);
	vecpush(parser->errors, error);
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
	ZNode *parsed = NULL;
	for (usize i = 0; i < len; i++) {
		parsed = wrapNode(parser, pf[i]);
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
					isValidToken(parser, validTokens, validTokensLen)) {
		node = makenode(NODE_BINARY);
		ZToken *op = consume(parser);
		
		ZNode *right = wrapNode(parser, parseRight);

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
		ZNode *node = wrapNode(parser, parseExpr);
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
	if (!isValidToken(parser, valids, 5)) {
		reportError(parser, "Failed to parse unary expression");
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

static ZType **parseTypeArgs(ZParser *parser) {
	ZType **args = NULL;

	do {
		ZType *curr = wrapType(parser, parseType);
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

	bool constant = match(parser, TOK_CONST);

	u8 stars = 0;
	while (match(parser, TOK_STAR)) stars++;

	// After stars expected token is a primitive type or an identifier for structs
	if (!checkMask(parser, TOK_TYPES_MASK) &&
			!check(parser, TOK_IDENT) && 
			!check(parser, TOK_LSBRACKET)) {
		parser->current = saved;
		reportError(parser, "Expected a primitive type, got %s", stoken(peek(parser)));
		return NULL;
	}

	if (match(parser, TOK_LSBRACKET)) {
		ZType *type = wrapType(parser, parseType);
		if (!match(parser, TOK_RSBRACKET)) {
			reportError(parser, "Invalid array type");
		}
		ZType *array = maketype(Z_TYPE_ARRAY);
		array->array.base = applyStarsToType(type, stars);
		return array;
	}

	ZType *base = maketype(Z_TYPE_PRIMITIVE);
	base->token = consume(parser);
	base = applyStarsToType(base, stars);
	base->constant = constant;


	if (match(parser, TOK_LPAREN)) {
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
		parseVarDecl
	};

	return parseOrGrammar(parser, toTry, 6);
}

static ZNode *parseBlock(ZParser *parser) {
	ensure(canPeek(parser) && match(parser, TOK_LBRACKET));

	ZNode *block = makenode(NODE_BLOCK);
	ZNode *stmt = NULL;
	do {
		stmt = parseStmt(parser);
		if (stmt) vecpush(block->block, stmt);
	} while (stmt);

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

	if (check(parser, TOK_IDENT)) name = consume(parser);

	if (!name) {
		reportError(parser, "Expected an identifier after 'struct'");
		return NULL;
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
	node->structDef.ident = name;
	return node;
}

static ZNode *parseExpr(ZParser *parser) {
	return parseBinary(parser);
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

static ZNode *parseWhile(ZParser *parser) {
	expect(parser, TOK_WHILE);

	ZNode *cond = wrapNode(parser, parseExpr);
	ZNode *body = wrapNode(parser, parseBlock);

	ZNode *node = makenode(NODE_WHILE);
	node->whileStmt.branch = body;
	node->whileStmt.cond = cond;
	return node;
}

static ZNode *parseFuncDecl(ZParser *parser) {
	ZType *ret = wrapType(parser, parseType);

	if (!ret || !canPeek(parser) || !check(parser, TOK_IDENT)) return NULL;

	ZToken *name = consume(parser);

	ensure(canPeek(parser) && match(parser, TOK_LPAREN));

	ZNode **args = NULL;

	while (canPeek(parser) && !match(parser, TOK_RPAREN)) {
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

	ensure(body);

	ZNode *node = makenode(NODE_FUNC);
	node->funcDef.ret = ret;
	node->funcDef.ident = name;
	node->funcDef.args = args;
	node->funcDef.body = body;
	node->funcDef.receiver = receiver;

	return node;
}

static ZNode *parseVarDef(ZParser *parser) {
	ZType *type = wrapType(parser, parseType);

	ensure(type && check(parser, TOK_IDENT));

	ZToken *ident = consume(parser);

	ZNode *node = makenode(NODE_VAR_DECL);
	node->varDecl.ident = ident;
	node->varDecl.type = type;
	node->varDecl.rvalue = NULL;
	return node;
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

static ZNode *getModuleByName(ZParser *parser, ZToken *name) {
	for (usize i = 0; i < veclen(parser->modules); i++) {
		if (strcmp(parser->modules[i].name, name->str) == 0) {
			reportError(parser, "Duplicate import\n");
			return parser->modules[i].root;
		}
	}

	ZToken **tokens = ztokenize(name->str);

	ZNode *node = makenode(NODE_MODULE);

	node->module.root = zparse(tokens, name->str);
	node->module.name = name;

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

	expect(parser, TOK_EQ);

	ZType *type = wrapType(parser, parseType);
	
	ensure(type);

	ZNode *node = makenode(NODE_TYPEDEF);
	node->typeDef.alias = alias;
	node->typeDef.type  = type;
	return node;
}

static ZNode *parse(ZParser *parser, char *module) {
	// for (usize i = 0; i < veclen(parser->modules); i++) {
	// 	if (!strcmp(parser->modules[i], module)) {
	// 		return NULL;
	// 	}
	// }

	parser->currentModule = module;
	
	// vecpush(parser->modules, NULL);

	ParseFunction pf[] = {
		parseImport,
		parseTypedef,
		parseFuncDecl,
		parseStructDecl,
		parseVarDef,
		parseVarDecl,
	};
	usize len = sizeof(pf) / sizeof(pf[0]);
	return parseOrGrammar(parser, pf, len);
}

static ZNode *parseProgram(ZParser *parser) {
	ZNode *root = makenode(NODE_PROGRAM);
	root->program = NULL;

	parser->modules = NULL;

	while (canPeek(parser)) {
		ZNode *child = parse(parser, parser->currentModule);
		if (!child) break;
		vecpush(root->program, child);
	}
	return root;
}

ZNode *zparse(ZToken **tokens, char *module) {
	ZParser *parser = makeparser(tokens);
	
	ZNode *root = parseProgram(parser);
	if (canPeek(parser)) {
		reportError(parser, "Compilation failed");
	}

	printf("\n====== Errors ======\n");
	for (usize i = 0; i < veclen(parser->errors); i++) {
		ZParserError *err = parser->errors[i];
		printf("%zu,%zu: %s\n", err->token->row, err->token->col, err->message);
	}
	printf("====== End errors ======\n\n");

	return root;
}
