/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "early_clock.h"

#include <zephyr/sys/util.h>
#include "cpu_pll.h"


enum parent_id {
	PARENT_XO,
	PARENT_PLL0,
	PARENT_PLL1,
	PARENT_PLL2,
	PARENT_PLL5,
	PARENT_PLL6,
	PARENT_COUNT
};
enum domain_id {
	CPU,
	CSR,
	WDT,
	APB,
	ATB,
	ASILB,
	XO,
	QSPI,
	BMTX,
	DMA,
	DEMET,
	QUP_CORE,
	QUP_AHB,
	ACMU0_RO,
	ACMU1_RO,
	ERROR,
	ACMU0_CORE,
	ACMU1_CORE,
	DOMAIN_COUNT
};

struct domain {
	uint32_t command;
	enum parent_id parent;
	uint8_t source, divider;
	bool inherited;
};

static const struct domain domains[DOMAIN_COUNT] = {
	[CPU] = {0xf1124000, PARENT_PLL1, 2, 2, false},
	[CSR] = {0xf111103c, PARENT_PLL0, 1, 12, false},
	[WDT] = {0xf112a020, PARENT_XO, 0, 2, true},
	[APB] = {0xf1114084, PARENT_XO, 0, 2, false},
	[ATB] = {0xf1114054, PARENT_PLL0, 1, 5, false},
	[ASILB] = {0xf111001c, PARENT_PLL0, 1, 6, false},
	[XO] = {0xf112301c, PARENT_XO, 0, 2, true},
	[QSPI] = {0xf1110038, PARENT_PLL2, 4, 8, false},
	[BMTX] = {0xf1111020, PARENT_PLL0, 1, 12, false},
	[DMA] = {0xf1111058, PARENT_PLL0, 1, 3, false},
	[DEMET] = {0xf111d008, PARENT_PLL5, 3, 2, true},
	[QUP_CORE] = {0xf1115020, PARENT_PLL0, 1, 6, false},
	[QUP_AHB] = {0xf1111074, PARENT_PLL0, 1, 6, false},
	[ACMU0_RO] = {0xf1132004, PARENT_PLL5, 3, 2, false},
	[ACMU1_RO] = {0xf1133004, PARENT_PLL6, 4, 2, false},
	[ERROR] = {0xf1137008, PARENT_PLL6, 4, 2, false},
	[ACMU0_CORE] = {0xf1132020, PARENT_XO, 0, 2, false},
	[ACMU1_CORE] = {0xf1133020, PARENT_XO, 0, 2, false},
};

static const struct {
	uint32_t cbcr;
	enum domain_id domain;
} branches[] = {
	{0xf1124018, CPU},        {0xf1124038, CPU},        {0xf1124030, CPU},
	{0xf1124034, CPU},        {0xf112402c, CPU},        {0xf1124040, CPU},
	{0xf1124020, CPU},        {0xf1124044, CPU},        {0xf111101c, CPU},
	{0xf1111014, CPU},        {0xf1134000, CPU},        {0xf1134008, CPU},
	{0xf1134010, CPU},        {0xf1134018, CPU},        {0xf1134020, CPU},
	{0xf1134028, CPU},        {0xf1124028, CSR},        {0xf111b004, CSR},
	{0xf1110010, CSR},        {0xf1111004, CSR},        {0xf1111038, CSR},
	{0xf1122004, CSR},        {0xf111e004, CSR},        {0xf112a000, CSR},
	{0xf112a004, CSR},        {0xf112a008, CSR},        {0xf112a00c, CSR},
	{0xf112a010, WDT},        {0xf112a014, WDT},        {0xf112a018, WDT},
	{0xf112a01c, WDT},        {0xf113203c, CSR},        {0xf113303c, CSR},
	{0xf111000c, APB},        {0xf1111010, APB},        {0xf1114030, APB},
	{0xf1110008, ATB},        {0xf111100c, ATB},        {0xf1110050, ASILB},
	{0xf1110018, ASILB},      {0xf1111018, ASILB},      {0xf1110014, XO},
	{0xf1123014, XO},         {0xf1128008, XO},         {0xf1110034, QSPI},
	{0xf1111094, BMTX},       {0xf112300c, BMTX},       {0xf1111054, DMA},
	{0xf1111090, DMA},        {0xf111d020, DEMET},      {0xf1115018, QUP_CORE},
	{0xf111500c, QUP_CORE},   {0xf111300c, QUP_AHB},    {0xf1113004, QUP_AHB},
	{0xf1115004, QUP_AHB},    {0xf1115008, QUP_AHB},    {0xf1111070, QUP_AHB},
	{0xf113201c, ACMU0_RO},   {0xf113301c, ACMU1_RO},   {0xf1137000, ERROR},
	{0xf1132038, ACMU0_CORE}, {0xf1133038, ACMU1_CORE},
};

static int parent_enable(struct monaco_clock *clock, enum parent_id parent)
{
	switch (parent) {
	case PARENT_XO:
		return 0;
	case PARENT_PLL0:
		return monaco_cpu_pll0_enable(clock);
	case PARENT_PLL1:
		return monaco_cpu_pll1_enable(clock);
	case PARENT_PLL2:
		return monaco_cpu_pll2_enable(clock);
	case PARENT_PLL5:
		return monaco_clock_pll5_enable(clock);
	case PARENT_PLL6:
		return monaco_clock_pll6_enable(clock);
	default:
		return MONACO_CLOCK_INVALID;
	}
}

static int domain_enable(struct monaco_clock *clock, const struct domain *domain,
			 bool *parent_ready)
{
	int rc;

	if (domain->inherited) {
		uint32_t value;

		if (clock->io.read32(clock->io.context, domain->command + 4, &value)) {
			return MONACO_CLOCK_IO;
		}
		uint32_t source = (value >> 8) & 7U;
		uint32_t divider = value & 0x1fU;

		divider = divider ? divider + 1U : 2U;
		/* These domains have no M/N fields, so only source and divider are compared. */
		if (source != domain->source || divider != domain->divider) {
			/* DEMET may stay on its reset default XO parent. */
			return source == 0 && divider == 2 ? 0 : MONACO_CLOCK_STATE;
		}
	}
	if (!parent_ready[domain->parent]) {
		rc = parent_enable(clock, domain->parent);
		if (rc != 0) {
			return rc;
		}
		parent_ready[domain->parent] = true;
	}
	return monaco_clock_rcg_update(clock, domain->command, domain->source, domain->divider);
}

int monaco_early_clocks_enable(struct monaco_clock *clock, struct monaco_early_clock_status *status)
{
	int rc;
	bool domain_ready[DOMAIN_COUNT] = {false};
	bool parent_ready[PARENT_COUNT] = {false};

	if (!status) {
		return MONACO_CLOCK_INVALID;
	}
	*status = (struct monaco_early_clock_status){.stage = MONACO_EARLY_CLOCK_INPUT};
	if (!clock || !clock->io.read32 || !clock->io.write32 || !clock->timer ||
	    !clock->timer->initialized || !clock->max_poll_reads) {
		return MONACO_CLOCK_INVALID;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(branches); i++) {
		enum domain_id id = branches[i].domain;

		if (!domain_ready[id]) {
			status->stage = MONACO_EARLY_CLOCK_DOMAIN;
			status->address = domains[id].command;
			rc = domain_enable(clock, &domains[id], parent_ready);
			if (rc != 0) {
				return rc;
			}
			domain_ready[id] = true;
		}
		status->stage = MONACO_EARLY_CLOCK_BRANCH;
		status->address = branches[i].cbcr;
		rc = monaco_clock_cbcr_enable(clock, branches[i].cbcr);
		if (rc != 0) {
			return rc;
		}
		status->completed_branches++;
	}
	/* PLL1 already runs at L=83, so the CPU mux is set before the refresh. */
	status->stage = MONACO_EARLY_CLOCK_CPU_REFRESH;
	status->address = domains[CPU].command;
	rc = monaco_clock_rcg_update(clock, domains[CPU].command, 2, 2);
	if (rc != 0) {
		return rc;
	}
	rc = monaco_cpu_pll1_refresh(clock);
	if (rc != 0) {
		return rc;
	}
	status->stage = MONACO_EARLY_CLOCK_COMPLETE;
	status->address = 0;
	return 0;
}
