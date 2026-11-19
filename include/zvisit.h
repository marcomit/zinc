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

typedef struct {
    ZVisitorState (*visit)(ZType *, ZClientData);
    ZClientData clientData;
} ZTypeVisitor;

bool visitNode(ZVisitor *, ZNode *);
bool visitType(ZTypeVisitor *, ZType *);

#endif //! ZVISIT_H
