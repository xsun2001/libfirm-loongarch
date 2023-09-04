/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   This file implements the creation of the achitecture specific firm
 *          opcodes and the coresponding node constructors for the loongarch64
 *          assembler irg.
 */
#include "loongarch64_new_nodes_t.h"

#include "bedump.h"
#include "gen_loongarch64_regalloc_if.h"
#include "ircons_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "irnode_t.h"
#include "irop_t.h"
#include "iropt_t.h"
#include "irprintf.h"
#include "irprog_t.h"
#include "loongarch64_nodes_attr.h"
#include "xmalloc.h"
#include <stdlib.h>

void loongarch64_dump_node(FILE *F, const ir_node *n, dump_reason_t reason) {
    switch (reason) {
    case dump_node_opcode_txt:
        fprintf(F, "%s", get_irn_opname(n));
        break;

    case dump_node_mode_txt:
        break;

    case dump_node_nodeattr_txt:

        /* TODO: dump some attributes which should show up */
        /* in node name in dump (e.g. consts or the like)  */

        break;

    case dump_node_info_txt:
        break;
    }
}

const loongarch64_attr_t *get_loongarch64_attr_const(const ir_node *node) {
    assert(is_loongarch64_irn(node) && "need loongarch64 node to get attributes");
    return (const loongarch64_attr_t *)get_irn_generic_attr_const(node);
}

loongarch64_attr_t *get_loongarch64_attr(ir_node *node) {
    assert(is_loongarch64_irn(node) && "need loongarch64 node to get attributes");
    return (loongarch64_attr_t *)get_irn_generic_attr(node);
}

int loongarch64_attrs_equal(const ir_node *a, const ir_node *b) { return 1; }

const loongarch64_immediate_attr_t *get_loongarch64_immediate_attr_const(const ir_node *node) {
    assert(is_loongarch64_irn(node) && "need loongarch64 node to get attributes");
    return (const loongarch64_immediate_attr_t *)get_irn_generic_attr_const(node);
}

loongarch64_immediate_attr_t *get_loongarch64_immediate_attr(ir_node *node) {
    assert(is_loongarch64_irn(node) && "need loongarch64 node to get attributes");
    return (loongarch64_immediate_attr_t *)get_irn_generic_attr(node);
}

int loongarch64_immediate_attrs_equal(const ir_node *a, const ir_node *b) {
    const loongarch64_immediate_attr_t *attr_a = get_loongarch64_immediate_attr_const(a);
    const loongarch64_immediate_attr_t *attr_b = get_loongarch64_immediate_attr_const(b);
    return attr_a->ent == attr_b->ent && attr_a->val == attr_b->val;
}

const loongarch64_cond_attr_t *get_loongarch64_cond_attr_const(const ir_node *node) {
    assert(is_loongarch64_irn(node) && "need loongarch64 node to get attributes");
    return (const loongarch64_cond_attr_t *)get_irn_generic_attr_const(node);
}

loongarch64_cond_attr_t *get_loongarch64_cond_attr(ir_node *node) {
    assert(is_loongarch64_irn(node) && "need loongarch64 node to get attributes");
    return (loongarch64_cond_attr_t *)get_irn_generic_attr(node);
}

int loongarch64_cond_attrs_equal(const ir_node *a, const ir_node *b) {
    const loongarch64_cond_attr_t *attr_a = get_loongarch64_cond_attr_const(a);
    const loongarch64_cond_attr_t *attr_b = get_loongarch64_cond_attr_const(b);
    return attr_a->cond == attr_b->cond;
}