#include <stdlib.h>
#include <stdio.h>
#include "zlex.h"
#include "zmem.h"

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

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: %s, <filename>", *argv);
		return 1;
	}
	char *program = readfile(argv[1]);
	printf("PROGRAM:\n%s\n", program);


	allocator.init();

	ZTokens *tokens = ztokenize(program);

	allocator.close();
	return 0;
}
