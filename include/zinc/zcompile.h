#ifndef ZCOMPILE_H
#define ZCOMPILE_H

#include "zforward.h"
#include "ztype.h"
#include "znode.h"
#include "zstate.h"

void zcompile(ZState *, const char *);
usize typeSize(ZState *, ZType *);
void validate(ZState *, ZNode *);
bool initTargetMachine(ZState *);
void disposeTargetMachine(ZState *);

#endif // ZCOMPILE_H
