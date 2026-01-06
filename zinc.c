#include "zlex.h"
#include "zmem.h"
#include "zparse.h"

#include <stdlib.h>
#include <stdio.h>

char *readfile(const char *filename) {
	FILE *fd = fopen(filename, "r");
	if (!fd) {
		perror("Unable to open file");
		exit(1);
	}

	fseek(fd, 0, SEEK_END);
	long flen = ftell(fd);
	fseek(fd, 0, SEEK_SET);
	char *buff = malloc(flen+1);
	fread(buff, 1, flen, fd);
	return buff;
}

int calculate(ZNode *node) {
	if (!node) return 0;
	if (node->type == NODE_LITERAL) {
		return node->literalTok->integer;
	}
	int left = calculate(node->binary.left);
	int right = calculate(node->binary.right);
	switch(node->binary.op->type) {
	case TOK_PLUS:
		return left + right;
	break;
	case TOK_MINUS:
		return left - right;
	break;
	case TOK_STAR:
		return left * right;
	break;
	case TOK_DIV:
		return left / right;
	break;
	default:
		return 0;
	}
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: %s, <filename>", *argv);
		return 1;
	}
	char *program = readfile(argv[1]);

	allocator.init();

	ZToken **tokens = ztokenize(program);

	ZNode *root = zparse(tokens);

	printNode(root, 0);

	// int res = calculate(root);
	// printf("calculated: %d", res);

	allocator.close();

	return 0;
}
