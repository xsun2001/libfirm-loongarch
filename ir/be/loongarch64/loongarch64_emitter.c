/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   emit assembler for a backend graph
 */
#include "loongarch64_emitter.h"

#include "be_t.h"
#include "bearch.h"
#include "beblocksched.h"
#include "bediagnostic.h"
#include "beemithlp.h"
#include "beemitter.h"
#include "begnuas.h"
#include "benode.h"
#include "besched.h"
#include "gen_loongarch64_emitter.h"
#include "gen_loongarch64_regalloc_if.h"
#include "irgwalk.h"
#include "loongarch64_new_nodes.h"
#include "panic.h"
#include "util.h"

static void loongarch64_emit_immediate(const ir_node *node) {
    loongarch64_immediate_attr_t const *const attr = get_loongarch64_immediate_attr_const(node);
    ir_entity *const                          ent  = attr->ent;
    int64_t                                   val  = attr->val;
    if (ent) {
        be_emit_irprintf("&%s", get_entity_ld_name(ent));
        be_emit_irprintf("%d", val);
    } else {
        be_emit_irprintf("%d", val);
    }
}

static void emit_register(const arch_register_t *reg) {
    be_emit_char('$');
    be_emit_string(reg->name);
}

static void loongarch64_emit_source_register(const ir_node *node, int pos) {
    const arch_register_t *reg = arch_get_irn_register_in(node, pos);
    emit_register(reg);
}

static void loongarch64_emit_dest_register(const ir_node *node, int pos) {
    const arch_register_t *reg = arch_get_irn_register_out(node, pos);
    emit_register(reg);
}

void loongarch64_emitf(const ir_node *node, const char *format, ...) {
    BE_EMITF(node, format, ap, false) {
        switch (*format++) {
        case 'S': {
            if (!is_digit(*format))
                goto unknown;
            unsigned const pos = *format++ - '0';
            loongarch64_emit_source_register(node, pos);
            break;
        }

        case 'D': {
            if (!is_digit(*format))
                goto unknown;
            unsigned const pos = *format++ - '0';
            loongarch64_emit_dest_register(node, pos);
            break;
        }

        case 'I': {
            loongarch64_emit_immediate(node);
            break;
        }

        case 'X': {
            int num = va_arg(ap, int);
            be_emit_irprintf("%X", num);
            break;
        }

        case 'A': {
            loongarch64_emit_source_register(node, 1);
            be_emit_char(',');
            be_emit_char('\t');
            loongarch64_emit_immediate(node);
            break;
        }

        case 'G': {
            loongarch64_immediate_attr_t const *const attr = get_loongarch64_immediate_attr_const(node);
            ir_entity *const                          ent  = attr->ent;
            if (ent) {
                be_gas_emit_entity(ent);
            } else {
                panic("no entity for global address");
            }
            break;
        }

        case 'B': {
            loongarch64_cond_t const cond = va_arg(ap, loongarch64_cond_t);
            be_emit_string(loongarch64_cond_inst_name(cond));
            break;
        }

        default:
        unknown:
            panic("unknown format conversion");
        }
    }
}

static void emit_jmp(ir_node const *const node, ir_node const *const target) {
    BE_EMIT_JMP(loongarch64, node, "b", target) { loongarch64_emitf(NULL, "nop"); }
}

static void emit_loongarch64_b(const ir_node *node) { emit_jmp(node, node); }

static void emit_loongarch64_b_cond(const ir_node *node) {
    loongarch64_cond_t const     cond  = get_loongarch64_cond_attr_const(node)->cond;
    be_cond_branch_projs_t const projs = be_get_cond_branch_projs(node);
    char const *const            fmt =
        cond == loongarch64_beqz || cond == loongarch64_bnez ? "%B\t%S0,\t%L" : "%B\t%S0,\t%S1,\t%L";

    if (be_is_fallthrough(projs.t)) {
        loongarch64_emitf(node, fmt, loongarch64_negate_cond(cond), projs.f);
    } else {
        loongarch64_emitf(node, fmt, cond, projs.t);
        emit_jmp(node, projs.f);
    }
}

static void emit_be_Copy(ir_node const *const node) {
    ir_node *const               op  = be_get_Copy_op(node);
    arch_register_t const *const in  = arch_get_irn_register(op);
    arch_register_t const *const out = arch_get_irn_register(node);
    if (in == out)
        return;

    loongarch64_emitf(node, "ori\t%D0,\t%S0,\t0");
}

static void emit_be_IncSP(const ir_node *node) {
    int offset = be_get_IncSP_offset(node);
    if (offset != 0) {
        loongarch64_emitf(node, "addi.d\t%D0,\t%S0,\t%d", -offset);
    }
}

static void emit_be_Perm(ir_node const *const node) {
    arch_register_t const *const out = arch_get_irn_register_out(node, 0);
    if (out->cls == &loongarch64_reg_classes[CLASS_loongarch64_gp]) {
        loongarch64_emitf(node, "xor\t%D0,\t%D0,\t%D1\n"
                                "xor\t%D1,\t%D0,\t%D1\n"
                                "xor\t%D0,\t%D0,\t%D1");
    } else {
        panic("unexpected register class");
    }
}

static void loongarch64_register_emitters(void) {
    be_init_emitters();
    loongarch64_register_spec_emitters();

    be_set_emitter(op_be_Copy, emit_be_Copy);
    be_set_emitter(op_be_IncSP, emit_be_IncSP);
    be_set_emitter(op_be_Perm, emit_be_Perm);

    be_set_emitter(op_loongarch64_b, emit_loongarch64_b);
    be_set_emitter(op_loongarch64_b_cond, emit_loongarch64_b_cond);
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void loongarch64_emit_block(ir_node *block) {
    be_gas_begin_block(block);

    sched_foreach(block, node) { be_emit_node(node); }
}

void loongarch64_emit_function(ir_graph *irg) {
    /* register all emitter functions */
    loongarch64_register_emitters();

    /* create the block schedule */
    ir_node **block_schedule = be_create_block_schedule(irg);

    /* emit assembler prolog */
    ir_entity *entity = get_irg_entity(irg);
    be_gas_emit_function_prolog(entity, 4, NULL);

    /* populate jump link fields with their destinations */
    ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);

    be_emit_init_cf_links(block_schedule);

    for (size_t i = 0, n = ARR_LEN(block_schedule); i < n; ++i) {
        ir_node *block = block_schedule[i];
        loongarch64_emit_block(block);
    }
    ir_free_resources(irg, IR_RESOURCE_IRN_LINK);

    be_gas_emit_function_epilog(entity);
}
