#ifndef ZVISIT_H
#define ZVISIT_H

#include "zinc.h"

typedef void *ZClientData;
typedef enum {
    Z_CONTINUE,
    Z_BREAK,
    Z_STOP
} ZVisitorState;

typedef struct {
    ZVisitorState (*visit)(ZNode *, ZClientData);
    ZClientData clientData;
} ZVisitor;

bool visitNode(ZVisitor *, ZNode *);

#endif //! ZVISIT_H
