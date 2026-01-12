#include "zmod.h"
#include "zmem.h"

#include <stdio.h>

char *readfile(char *filename) {
	FILE *fd = fopen(filename, "r");
	
	if (!fd) {
		perror("open()");
		return NULL;
	}

	fseek(fd, 0, SEEK_END);
	i64 flen = ftell(fd);
	fseek(fd, 0, SEEK_SET);
	char *buff = allocator.alloc(flen + 1);
	fread(buff, flen, 1, fd);

	buff[flen] = 0;

	return buff;
}
