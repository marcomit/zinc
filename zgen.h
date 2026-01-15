#ifndef ZGEN_H
#define ZGEN_H

#include "base.h"
#include "zparse.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>

typedef struct {
	LLVMContextRef 	*context;
	LLVMModuleRef 	*module;
	LLVMBuilderRef 	*builder;

	/* Current function being generated */
	LLVMValueRef 		*val;
	ZNode 					*currFunc;

	/* Simple Symbol table */
	struct {
		char 					*name;
		LLVMValueRef 	*value;
		ZType 				*type;
	} *locals;
} ZGen;

void zgenerate(ZGen *, char *);

#endif
