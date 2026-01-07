#ifndef Z_DEBUG_H
#define Z_DEBUG_H

#include "zlex.h"
#include "zparse.h"

void printToken(ZToken *);
void printTokens(ZToken **);

void printType(ZType *);
void printNode(ZNode *, u8);

#endif
