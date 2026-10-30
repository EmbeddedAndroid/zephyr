/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i2c_profile.h"

#include <zephyr/sys/util.h>

#define SE                UINT32_C(0xf1980000)
#define QUP_VERSION       UINT32_C(0xf19c0004)
#define QUP_CONTROL       UINT32_C(0xf19c0120)
#define QUP_CLOCK_CONTROL UINT32_C(0xf19c0118)

/* Register values for CFG0..18 and CFG64..113; no sequencer code is loaded. */
static const uint32_t cfg0_18[] = {
	0x00000090, 0x00000000, 0x00000090, 0x00000000, 0x00038028, 0x00084080, 0x00000343,
	0x00010000, 0x00000000, 0x00001a00, 0x00000100, 0x00000000, 0x00000000, 0x00000000,
	0x00808008, 0x001c0020, 0x00000000, 0x00020000, 0x00000000,
};

static const uint32_t cfg64_113[] = {
	0x00000201, 0x0001fc01, 0x00036222, 0x09c01ffc, 0x00100120, 0x02c00000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
	0x00000000, 0x00000000, 0x00000409, 0x00000003, 0x00000002, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x0007f8fe, 0x000ffefe, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x0007f807, 0x000ffefe,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00c00000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000055,
};

_Static_assert(ARRAY_SIZE(cfg0_18) == 19, "CFG0..18 count");
_Static_assert(ARRAY_SIZE(cfg64_113) == 50, "CFG64..113 count");

struct profile_context {
	const struct monaco_timer_io *io;
	struct monaco_i2c_profile_status *status;
};

bool monaco_i2c_profile_write_allowed(uint32_t address)
{
	if (address & 3U) {
		return false;
	}
	if (address == QUP_CONTROL || address == QUP_CLOCK_CONTROL || address == SE + 0x1000 ||
	    address == SE + 0x1004 || address == SE || address == SE + 4) {
		return true;
	}
	return (address >= SE + 0x100 && address <= SE + 0x148) ||
	       (address >= SE + 0x200 && address <= SE + 0x2c4);
}

static int read32(struct profile_context *p, uint32_t address, uint32_t *value)
{
	struct monaco_i2c_profile_status *s = p->status;
	uint32_t sampled = 0;

	s->address = address;
	s->value = 0;
	s->value_valid = false;
	s->reads_attempted++;
	s->callback_result = p->io->read32(p->io->context, address, &sampled);
	if (s->callback_result) {
		return MONACO_I2C_PROFILE_IO;
	}
	s->reads_completed++;
	s->value = sampled;
	s->value_valid = true;
	*value = sampled;
	return 0;
}

static int write32(struct profile_context *p, uint32_t address, uint32_t value)
{
	struct monaco_i2c_profile_status *s = p->status;

	s->address = address;
	s->value = value;
	s->value_valid = true;
	if (!monaco_i2c_profile_write_allowed(address)) {
		return MONACO_I2C_PROFILE_INVALID;
	}
	s->writes_attempted++;
	s->callback_result = p->io->write32(p->io->context, address, value);
	if (s->callback_result) {
		return MONACO_I2C_PROFILE_IO;
	}
	s->writes_completed++;
	return 0;
}

static int check_idle(struct profile_context *p)
{
	int rc;
	uint32_t value;

	rc = read32(p, SE + 0x40, &value);
	if (rc != 0) {
		return rc;
	}
	if (value & 0x1001U) {
		return MONACO_I2C_PROFILE_BUSY;
	}
	rc = read32(p, SE + 0x800, &value);
	if (rc != 0) {
		return rc;
	}
	if (value & 0x0fffffffU) {
		return MONACO_I2C_PROFILE_FIFO;
	}
	rc = read32(p, SE + 0x804, &value);
	if (rc != 0) {
		return rc;
	}
	return value & 0x01ffffffU ? MONACO_I2C_PROFILE_FIFO : 0;
}

static int check_geometry(struct profile_context *p)
{
	int rc;
	uint32_t tx, rx;

	rc = read32(p, SE + 0xe24, &tx);
	if (rc != 0) {
		return rc;
	}
	rc = read32(p, SE + 0xe28, &rx);
	if (rc != 0) {
		return rc;
	}
	p->status->tx_depth = (tx >> 16) & 0x3fU;
	p->status->rx_depth = (rx >> 16) & 0x3fU;
	if (((tx >> 24) & 0x3fU) != 32 || ((rx >> 24) & 0x3fU) != 32 || !p->status->tx_depth ||
	    p->status->rx_depth < 3) {
		return MONACO_I2C_PROFILE_FIFO;
	}
	return 0;
}

static int configure(struct profile_context *p)
{
	int rc;
	struct monaco_i2c_profile_status *s = p->status;
	uint32_t value;

	s->stage = MONACO_I2C_PROFILE_GUARDS;
	rc = read32(p, QUP_VERSION, &s->qup_version);
	if (rc != 0) {
		return rc;
	}
	if (s->qup_version != 0x300b0000U) {
		return MONACO_I2C_PROFILE_VERSION;
	}
	rc = read32(p, SE + 0xe2c, &s->hardware_parameters);
	if (rc != 0) {
		return rc;
	}
	if (s->hardware_parameters == UINT32_MAX || !(s->hardware_parameters & 0x1000U)) {
		return MONACO_I2C_PROFILE_CAPABILITY;
	}
	rc = check_idle(p);
	if (rc != 0) {
		return rc;
	}
	rc = check_geometry(p);
	if (rc != 0) {
		return rc;
	}

	s->stage = MONACO_I2C_PROFILE_WRAPPER;
	rc = read32(p, QUP_CONTROL, &value);
	if (rc != 0) {
		return rc;
	}
	rc = write32(p, QUP_CONTROL, (value & 0x7eU) | 1U);
	if (rc != 0) {
		return rc;
	}
	rc = read32(p, QUP_CLOCK_CONTROL, &value);
	if (rc != 0) {
		return rc;
	}
	rc = write32(p, QUP_CLOCK_CONTROL, 1);
	if (rc != 0) {
		return rc;
	}

	s->stage = MONACO_I2C_PROFILE_METADATA;
	rc = write32(p, SE + 0x1000, 0x303);
	if (rc != 0) {
		return rc;
	}
	rc = write32(p, SE + 0x1004, 0x303);
	if (rc != 0) {
		return rc;
	}
	rc = write32(p, SE, 9);
	if (rc != 0) {
		return rc;
	}
	rc = write32(p, SE + 4, 9);
	if (rc != 0) {
		return rc;
	}
	s->stage = MONACO_I2C_PROFILE_CFG0;
	for (uint32_t i = 0; i < ARRAY_SIZE(cfg0_18); i++) {
		rc = write32(p, SE + 0x100 + 4 * i, cfg0_18[i]);
		if (rc != 0) {
			return rc;
		}
	}
	s->stage = MONACO_I2C_PROFILE_CFG64;
	for (uint32_t i = 0; i < ARRAY_SIZE(cfg64_113); i++) {
		rc = write32(p, SE + 0x200 + 4 * i, cfg64_113[i]);
		if (rc != 0) {
			return rc;
		}
	}

	s->stage = MONACO_I2C_PROFILE_VERIFY;
	rc = read32(p, SE + 0x68, &s->protocol_revision);
	if (rc != 0) {
		return rc;
	}
	if (((s->protocol_revision >> 8) & 0xffU) != 3) {
		return MONACO_I2C_PROFILE_READBACK;
	}
	rc = check_geometry(p);
	if (rc != 0) {
		return rc;
	}
	rc = check_idle(p);
	if (rc != 0) {
		return rc;
	}
	s->configuration_readback = true;
	s->stage = MONACO_I2C_PROFILE_COMPLETE;
	return 0;
}

int monaco_i2c_profile_init(const struct monaco_timer_io *io,
			    const struct monaco_i2c_profile_qualification *qualification,
			    struct monaco_i2c_profile_status *status)
{
	if (!status) {
		return MONACO_I2C_PROFILE_INVALID;
	}
	*status = (struct monaco_i2c_profile_status){
		.stage = MONACO_I2C_PROFILE_INPUT,
		.result = MONACO_I2C_PROFILE_INVALID,
	};
	if (!io || !io->read32 || !io->write32 || !qualification) {
		return status->result;
	}
	if (qualification->board != MONACO_I2C_PROFILE_VENTUNO_Q ||
	    !qualification->exclusive_ownership || !qualification->clocks_qualified ||
	    !qualification->reset_state_qualified || !qualification->board_state_qualified) {
		status->result = MONACO_I2C_PROFILE_UNQUALIFIED;
		return status->result;
	}
	struct profile_context p = {.io = io, .status = status};

	status->result = configure(&p);
	return status->result;
}
