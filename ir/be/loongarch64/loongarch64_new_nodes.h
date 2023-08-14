/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   Function prototypes for the assembler ir node constructors.
 */
#ifndef FIRM_BE_loongarch64_loongarch64_NEW_NODES_H
#define FIRM_BE_loongarch64_loongarch64_NEW_NODES_H

#include "loongarch64_nodes_attr.h"

/**
 * Returns the attributes of an loongarch64 node.
 */
loongarch64_attr_t *get_loongarch64_attr(ir_node *node);

const loongarch64_attr_t *get_loongarch64_attr_const(const ir_node *node);

loongarch64_immediate_attr_t *get_loongarch64_immediate_attr(ir_node *node);

const loongarch64_immediate_attr_t *get_loongarch64_immediate_attr_const(const ir_node *node);

/* Include the generated headers */
#include "gen_loongarch64_new_nodes.h"

#endif
