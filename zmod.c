#include "zmod.h"

#include <stdio.h>
#include <stdarg.h>

ZState *makestate(char *filename) {
	ZState *self = zalloc(ZState);

	self->currentPhase 			= Z_LEXICAL;
	self->filename 					= filename;
	self->errors 						= NULL;
	self->verbose			 			= false;
	self->pathFiles					= NULL;
	self->visitedFiles			= NULL;
	self->optimizationLevel = 0;

	return self;
}

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

ZLog *vmakelog(ZLogLevel level,
               char *filename,
               ZToken *tok,
               const char *fmt,
               va_list args) {
	ZLog *log = zalloc(ZLog);

	log->filename = filename;
	log->level = level;
	log->token = tok;

	va_list args_copy;
	va_copy(args_copy, args);

	int len = vsnprintf(NULL, 0, fmt, args_copy);
	va_end(args_copy);

	log->message = allocator.alloc((size_t)len + 1);
	if (log->message) {
		vsnprintf(log->message, (size_t)len + 1, fmt, args);
	}

	return log;
}



void error(ZState *state, ZToken *tok, char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	ZLog *log = vmakelog(Z_ERROR, state->filename, tok, fmt, args); 
	vecpush(state->errors, log);
	
	va_end(args);
}

void warning(ZState *state, ZToken *tok, char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	ZLog *log = vmakelog(Z_WARNING, state->filename, tok, fmt, args); 
	vecpush(state->errors, log);

	va_end(args);
}

void info(ZState *state, ZToken *tok, char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	ZLog *log = vmakelog(Z_INFO, state->filename, tok, fmt, args); 
	vecpush(state->errors, log);

	va_end(args);
}

void visit(ZState *state, char *filename) {
	vecpush(state->visitedFiles, 	filename);
	vecpush(state->pathFiles, 		filename);
	state->filename = filename;
}

void undoVisit(ZState *state) {
	char *filename = vecpop(state->pathFiles);
	state->filename = filename;
}
