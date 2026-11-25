#ifndef ZINC_H
#define ZINC_H

#include <stdatomic.h>
#include <pthread.h>

#include "base.h"
#include "zarena.h"
#include "zvec.h"
#include "zhset.h"
#include "zmap.h"

#include "zconfig.h"
#include "zforward.h"
#include "ztok.h"
#include "zann.h"
#include "znode.h"
#include "ztype.h"
#include "zstate.h"
#include "zsem.h"
#include "zcompile.h"

#define hash(x) _Generic(x,                                                     \
    ZToken*: hashtoken,                                                         \
    ZNode* : hashNode,                                                          \
    ZType* : hashType                                                           \
)(x)

/* Last, so every type zlsp.h needs is already declared. zlsp.h includes this
 * header too: the include guards make either inclusion order work. */
#if Z_LSP
#include "zlsp.h"
#endif

void zparse(ZState *, ZFile *);

#endif // ZINC_H
