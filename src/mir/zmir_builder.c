#include "zmir/builder.h"

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
