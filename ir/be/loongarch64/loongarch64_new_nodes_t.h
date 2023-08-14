/*
 * This file is part of libFirm.
 * Copyright (C) 2016 Matthias Braun
 */

/**
 * @file
 * @brief   Internal declarations used by gen_new_nodes.c
 */
#ifndef FIRM_BE_loongarch64_loongarch64_NEW_NODES_T_H
#define FIRM_BE_loongarch64_loongarch64_NEW_NODES_T_H

#include "loongarch64_new_nodes.h"

void loongarch64_dump_node(FILE *F, const ir_node *n, dump_reason_t reason);

void set_loongarch64_value(ir_node *const node, ir_entity *const entity,
                        ir_tarval *const value);

int loongarch64_attrs_equal(const ir_node *a, const ir_node *b);

#endif
