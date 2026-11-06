#ifndef ZLLVM_H
#define ZLLVM_H

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Error.h>
#include <llvm-c/Linker.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include "zmir.h"

/* Translates one ZMIR module into a new LLVM module owned by `ctx`.
 * Returns NULL when there is nothing to translate. */
LLVMModuleRef zmir2llvm(LLVMContextRef ctx, ZMIRModuleRef mod);

#endif
