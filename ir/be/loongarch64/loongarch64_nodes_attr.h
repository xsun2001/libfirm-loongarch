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

typedef struct loongarch64_attr_t loongarch64_attr_t;

struct loongarch64_attr_t {};

typedef struct loongarch64_immediate_attr_t {
    loongarch64_attr_t attr;
    ir_entity *ent;
    int64_t val;
} loongarch64_immediate_attr_t;

#endif
