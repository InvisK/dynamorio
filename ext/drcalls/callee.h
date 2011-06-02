/* *******************************************************************************
 * Copyright (c) 2011 Massachusetts Institute of Technology  All rights reserved.
 * *******************************************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of MIT nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL MIT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Header for callee.c, which is responsible for callee decoding/analysis. */

#ifndef DRCALLS_CALLEE_H
#define DRCALLS_CALLEE_H

#include "core_compat.h"

/* Analysis results for functions called. */
typedef struct _callee_info_t {
    bool bailout;             /* if we bail out on function analysis */
    uint num_args;            /* number of args the callee takes */
    int num_instrs;           /* total number of instructions of a function */
    app_pc start;             /* entry point of a function  */
    int num_xmms_used;        /* number of xmms used by callee */
    bool xmm_used[NUM_XMM_REGS];  /* xmm/ymm registers usage */
    int num_regs_used;        /* number of regs used by callee */
    bool reg_used[NUM_GP_REGS];   /* general purpose registers usage */
    bool callee_save_regs[NUM_GP_REGS]; /* callee-save registers */
    int frame_size;           /* size of stack frame adjustment for locals */
    bool has_locals;          /* if reference local via statck */
    bool xbp_is_fp;           /* if xbp is used as frame pointer */
    bool opt_inline;          /* can be inlined or not */
    bool opt_partial;         /* can be partially inlined */
    bool write_aflags;        /* if the function changes aflags */
    bool read_aflags;         /* if the function reads aflags from caller */
    bool tls_used;            /* application accesses TLS (errno, etc.) */
    bool has_cti;             /* true if callee has any control flow */
    bool is_leaf;             /* true if all control flow is within callee */
    bool stack_complex;       /* true if the stack usage is complicated */
    instrlist_t *ilist;       /* instruction list of function for inline. */
    instr_t *partial_label;   /* label of slowpath entry */
    app_pc partial_pc;      /* pc of slowpath entry */
    uint framesize;           /* size of the frame on dstack */
} callee_info_t;

void callee_info_init(callee_info_t *ci);
void callee_info_table_init(void);
void callee_info_table_destroy(void);
callee_info_t *callee_info_analyze(void *dc, void *callee, uint num_args);

/* TODO(rnk): Create and move to instr_builder.h or utils.h. */
void remove_and_destroy(void *dc, instrlist_t *ilist, instr_t *instr);

#endif /* DRCALLS_CALLEE_H */