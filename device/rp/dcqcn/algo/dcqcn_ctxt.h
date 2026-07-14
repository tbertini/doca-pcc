/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef DCQCN_CTXT_H_
#define DCQCN_CTXT_H_

/*
 * DCQCN per-flow context structure (48 bytes).
 *
 * Implements the per-flow state required by the DCQCN algorithm
 * (Zhu et al., "Congestion Control for Large-Scale RDMA Deployments", SIGCOMM 2015).
 *
 * RC (cur_rate) and RT (target_rate) are in FXP20 format (1<<20 = 100% line rate).
 * Alpha (α) is in FXP16 format (1<<16 = 1.0).
 * Timestamps are in nanoseconds (from doca_pcc_dev_get_timestamp).
 */
typedef struct {
	uint32_t cur_rate;		/* RC: current sending rate (FXP20) */
	uint32_t target_rate;		/* RT: target rate for recovery (FXP20) */
	uint32_t alpha;			/* α: congestion severity EWMA (FXP16, 1<<16 = 1.0) */
	uint32_t last_alpha_ts;		/* Timestamp (ns) of last alpha timer fire */
	uint32_t last_timer_ts;		/* Timestamp (ns) of last rate-increase timer fire */
	uint32_t bytes_accumulated;	/* Bytes accumulated since last byte counter fire */
	uint16_t t_count;		/* Timer fire count since last CNP (T in paper Fig. 7) */
	uint16_t bc_count;		/* Byte counter fire count since last CNP (BC in paper Fig. 7) */
	uint16_t last_sent_32bytes;	/* Last cumulative TX 32-byte counter value (for delta computation) */
	uint8_t reserved0[2];		/* Padding to 4-byte alignment */
	uint32_t reserved[4];		/* Reserved for future use, pad to 48 bytes total */
} cc_ctxt_dcqcn_t;

#endif /* DCQCN_CTXT_H_ */
