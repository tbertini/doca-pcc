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

#ifndef SWIFT_CX8_CTXT_H_
#define SWIFT_CX8_CTXT_H_

#include <stdint.h>

/*
 * Swift per-flow context structure (48 bytes).
 *
 * HARD SIZE CONSTRAINT: the framework's per-flow context slot is
 * mlnx_cc_algo_ctxt_t { uint32_t data[12]; } == 48 bytes (12 dwords) on BlueField-3
 * (doca_pcc_dev_data_structure_le_bf3.h). This struct is cast over that slot, so it
 * MUST be exactly 48 bytes -- TIMELY, DCQCN, and rtt_template are all 48 B. Overrunning
 * it corrupts adjacent per-flow state. Keep the reserved tail sized to hold this at 48.
 *
 * Byte count, field by field:
 *   cur_rate, fcwnd, t_last_decrease, probe_start_ts    4 x 4 = 16
 *   ecwnd, endpoint_delay_ewma                          2 x 4 =  8   (running 24)
 *   retx_cnt, probe_in_flight, abort_cnt,
 *     tx_since_probe_req                                4 x 2 =  8   (running 32)
 *   last_fabric_rtt, last_ack_psn, flags, reserved[1]   4 x 4 = 16   (running 48)
 *
 * Implements the per-flow state for Swift (Kumar et al., SIGCOMM 2020, Algorithm 1).
 *
 * Control variable: fcwnd (fabric congestion window) in FXP16 packets. Swift uses the
 * INSTANTANEOUS fabric delay each event (paper §3.2: "instantaneous delay as opposed to
 * minimum or low-pass filtered"), so -- unlike TIMELY -- there is NO stored rtt_diff
 * EWMA on the fabric path. cwnd_prev (Algorithm 1 line 2) is a per-event local, not
 * stored. cur_rate is the FXP20 rate that accompanies the credit window (and the
 * new-flow sentinel: the framework zeroes the context on flow creation, so
 * cur_rate == 0 => new flow; swift_cx8_actuate never leaves it 0).
 *
 * last_ack_psn / flags carry the per-ACK num_acked estimator (Algorithm 1's num_acked
 * argument): the ACK handler differences the RoCE ACK PSN against the previous one.
 * The PSN_VALID bit distinguishes "no PSN seen yet" from "last PSN was 0".
 *
 * ecwnd / endpoint_delay_ewma are the endpoint-window state (paper §3.3): reserved and
 * UNUSED in v1 (fabric-only); present so the layout is stable when ecwnd lands.
 */
typedef struct {
	uint32_t cur_rate;		/* Actuated Tx rate (FXP20); == 0 => new flow sentinel */
	uint32_t fcwnd;			/* Fabric congestion window, FXP16 packets (live control variable) */
	uint32_t t_last_decrease;	/* Timestamp (ns) of last cwnd non-increase; once-per-RTT MD guard */
	uint32_t probe_start_ts;		/* Timestamp (ns) when RTT_REQ was last sent on a TX event (abort machine) */

	uint32_t ecwnd;			/* Endpoint congestion window, FXP16 pkts; reserved (paper §3.3, phase 2) */
	uint32_t endpoint_delay_ewma;	/* Smoothed endpoint delay (ns); reserved (paper §3.3, phase 2) */

	uint16_t retx_cnt;		/* Consecutive NACK/RTO count (Algorithm 1 "On Retransmit Timeout") */
	uint16_t probe_in_flight;		/* 1 if a probe was sent and we await its response */
	uint16_t abort_cnt;		/* Consecutive abort/retry rounds (exponential backoff) */
	uint16_t tx_since_probe_req;	/* TX events between rtt_req=1 and RTT_REQ_SENT flag */

	uint32_t last_fabric_rtt;	/* Last usable fabric RTT sample (ns); 0 = none yet. Raw last
					 * sample (not a filter -- §3.2 instantaneous-delay rationale):
					 * the delay signal the ACK handler regulates on, and the
					 * pacing denominator when cwnd < 1 packet. */

	uint32_t last_ack_psn;		/* Last RoCE ACK PSN seen; num_acked = wrap-safe delta */
	uint32_t flags;			/* SWIFT_CX8_FLAG_* bits */
	uint32_t reserved[1];		/* Pad to 48 bytes total (12 dwords). Do not exceed. */
} cc_ctxt_swift_cx8_t;

/* Bit 0: last_ack_psn holds a real PSN (set on the first ACK; a zeroed context means
 * "no PSN yet", which is indistinguishable from a genuine PSN of 0 without this flag). */
#define SWIFT_CX8_FLAG_PSN_VALID (1u << 0)

#endif /* SWIFT_CX8_CTXT_H_ */
