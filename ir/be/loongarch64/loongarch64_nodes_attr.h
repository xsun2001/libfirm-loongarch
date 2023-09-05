/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   attributes attached to all loongarch64 nodes
 */
#ifndef FIRM_BE_loongarch64_loongarch64_NODES_ATTR_H
#define FIRM_BE_loongarch64_loongarch64_NODES_ATTR_H

#include "firm_types.h"
#include "stdint.h"

typedef struct loongarch64_attr_t {
} loongarch64_attr_t;

loongarch64_attr_t       *get_loongarch64_attr(ir_node *node);
const loongarch64_attr_t *get_loongarch64_attr_const(const ir_node *node);

typedef struct loongarch64_immediate_attr_t {
    loongarch64_attr_t attr;
    ir_entity         *ent;
    int64_t            val;
} loongarch64_immediate_attr_t;

loongarch64_immediate_attr_t       *get_loongarch64_immediate_attr(ir_node *node);
const loongarch64_immediate_attr_t *get_loongarch64_immediate_attr_const(const ir_node *node);

typedef enum loongarch64_cond_t {
    loongarch64_beq,
    loongarch64_bne,
    loongarch64_blt,
    loongarch64_bge,
    loongarch64_bltu,
    loongarch64_bgeu,
    loongarch64_beqz,
    loongarch64_bnez,
    loongarch64_invalid,
} loongarch64_cond_t;

static inline loongarch64_cond_t loongarch64_negate_cond(loongarch64_cond_t const c) {
    // Flip the lowest bit
    return (loongarch64_cond_t)(c ^ 1U);
}

static char *loongarch64_cond_inst_name(loongarch64_cond_t cond) {
    switch (cond) {
    case loongarch64_beq:
        return "beq";
    case loongarch64_bne:
        return "bne";
    case loongarch64_blt:
        return "blt";
    case loongarch64_bge:
        return "bge";
    case loongarch64_bltu:
        return "bltu";
    case loongarch64_bgeu:
        return "bgeu";
    case loongarch64_beqz:
        return "beqz";
    case loongarch64_bnez:
        return "bnez";
    default:
        return "invalid";
    }
}

typedef struct loongarch64_cond_attr_t {
    loongarch64_attr_t attr;
    loongarch64_cond_t cond;
} loongarch64_cond_attr_t;

loongarch64_cond_attr_t       *get_loongarch64_cond_attr(ir_node *node);
const loongarch64_cond_attr_t *get_loongarch64_cond_attr_const(const ir_node *node);

#endif
