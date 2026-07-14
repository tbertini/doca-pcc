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
 */

/*
 * swift_params_dump: emit the Swift PPCC parameter/counter contract as JSON.
 *
 * incast-bench invokes this on the sender host to learn the wire-format contract
 * (algo ID, parameter indices, units, defaults, min/max, counter indices) at session
 * startup. Keeping this a one-shot dumper that #includes the same swift_params.h the
 * DPA registers from avoids drift between the DPA's compiled-in contract and the
 * orchestrator's encoder.
 *
 * No DOCA dependencies -- pure stdlib, can be cross-compiled or run on either the
 * BlueField host or the user's Mac.
 */

#include <stdint.h>
#include <stdio.h>

#include "swift_params.h"

int main(void)
{
	printf("{\n");
	printf("  \"algo_id\": %u,\n", (unsigned int)SWIFT_ALGO_ID);

	printf("  \"params\": [\n");
	for (uint32_t i = 0; i < SWIFT_NUM_PARAMS; i++) {
		const struct swift_param_desc *p = &swift_params[i];

		printf("    {\"idx\": %u, \"name\": \"%s\", \"unit\": \"%s\", "
		       "\"default\": %u, \"min\": %u, \"max\": %u}%s\n",
		       (unsigned int)i, p->name, p->unit,
		       (unsigned int)p->def, (unsigned int)p->min, (unsigned int)p->max,
		       (i + 1 == SWIFT_NUM_PARAMS) ? "" : ",");
	}
	printf("  ],\n");

	printf("  \"counters\": [\n");
	for (uint32_t i = 0; i < SWIFT_NUM_COUNTERS; i++) {
		printf("    {\"idx\": %u, \"name\": \"%s\"}%s\n",
		       (unsigned int)i, swift_counter_names[i],
		       (i + 1 == SWIFT_NUM_COUNTERS) ? "" : ",");
	}
	printf("  ]\n");

	printf("}\n");
	return 0;
}
