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

/*
 * TIMELY per-flow context: the state Algorithm 1 carries between events.
 *
 * Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter",
 * SIGCOMM 2015 (Algorithm 1, paper Section 4.3).
 */

#ifndef TIMELY_CTXT_H_
#define TIMELY_CTXT_H_

/* 40 bytes, within the 48-byte per-flow context cap. */
typedef struct {
	uint32_t cur_rate;		/* R(t): current sending rate (FXP20) */
	uint32_t prev_rtt;		/* RTT of the previous completion event (ns) */
	int32_t  rtt_diff;		/* Smoothed RTT difference EWMA (signed, ns) */
	uint16_t neg_gradient_count;	/* Consecutive strictly negative gradients (HAI trigger) */
	uint16_t reserved0;		/* Padding to 4-byte alignment */
	/*
	 * RTT measurement state machine: results->rtt_req=1 only schedules a probe,
	 * which is sent on a later TX event, so a dropped response must be detected
	 * and retried rather than waited on.
	 */
	uint32_t probe_start_ts;		/* Timestamp (ns) when RTT_REQ was last sent on a TX event */
	uint16_t probe_in_flight;		/* 1 if a probe has been sent and we're waiting for the response */
	uint16_t abort_cnt;		/* Number of consecutive abort/retry rounds */
	uint16_t tx_since_probe_req;	/* Counts TX events between rtt_req=1 and RTT_REQ_SENT flag */
	uint16_t reserved1;		/* Padding */
	uint32_t reserved[3];		/* Reserved for future use */
} cc_ctxt_timely_t;

#endif /* TIMELY_CTXT_H_ */
