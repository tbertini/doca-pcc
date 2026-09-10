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

#ifndef SMARTT_REPS_CTXT_H_
#define SMARTT_REPS_CTXT_H_

#include <stdint.h>

/*
 * SMaRTT per-flow context structure.
 *
 * Holds the per-flow state of SMaRTT, from Bonato et al., "SMaRTT-REPS: Sender-based
 * Marked Rapidly-adapting Trimmed & Timed Transport with Recycled Entropies",
 * arXiv:2404.01630, 2024. Control variable: cwnd_fxp16 (FXP16 packets).
 *
 * This struct is cast over the BlueField-3 per-flow context slot, which is exactly
 * 48 bytes; overrunning it corrupts adjacent per-flow state.
 */
typedef struct {
	uint32_t cur_rate;        /* FXP20 actuated rate; 0 = new-flow sentinel   [4]  */
	uint32_t cwnd_fxp16;      /* control variable, FXP16 packets              [4]  */
	uint32_t last_rtt_ns;     /* latest fabric RTT from probe; init = brtt    [4]  */
	uint32_t last_cnp_ts;     /* CNP arrival ts for the ECN-hold predicate    [4]  */
	uint32_t qa_epoch_start;  /* QuickAdapt trtt-epoch start ("end" anchor)   [4]  */
	uint32_t last_ack_psn;    /* PSN tracker for delta-PSN acked packets      [4]  */
	uint32_t probe_start_ts;  /* RTT-probe abort machine                     [4]  */
	uint16_t acked_pkts;      /* acked in current trtt epoch (saturating)     [2]  */
	uint16_t packets_to_ignore;     /* bytes_to_ignore, packets                     [2]  */
	uint16_t ignored_pkts;    /* bytes_ignored, packets (saturating)          [2]  */
	uint16_t avg_wtd_fxp16;   /* WTD EWMA, FXP16 capped at 65535 (~1.0)      [2]  */
	uint16_t fast_inc_count;  /* FastIncrease byte counter, packets (sat)     [2]  */
	uint8_t  flags;           /* bit0 trigger_qa  bit1 fi_active  bit2 psn_valid
	                             bit3 qa_epoch_valid  bit4 cnp_seen
	                             bit5 rtt_probe_outstanding                   [1]  */
	uint8_t  abort_cnt;       /* probe abort backoff exponent                 [1]  */
	uint8_t  tx_since_probe_req; /* TX events between req and SENT flag      [1]  */
	uint8_t  reserved8;       /*                                              [1]  */
	uint16_t reserved16;      /*                                              [2]  */
	uint32_t reserved32;      /* pad to exactly 48                            [4]  */
} cc_ctxt_smartt_reps_t;      /* 7*4 + 5*2 + 4*1 + 2 + 4 = 48 bytes */

_Static_assert(sizeof(cc_ctxt_smartt_reps_t) == 48,
	       "cc_ctxt_smartt_reps_t must be exactly 48 bytes (BF3 per-flow ctxt slot)");

/* Flag bit positions for cc_ctxt_smartt_reps_t.flags. */
#define SMARTT_REPS_FLAG_TRIGGER_QA        (1u << 0)
#define SMARTT_REPS_FLAG_FI_ACTIVE         (1u << 1)
#define SMARTT_REPS_FLAG_PSN_VALID         (1u << 2)
#define SMARTT_REPS_FLAG_QA_EPOCH_VALID    (1u << 3)
#define SMARTT_REPS_FLAG_CNP_SEEN          (1u << 4)
#define SMARTT_REPS_FLAG_RTT_PROBE_OUTSTANDING (1u << 5)

#endif /* SMARTT_REPS_CTXT_H_ */
