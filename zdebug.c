#include "zdebug.h"
#include "zparse.h"

#define indent(t) for (u8 i = 0; i < (t); i++) printf("  ");

char *stoken(ZToken *token) {
	char *tok = allocator.alloc(32);
	bool istype = token->type & TOK_TYPES_MASK;

	if (istype) {
		sprintf(tok, "type(");
	}

	
	switch(token->type) {
		case TOK_INT_LIT:
			sprintf(tok, "int(%llu)", token->integer);
			break;
		case TOK_STR_LIT:
			sprintf(tok, "string(%s)", token->str);
			break;
		case TOK_BOOL_LIT:
			sprintf(tok, "bool(%s)", token->boolean ? "true" : "false");
			break;
		case TOK_IDENT:
			sprintf(tok, "ident(%s)", token->str);
			break;
		#define DEF(id, str, _) case id: sprintf(tok, str); break;

		#define TOK_FLOWS
		#define TOK_TYPES
		#define TOK_SYMBOLS

		#include "ztok.h"

		#undef TOK_SYMBOLS
		#undef TOK_TYPES
		#undef TOK_FLOWS

		#undef DEF
	}

	return tok;
}

void printToken(ZToken *token) {
	char *tok = stoken(token);
	printf("%s", tok);
}


void printTokens(ZToken **tokens) {
	printf("==== Tokens: %zu ====\n", veclen(tokens));
	for (usize i = 0; i < veclen(tokens); i++) {
		printToken(tokens[i]);

		if (i > 0 && tokens[i-1]->row != tokens[i]->row) {
			printf("\n");
		} else {
			printf(" ");
		}

	}
	printf("\n==== End tokens ====\n");
}

void printType(ZType *type) {
	if (!type) {
		printf("unknown");
		return;
	}

	if (type->constant) printf("const ");

	switch(type->kind) {
	case Z_TYPE_POINTER:
		// printf("*");
		printf("pointer of ");
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
			ZNode *field = type->strct.fields[i];
			printType(field->field.type);
			printf("%s\n", field->field.identifier->str);
		}
		printf("}");
		break;
	case Z_TYPE_ARRAY:
		// printf("[");
		printf("array of ");
		printType(type->array.base);
		// printf("]");
		break;
	}
}

void printNode(ZNode *node, u8 depth) {
	if (node == NULL) {
		printf("unknown");
		return;
	}

	// Helper to print indentation
	indent(depth);

	printf("[%s] ", (char*[]){
			"BLOCK", "IF", "WHILE", "RETURN", "VAR_DECL", "ASSIGN", 
			"BINARY", "UNARY", "CALL", "FUNC", "LITERAL", "IDENTIFIER", 
			"CAST", "STRUCT", "SUBSCRIPT", "MEMBER", "MODULE", "PROGRAM",
			"UNION", "FIELD", "TYPEDEF"
	}[node->type]);

	depth++;
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
		printNode(node->binary.left, depth);
		printNode(node->binary.right, depth);
		return; // Return early to avoid the double newline

	case NODE_VAR_DECL:
		printf("Var: %s Type: ", node->varDecl.ident->str);
		printType(node->varDecl.type);
		if (node->varDecl.rvalue) {
			printf("\n");
			printNode(node->varDecl.rvalue, depth);
		}
		break;

	case NODE_BLOCK:
		printf(" %zu\n", veclen(node->block));
		for (usize i = 0; i < veclen(node->block); i++) {
			printNode(node->block[i], depth);
		}

		return;

	case NODE_FUNC:
		if (node->funcDef.receiver) {
			printf("Receiver: ");
			printType(node->funcDef.receiver->field.type);
			printf(" ");
			printToken(node->funcDef.receiver->field.identifier);
			printf(" ");
		}
		printf("Name: %s, Type: ", node->funcDef.ident->str);
		printType(node->funcDef.ret);
		printf("\n");
		printNode(node->funcDef.body, depth);
		return;

	case NODE_CALL:
		printf("Callee: %s\n", node->call.callee->identTok->str);
		for (usize i = 0; i < veclen(node->call.args); i++){
			printNode(node->call.args[i], depth);
		}
		return;

	case NODE_RETURN:
		printf("\n");
		if (node->returnStmt.expr) printNode(node->returnStmt.expr, depth);
		return;

	case NODE_IF:
		printf("Cond: \n");
		printNode(node->ifStmt.cond, depth);
		printNode(node->ifStmt.body, depth);
		if (node->ifStmt.elseBranch) {
			indent(depth - 1);
			printf("[ELSE]\n");
			printNode(node->ifStmt.elseBranch, depth);
		}
		break;
	case NODE_WHILE:
		printf("Cond: \n");
		printNode(node->whileStmt.cond, depth);
		printNode(node->whileStmt.branch, depth);

	case NODE_PROGRAM:
		printf("\n");
		for (usize i = 0; i < veclen(node->program); i++) {
			printNode(node->program[i], depth);
		}
		break;
	case NODE_STRUCT:
		printf("%s\n", node->structDef.ident->str);

		for (usize i = 0; i < veclen(node->structDef.fields); i++) {
			ZNode *field = node->structDef.fields[i];
			indent(depth);
			printType(field->field.type);
			printf(" %s\n", field->field.identifier->str);
		}
		break;
	case NODE_UNARY:
		printf("Op: %s\n", stoken(node->unary.operat));
		printNode(node->unary.operand, depth);
		break;
	case NODE_MODULE:
		printf("Name: %s\n", stoken(node->module.name));
		printNode(node->module.root, depth);
		break;
	
	case NODE_MEMBER:
		printf("Field: %s\n", node->memberAccess.field->str);
		printNode(node->memberAccess.object, depth);
		break;
	case NODE_TYPEDEF:
		printf(" %s alias for ", node->typeDef.alias->str);
		printType(node->typeDef.type);
		break;
	// Add cases for WHILE, MEMBER, etc., following the same pattern
	default:
			printf("(details not implemented in printer for node %d)", node->type);
			break;
	}
	printf("\n");
}


