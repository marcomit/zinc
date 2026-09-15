#include "zmir.h"

typedef struct ZMirInstruction ZMirInstruction;

typedef enum {
    #define INSTR(builder, category, emitted) ZMir##builder,
    #define ZMIR_INSTRUCTIONS

    #include "zllvm.def"

    #undef ZMIR_INSTRUCTIONS
    #undef INSTR
} ZMirInstructionType;

typedef enum {
    #define ICMP(builder, category) ZMirInt##builder,
    #define ZMIR_ICMP

    #include "zllvm.def"

    #undef ZMIR_ICMP
    #undef ICMP
} ZMirIntPredicate;

typedef enum {
    #define FCMP(builder, category) ZMirFloat##builder,
    #define ZMIR_FCMP

    #include "zllvm.def"

    #undef ZMIR_FCMP
    #undef FCMP
} ZMirFloatPredicate;

typedef enum {
    ZMirI8,
    // ZMirU8,
    ZMirI16,
    // ZMirU16,
    ZMirI32,
    // ZMirU32,
    ZMirI64,
    // ZMirU64,
} ZMirInt;

struct ZMirInstruction {
    ZMirInstructionType type;
    union {
        struct {
            ZMirIntPredicate predicate;
            ZMirOpaque *left;
            ZMirOpaque *right;
        } icmp;

        struct {
            ZMirFloatPredicate predicate;
            ZMirOpaque *left;
            ZMirOpaque *right;
        } fcmp;

        struct {
            ZMirInstruction *left;
            ZMirInstruction *right;
        } binary;

        struct {
            ZMirInt inttype;
            u64 value;
        } constint;
    };
};

typedef struct ZMirCursor {
    ZMirOpaque      *currentBlock;
    ZMirInstruction **instructions;
} ZMirCursor;

struct ZMirBuilder {
    ZState      *state;
    ZMirCursor  *cursor;
    ZMirOpaque  **instructions;
};


static ZMirInstruction *makeinstr(ZMirBuilder *builder, ZMirInstructionType type) {
    ZMirInstruction *self   = zalloc(ZMirInstruction);
    self->type              = type;
    return self;
}

ZMirInstruction *ZMirConstInt(ZMirBuilder *builder, ZMirInt inttype) {
    return makeinstr(builder, );
}

ZMirInstruction *ZMirBuildAdd(ZMirBuilder *builder, ZMirInstruction *left, ZMirInstruction *right) {
    ZMirInstruction *add    = makeinstr(builder, ZMirAdd);
    add->binary.left        = left;
    add->binary.right       = right;
    vecpush(builder->cursor->instructions, add);
    return add;
}

ZMirInstruction *ZMirBuildSub(ZMirBuilder *builder, ZMirInstruction *left, ZMirInstruction *right) {
    ZMirInstruction *add    = makeinstr(builder, ZMirAdd);
    add->binary.left        = left;
    add->binary.right       = right;
    vecpush(builder->cursor->instructions, add);
    return add;
}
