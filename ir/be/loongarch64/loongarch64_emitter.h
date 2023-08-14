/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    declarations for emit functions
 */
#ifndef FIRM_BE_loongarch64_loongarch64_EMITTER_H
#define FIRM_BE_loongarch64_loongarch64_EMITTER_H

#include "firm_types.h"

/**
 * emit assembler instructions with format string. Automatically indents
 * instructions and adds debug comments at the end (in verbose-asm mode).
 * Format specifiers:
 *
 * fmt  parameter               output
 * ---- ----------------------  ---------------------------------------------
 * %r   const arch_register_t*  register
 * %Sx  <node>                  source register x
 * %Dx  <node>                  destination register x
 * %O   <node>                  shifter operand
 * %I   <node>                  immediate
 */
void loongarch64_emitf(const ir_node *node, const char *format, ...);

void loongarch64_emit_function(ir_graph *irg);

#endif
