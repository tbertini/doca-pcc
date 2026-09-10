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
 * dcqcn_params_dump: emit the DCQCN PPCC parameter and counter contract
 * (algo ID, parameter indices, units, defaults, min/max, counter indices) as
 * JSON on stdout, for the benchmark orchestrator to read at session startup.
 * It includes the same dcqcn_params.h the DPA program registers from, so the
 * emitted contract cannot drift from the device.
 * No DOCA dependencies: pure stdlib.
 */

#include <stdint.h>
#include <stdio.h>

#include "dcqcn_params.h"

/*
 * Print the DCQCN PPCC contract as a JSON object on stdout
 *
 * @return: 0 on success
 */
int main(void)
{
	printf("{\n");
	printf("  \"algo_id\": %u,\n", (unsigned int)DCQCN_ALGO_ID);

	printf("  \"params\": [\n");
	for (uint32_t i = 0; i < DCQCN_PARAM_NUM; i++) {
		const struct dcqcn_param_desc *p = &dcqcn_params[i];

		printf("    {\"idx\": %u, \"name\": \"%s\", \"unit\": \"%s\", "
		       "\"default\": %u, \"min\": %u, \"max\": %u}%s\n",
		       (unsigned int)i, p->name, p->unit,
		       (unsigned int)p->def, (unsigned int)p->min, (unsigned int)p->max,
		       (i + 1 == DCQCN_PARAM_NUM) ? "" : ",");
	}
	printf("  ],\n");

	printf("  \"counters\": [\n");
	for (uint32_t i = 0; i < DCQCN_COUNTER_NUM; i++) {
		printf("    {\"idx\": %u, \"name\": \"%s\"}%s\n",
		       (unsigned int)i, dcqcn_counter_names[i],
		       (i + 1 == DCQCN_COUNTER_NUM) ? "" : ",");
	}
	printf("  ]\n");

	printf("}\n");
	return 0;
}
