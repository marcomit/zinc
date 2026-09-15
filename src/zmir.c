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

        struct {
            u32 align;

            union {
                ZType *alloca;

                struct {
                    ZMirInstruction *val;
                    ZMirInstruction *ptr;
                } store;

                struct {
                    ZType           *type;
                    ZMirInstruction *value;
                } load;

                struct {
                    ZMirInstruction *ptr;
                    ZMirInstruction *val;
                    ZMirInstruction *len;
                } mmset;
            };
        };

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

ZMirInstruction *ZMirBuildConstInt(ZMirBuilder *builder, ZMirInt inttype, i64 value) {
    ZMirInstruction *instr  = makeinstr(builder, ZMirConstInt);
    instr->constint.inttype = inttype;
    instr->constint.value   = value;
    return instr;
}

ZMirInstruction *ZMirBuildAlloca(ZMirBuilder *builder, ZType *type) {
    ZMirInstruction *self   = makeinstr(builder, ZMirAlloca);
    self->align             = 4;
    self->alloca            = type;
    return self;
}

ZMirInstruction *ZMirBuildStore(ZMirBuilder *builder, ZMirInstruction *value, ZMirInstruction *ptr) {
    ZMirInstruction *self   = makeinstr(builder, ZMirStore);
    self->align             = 4;
    self->store.val         = value;
    self->store.ptr         = ptr;
    return self;
}

ZMirInstruction *ZMirBuildLoad(ZMirBuilder *builder, ZType *type, ZMirInstruction *ptr) {
    ZMirInstruction *self   = makeinstr(builder, ZMirLoad2);
    self->align             = 4;
    self->load.type         = type;
    self->load.value        = ptr;
    return self;
}

ZMirInstruction *ZMirBuildZeroed(ZMirBuilder *builder, ZMirInstruction *ptr, ZMirInstruction *val, ZMirInstruction *len, u32 align) {
    ZMirInstruction *self   = makeinstr(builder, ZMirMemSet);
    self->align             = 4;
    self->mmset.ptr         = ptr;
    self->mmset.val         = val;
    self->mmset.len         = len;
    return self;
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
