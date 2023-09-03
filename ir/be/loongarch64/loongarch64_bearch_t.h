/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   declarations for loongarch64 backend -- private header
 */
#ifndef FIRM_BE_loongarch64_loongarch64_BEARCH_T_H
#define FIRM_BE_loongarch64_loongarch64_BEARCH_T_H

static inline bool is_simm12(long const val)
{
	return -2048 <= (int32_t)val && (int32_t)val < 2048;
}

#endif
