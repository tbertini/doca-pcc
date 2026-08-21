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

#ifndef SWIFT_CTXT_H_
#define SWIFT_CTXT_H_

#include <stdint.h>

/*
 * Swift per-flow context. MUST be exactly 48 bytes: the framework's per-flow slot is
 * mlnx_cc_algo_ctxt_t { uint32_t data[12]; } (doca_pcc_dev_data_structure_le_bf3.h)
 * and this struct is cast over it; overrunning corrupts adjacent per-flow state.
 *
 * fcwnd (FXP16 packets) is the control variable. Swift regulates on instantaneous
 * delay (paper §3.2), so no EWMA is stored on the fabric path. The framework zeroes
 * the context on flow creation, so cur_rate == 0 marks a new flow.
 */
typedef struct {
	uint32_t cur_rate;		/* Actuated Tx rate (FXP20); == 0 => new flow sentinel */
	uint32_t fcwnd;			/* Fabric congestion window, FXP16 packets (live control variable) */
	uint32_t t_last_decrease;	/* Timestamp (ns) of last cwnd non-increase; once-per-RTT MD guard */
	uint32_t probe_start_ts;		/* Timestamp (ns) when RTT_REQ was last sent on a TX event (abort machine) */

	uint32_t ecwnd;			/* Endpoint congestion window (paper §3.3); inert, no endpoint signal on BF3 */
	uint32_t endpoint_delay_ewma;	/* Smoothed endpoint delay (ns); inert, no endpoint signal on BF3 */

	uint16_t retx_cnt;		/* Consecutive NACK/RTO count (Algorithm 1 "On Retransmit Timeout") */
	uint16_t probe_in_flight;		/* 1 if a probe was sent and we await its response */
	uint16_t abort_cnt;		/* Consecutive abort/retry rounds (exponential backoff) */
	uint16_t tx_since_probe_req;	/* TX events between rtt_req=1 and RTT_REQ_SENT flag */

	uint32_t last_fabric_rtt;	/* Last usable fabric RTT sample (ns); 0 = none yet; the pacing denominator */
	uint32_t reserved[3];		/* Pad to 48 bytes total (12 dwords). Do not exceed. */
} cc_ctxt_swift_t;

#endif /* SWIFT_CTXT_H_ */
