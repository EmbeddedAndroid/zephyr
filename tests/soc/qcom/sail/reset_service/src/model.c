/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
/* Coordinator, receive, preparation and PMIC chain on modeled kernel and device boundaries. */
#include "api.h"
#define sail_model_test cold_component_tests
#include "test_cold_boot.c"
#undef sail_model_test
#include "reset_service_internal.h"
#include "reset_io.h"

unsigned int test_irq_depth;
bool test_isr;
uintptr_t test_thread = 1;
static struct model hardware;
static struct monaco_cold_boot boot;
static struct sail_reset_service service;
static enum sail_boot_mode mode;
static int boot_result;
static struct device device = {true};
static struct channel {
	mbox_callback_t callback;
	void *context;
	bool enabled;
} channels[2];
static uint32_t early_replies, fail_early;
static uint32_t retires, writes, replies, barriers, control_reads, io_count, inject_at;
static uint32_t delay_first;
static uint32_t fail_read, fail_write, fail_reply, fail_mbox, mbox_calls;
static bool wrong_control, bad_crc, bad_readback, stop_timer, active, reenter, retired;
static int retire_error;
static char order[64];
static size_t order_count;
static uint8_t slave2[256];
static uint32_t shutdown_ops, shutdown_fail, shutdown_crc, shutdown_feedback, shutdown_readback;
static bool shutdown_after;
static uint32_t shutdown_control_reads, shutdown_control_nonzero, shutdown_control_fail;
static uint32_t shutdown_control_inject, shutdown_control_reenter;
static uint32_t shutdown_ops_at_control[16];
static int shutdown_control_error;
static bool shutdown_control_after;
static struct shutdown_op {
	uint8_t address, reg, value;
	bool read;
} shutdown_trace[16];
static void emit(uint32_t channel, uint32_t word);
static void trace(char c)
{
	sail_check(order_count < sizeof(order));
	order[order_count++] = c;
}

int mbox_mtu_get(const struct device *d)
{
	sail_check(d == &device);
	return 4;
}
uint32_t mbox_max_channels_get(const struct device *d)
{
	sail_check(d == &device);
	return 2;
}
int mbox_register_callback(const struct device *d, uint32_t ch, mbox_callback_t cb, void *ctx)
{
	sail_check(d == &device && ch < 2 && test_irq_depth);
	mbox_calls++;
	if (mbox_calls == fail_mbox) {
		return -EIO;
	}
	if (channels[ch].enabled) {
		return -EBUSY;
	}
	channels[ch].callback = cb;
	channels[ch].context = ctx;
	return 0;
}
int mbox_set_enabled(const struct device *d, uint32_t ch, bool enabled)
{
	sail_check(d == &device && ch < 2 && test_irq_depth);
	mbox_calls++;
	if (mbox_calls == fail_mbox) {
		return -EIO;
	}
	if (channels[ch].enabled == enabled) {
		return -EALREADY;
	}
	channels[ch].enabled = enabled;
	return 0;
}
static void emit(uint32_t ch, uint32_t word)
{
	uint8_t b[4] = {word, word >> 8, word >> 16, word >> 24};
	struct mbox_msg msg = {b, 4};

	sail_check(channels[ch].enabled && channels[ch].callback);
	sail_check(!test_irq_depth);
	test_isr = true;
	channels[ch].callback(&device, ch, channels[ch].context, &msg);
	test_isr = false;
	memset(b, 0xff, 4);
}
static void boundary(void)
{
	if (!active) {
		return;
	}
	sail_check(test_irq_depth == 0);
	io_count++;
	if (!delay_first && service.transaction.result.phase == MONACO_PMIC_TRANSACTION_DELAYING) {
		delay_first = io_count;
	}
	if (io_count == inject_at) {
		emit(0, 0x01020304);
	}
	if (reenter) {
		sail_check(sail_reset_poll(&service) == -EBUSY);
		sail_check(sail_reset_stop(&service) == -EBUSY);
		sail_check(sail_reset_timer_enter(&service) == -EBUSY);
	}
}
static int observed_read(void *ctx, uint32_t a, uint32_t *v)
{
	boundary();
	bool shutdown_control = active && a == UINT32_C(0xf1149000);

	if (shutdown_control) {
		sail_check(retired && !test_irq_depth);
		sail_check(shutdown_control_reads < ARRAY_SIZE(shutdown_ops_at_control));
		shutdown_ops_at_control[shutdown_control_reads++] = shutdown_ops;
		if (shutdown_control_reads == shutdown_control_nonzero) {
			*reg(&hardware, a) = UINT32_C(0x47f2);
		}
		if (shutdown_control_reads == shutdown_control_fail && !shutdown_control_after) {
			return shutdown_control_error;
		}
	}
	int rc;

	if (shutdown_control) {
		rc = record(&hardware, a, false);
		if (rc == 0) {
			*v = *reg(&hardware, a);
		}
	} else {
		rc = read32(ctx, a, v);
	}
	if (shutdown_control) {
		if (shutdown_control_reads == shutdown_control_inject) {
			emit(0, UINT32_C(0x01020304));
		}
		if (shutdown_control_reads == shutdown_control_reenter) {
			sail_check(sail_reset_poll(&service) == -EBUSY);
			sail_check(sail_reset_stop(&service) == -EBUSY);
			sail_check(sail_reset_timer_enter(&service) == -EBUSY);
		}
		if (shutdown_control_reads == shutdown_control_fail) {
			return shutdown_control_error;
		}
	}
	if (active && rc == 0 && a == SE + 0x780 &&
	    (bad_crc || (shutdown_crc != 0U && shutdown_ops == shutdown_crc))) {
		*v ^= 0x100;
	}
	return rc;
}
static int observed_write(void *ctx, uint32_t a, uint32_t v)
{
	boundary();
	if (active && bad_readback && a == SE + 0x700) {
		hardware.pmic[0xc9] = 2;
	}
	if (active && a == SE + 0x700 && service.status.word == 0xcd060000) {
		sail_check(retired && !test_irq_depth);
		uint32_t rx = *reg(&hardware, SE + 0x270);
		uint8_t offset = (uint8_t)v, value = (uint8_t)(v >> 8);

		sail_check(shutdown_ops < ARRAY_SIZE(shutdown_trace));
		shutdown_trace[shutdown_ops++] =
			(struct shutdown_op){hardware.address, offset, value, rx != 0};
		if (shutdown_ops == shutdown_fail && !shutdown_after) {
			return IO_ERROR;
		}
		if (shutdown_readback && hardware.address == 0x28 && rx &&
		    offset == shutdown_readback) {
			hardware.pmic[offset] ^= 1;
		}
		int rc;

		if (hardware.address == 0x12) {
			uint8_t covered[] = {0x24, offset, rx ? 0x25 : value, slave2[offset]};

			sail_check(*reg(&hardware, SE + 0x26c) == (rx ? 1U : 3U));
			sail_check(rx == 0U || rx == 2U);
			sail_check(!record(&hardware, a, true));
			hardware.transactions++;
			if (rx) {
				*reg(&hardware, SE + 0x780) =
					slave2[offset] | (uint32_t)crc(covered, 4) << 8;
				*reg(&hardware, SE + 0x804) = UINT32_C(0x80000001) | rx << 28;
			} else {
				sail_check((uint8_t)(v >> 16) == crc(covered, 3));
				slave2[offset] = value;
			}
			*reg(&hardware, SE + 0x610) = 1;
			rc = 0;
		} else {
			rc = write32(ctx, a, v);
			if (!rx && offset == 0x80 && !shutdown_feedback) {
				hardware.pmic[0x82] = (hardware.pmic[0x82] & 0xfeU) | (value & 1U);
			}
		}
		return shutdown_ops == shutdown_fail ? IO_ERROR : rc;
	}
	return write32(ctx, a, v);
}
static int controls_read(void *ctx, uint32_t a, uint32_t *v)
{
	(void)ctx;
	sail_check(retired && !test_irq_depth);
	boundary();
	control_reads++;
	if (control_reads == fail_read) {
		return -EFAULT;
	}
	*v = *reg(&hardware, a);
	if (wrong_control && writes && a == 0xf1149420) {
		*v ^= 1;
	}
	return 0;
}
static int controls_write(void *ctx, uint32_t a, uint32_t v)
{
	(void)ctx;
	sail_check(retired && !test_irq_depth);
	boundary();
	writes++;
	trace('C');
	sail_check((a == 0xf1149420 && v == 0xcff8) || (a == 0xf0300044 && v == 0x00400ea0));
	*reg(&hardware, a) = v;
	return writes == fail_write ? -EFAULT : 0; /* failure after the modeled effect */
}
static int reply(void *ctx, uint32_t answer, uint32_t index)
{
	(void)ctx;
	if (!service.status.claimed) {
		/* An SDI-disable answer outside a reset: no retirement, no effects. */
		sail_check(test_irq_depth && !retired && !writes && index == early_replies % 4);
		sail_check(service.status.word == 0xcd0a0000 && answer == 0xaa0c0000);
		early_replies++;
		return early_replies == fail_early ? -EFAULT : 0;
	}
	sail_check(test_irq_depth && retired && index == replies % 4);
	sail_check(answer == (replies < 4 ? 0xaa030000 : 0xaa0c0000));
	trace('A');
	if (replies >= 4) {
		sail_check(service.status.phase == SAIL_RESET_SERVICE_ACKED &&
		       service.status.word == 0xcd0a0000);
	} else if (service.status.word == 0xcd060000) {
		sail_check(service.transaction.result.phase ==
		       MONACO_PMIC_TRANSACTION_SHUTDOWN_VERIFIED);
		sail_check(service.transaction.result.shutdown.stage ==
			   MONACO_PMIC_SHUTDOWN_VERIFIED);
		sail_check(service.transaction.result.time_reads == 0 && writes == 0 &&
		       delay_first == 0);
	} else {
		sail_check(service.transaction.result.phase == MONACO_PMIC_TRANSACTION_DELAYED);
		sail_check(service.transaction.result.after_us -
				   service.transaction.result.before_us >=
			   50000);
	}
	replies++;
	return replies == fail_reply ? -EFAULT : 0;
}
static int retire(void *ctx)
{
	sail_check(ctx == &hardware && test_irq_depth);
	retires++;
	retired = true;
	trace('R');
	sail_check(writes == 0 && replies == 0);
	sail_check(sail_reset_poll(&service) == -EWOULDBLOCK);
	sail_check(sail_reset_timer_enter(&service) == -EWOULDBLOCK);
	if (stop_timer) {
		hardware.stopped_timer = true;
	}
	return retire_error;
}
static const struct monaco_timer_io controls = {NULL, controls_read, controls_write};
static void begin(void)
{
	active = false;
	shutdown_ops = shutdown_fail = shutdown_crc = shutdown_feedback = shutdown_readback = 0;
	shutdown_after = false;
	shutdown_control_reads = shutdown_control_nonzero = shutdown_control_fail = 0;
	shutdown_control_inject = shutdown_control_reenter = 0;
	shutdown_control_error = -EFAULT;
	shutdown_control_after = false;
	memset(shutdown_ops_at_control, 0, sizeof(shutdown_ops_at_control));
	memset(slave2, 0, sizeof(slave2));
	memset(shutdown_trace, 0, sizeof(shutdown_trace));
	test_isr = false;
	test_irq_depth = 0;
	test_thread = 1;
	prepare(&hardware, &boot, true);
	const struct monaco_cold_boot_io io = {.context = &hardware,
					       .read32 = observed_read,
					       .write32 = observed_write,
					       .read_mpidr = read_mpidr,
					       .read_cpsr = read_cpsr,
					       .md_access_contract = hardware.access_contract,
					       .md_publication = {&hardware, NULL}};
	struct monaco_cold_boot_limits limits = monaco_cold_boot_defaults;

	limits.timer_read_budget = 6000;
	limits.md_bist.max_poll_reads = 32;
	limits.clock_poll_reads = 16;
	boot_result = monaco_cold_boot_run(&boot, &io, &limits);
	sail_check(boot_result == MONACO_COLD_PCINIT_REQUIRED);
	mode = sail_boot_mode_select(&boot, boot_result, true);
	sail_check(mode == SAIL_BOOT_PCINIT_DEFERRED);
	memset(&service, 0, sizeof(service));
	memset(channels, 0, sizeof(channels));
	retires = writes = replies = barriers = control_reads = io_count = inject_at = 0;
	early_replies = fail_early = 0;
	delay_first = 0;
	fail_read = fail_write = fail_reply = fail_mbox = mbox_calls = 0;
	wrong_control = bad_crc = bad_readback = stop_timer = reenter = retired = false;
	retire_error = 0;
	order_count = 0;
	*reg(&hardware, 0xf1149024) = 0x00020101;
	*reg(&hardware, 0xf1149020) = 0x1002;
	*reg(&hardware, 0xf1149420) = 0xffff;
	*reg(&hardware, 0xf0300044) = 0;
	active = true;
}
static int start(void)
{
	return sail_reset_start(&service, &boot, &boot_result, &mode, &device, &controls, reply,
				0x0801d400, retire, &hardware);
}
static void no_retry(void)
{
	struct sail_reset_service_status s;

	/* A replied reset or shutdown completes on the SDI-disable notice. */
	if (sail_reset_status_get(&service, &s) == 0 && s.phase == SAIL_RESET_SERVICE_ACKED) {
		uint32_t answered = replies;

		emit(1, 0xcd0a0000);
		sail_check(sail_reset_poll(&service) == 1 && replies == answered + 4);
	}
	uint32_t count = hardware.count;

	sail_check(start() == -EALREADY);
	sail_check(sail_reset_poll(&service) < 0);
	sail_check(hardware.count == count);

	sail_check(sail_reset_status_get(&service, &s) == 0);
	sail_check(s.phase == SAIL_RESET_SERVICE_FAILED || s.phase == SAIL_RESET_SERVICE_COMPLETE);
	sail_check(sail_reset_stop(&service) == 0);
}
/* Run the fixed adapter through instrumented native access boundaries. */
uint32_t sys_read32(uintptr_t address)
{
	sail_check(address == 0xf1149024 || address == 0xf1149020 || address == 0xf1149420 ||
	       address == 0xf0300044);
	return 0x12345678;
}
void sys_write32(uint32_t value, uintptr_t address)
{
	if (address >= 0xf0305094 && address <= 0xf03050a0) {
		static const uint32_t bytes[8] = {0, 0, 3, 0xaa, 0, 0, 0xc, 0xaa};

		sail_check(address == 0xf0305094 + (replies % 4) * 4 && value == bytes[replies]);
		replies++;
	} else {
		sail_check((address == 0xf1149420 && value == 0xcff8) ||
		       (address == 0xf0300044 && value == 0x00400ea0));
		writes++;
	}
}
void barrier_dsync_fence_full(void)
{
	barriers++;
}
static void shutdown_begin(void)
{
	begin();
	slave2[9] = 0xf7;
	hardware.pmic[0x80] = 0xa5;
	hardware.pmic[0x82] = 0x21;
	sail_check(!start());
}
static int shutdown_poll(void)
{
	emit(1, 0xcd060000);
	return sail_reset_poll(&service);
}
static void shutdown_tests(void)
{
	shutdown_begin();
	reenter = true;
	uint32_t transactions = hardware.transactions;

	sail_check(shutdown_poll() == 1 && replies == 4 && writes == 0 && retires == 1);
	sail_check(order_count == 5 && memcmp(order, "RAAAA", 5) == 0);
	sail_check(hardware.transactions == transactions + 11 && shutdown_ops == 11);
	sail_check(shutdown_control_reads == 14 && service.transaction.result.guards == 28);
	sail_check(service.transaction.result.shutdown_control.reads_attempted == 14 &&
	       service.transaction.result.shutdown_control.reads_completed == 14 &&
	       service.transaction.result.shutdown_control.valid &&
	       service.transaction.result.shutdown_control.value == 0 &&
	       service.transaction.result.shutdown_control.read_status == 0);
	sail_check(slave2[9] == 0xb7 && hardware.pmic[0x80] == 0xa4 && hardware.pmic[0x82] == 0x20);
	sail_check(hardware.pmic[0xc9] == 0 && hardware.pmic[0xcb] == 10 &&
		   hardware.pmic[0xcc] == 0);
	sail_check(service.transaction.result.shutdown.reads_completed == 6 &&
	       service.transaction.result.shutdown.writes_completed == 5 &&
	       service.transaction.result.shutdown.valid == 63);
	static const uint8_t expected_regs[] = {0x82, 9,    9,    0x80, 0x80, 0x82,
						0xc9, 0xcb, 0xcc, 0xc9, 0xcc};
	static const bool expected_reads[] = {true,  true,  false, true, false, true,
					      false, false, false, true, true};
	for (uint32_t i = 0; i < 11; i++) {
		sail_check(shutdown_trace[i].reg == expected_regs[i] &&
		       shutdown_trace[i].read == expected_reads[i]);
		sail_check(shutdown_trace[i].address == ((i == 1 || i == 2) ? 0x12 : 0x28));
	}
	uint32_t domain = io_count;
	uint32_t operations[14];

	memcpy(operations, shutdown_ops_at_control, sizeof(operations));
	no_retry();
	/* Each guard rereads the owner, before any effect and at completion. */
	for (uint32_t i = 1; i <= ARRAY_SIZE(operations); i++) {
		shutdown_begin();
		shutdown_control_nonzero = i;
		sail_check(shutdown_poll() == -ESTALE && !replies && !writes);
		sail_check(shutdown_ops == operations[i - 1] && shutdown_control_reads == i);
		sail_check(service.transaction.result.shutdown_control.valid &&
		       service.transaction.result.shutdown_control.value == UINT32_C(0x47f2) &&
		       service.transaction.result.shutdown_control.read_status == 0);
		sail_check(*reg(&hardware, UINT32_C(0xf1149000)) == UINT32_C(0x47f2));
		no_retry();
		for (uint32_t after = 0; after < 2; after++) {
			for (uint32_t positive = 0; positive < 2; positive++) {
				shutdown_begin();
				shutdown_control_fail = i;
				shutdown_control_after = after;
				shutdown_control_error = positive ? 1 : -EFAULT;
				sail_check(shutdown_poll() == (positive ? -EIO : -EFAULT));
				sail_check(!replies && !writes &&
					   shutdown_ops == operations[i - 1]);
				sail_check(service.transaction.result.shutdown_control
						       .reads_attempted == i &&
				       service.transaction.result.shutdown_control
						       .reads_completed == i - 1 &&
				       !service.transaction.result.shutdown_control.valid &&
				       service.transaction.result.shutdown_control.read_status ==
					       shutdown_control_error);
				no_retry();
			}
		}
		shutdown_begin();
		shutdown_control_inject = i;
		sail_check(shutdown_poll() < 0 && !replies && !writes &&
		       shutdown_ops == operations[i - 1] && shutdown_control_reads == i);
		sail_check(service.transaction.result.shutdown_control.valid &&
		       service.transaction.result.shutdown_control.value == 0);
		no_retry();
		shutdown_begin();
		shutdown_control_reenter = i;
		sail_check(shutdown_poll() == 1 && replies == 4 && shutdown_control_reads == 14);
		no_retry();
	}
	/* Every transfer fails both before its effect and after its modeled effect. */
	for (uint32_t i = 1; i <= 11; i++) {
		for (uint32_t after = 0; after < 2; after++) {
			shutdown_begin();
			shutdown_fail = i;
			shutdown_after = after;
			sail_check(shutdown_poll() < 0 && shutdown_ops == i && replies == 0 &&
			       writes == 0 && retires == 1);
			sail_check(service.transaction.result.shutdown.status == MONACO_I2C_IO);
			no_retry();
		}
	}
	/* A CRC error in any of the six reads is never treated as a valid value. */
	for (uint32_t i = 0; i < 11; i++) {
		if (expected_reads[i]) {
			shutdown_begin();
			shutdown_crc = i + 1;
			sail_check(shutdown_poll() == MONACO_PMIC_CRC && shutdown_ops == i + 1 &&
			       !replies);
		}
	}
	/* RX history checks surround all MMIO and transfer boundaries; no ACK on loss. */
	for (uint32_t i = 1; i <= domain; i++) {
		shutdown_begin();
		inject_at = i;
		sail_check(shutdown_poll() < 0 && !replies && !writes);
	}
	shutdown_begin();
	hardware.pmic[0x82] |= 8;
	sail_check(shutdown_poll() == MONACO_PMIC_RESET_VERIFY && shutdown_ops == 1 &&
	       !service.transaction.result.shutdown.writes_attempted && !replies);
	shutdown_begin();
	shutdown_feedback = 1;
	sail_check(shutdown_poll() == MONACO_PMIC_RESET_VERIFY && shutdown_ops == 6 && !replies);
	for (uint32_t reg = 0xc9; reg <= 0xcc; reg += 3) {
		shutdown_begin();
		shutdown_readback = reg;
		sail_check(shutdown_poll() == MONACO_PMIC_RESET_VERIFY && !replies);
	}
	shutdown_begin();
	retire_error = -ECANCELED;
	sail_check(shutdown_poll() == -ECANCELED && !shutdown_ops && !replies);
	shutdown_begin();
	boot.pmic_boot.tail_owner = NULL;
	sail_check(shutdown_poll() == -ESTALE && !shutdown_ops && !replies);
	shutdown_begin();
	*reg(&hardware, 0xf1149024) = 12;
	sail_check(shutdown_poll() == -ESTALE && !shutdown_ops && !replies);
	shutdown_begin();
	emit(0, 0xcd060000);
	sail_check(sail_reset_poll(&service) == -EPERM && !retires && !shutdown_ops);
	shutdown_begin();
	emit(1, 0xcd060000);
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == -EALREADY && !retires && !shutdown_ops);
	for (uint32_t i = 1; i <= 4; i++) {
		shutdown_begin();
		fail_reply = i;
		sail_check(shutdown_poll() < 0 && service.status.reply_attempted == i &&
		       service.status.reply_completed == i - 1 && shutdown_ops == 11);
		no_retry();
	}
	printf("PASS: shutdown: 14 live-zero guards: all nonzero, 56 before/after read failures, "
	       "14 post-read history losses and 14 reentry cases; no ACK on failure\n");
	printf("PASS: shutdown real CRC/I2C chain; 11-transfer order, 22 before/after failures, "
	       "6 CRC failures, %u history boundaries, bit "
	       "preservation/feedback/ownership/terminal ACK gates\n",
	       domain);
}
int sail_model_test(void)
{
	FILE *file = fopen(SAIL_TEST_FIXTURE_DIR "/early-clock.trace", "r");

	sail_check(file);
	while (clock_events < ARRAY_SIZE(clock_trace)) {
		struct clock_event *e = &clock_trace[clock_events++];

		sail_check(fscanf(file, " %c %x %x", &e->kind, &e->address, &e->value) == 3);
	}
	sail_check(!fclose(file));
	begin();
	sail_check(!start());
	sail_check(sail_reset_poll(&service) == 0);
	sail_check(!sail_reset_timer_enter(&service));
	sail_check(start() == -EALREADY);
	sail_reset_timer_leave(&service);
	emit(0, 0xcc010000);
	sail_check(sail_reset_poll(&service) == 1);
	emit(0, 0xcb000003);
	sail_check(sail_reset_poll(&service) == 1);
	emit(1, 0xcb000007);
	sail_check(sail_reset_poll(&service) == 1);
	emit(0, 0xcb00000f);
	sail_check(sail_reset_poll(&service) == 1);
	sail_check(!retires && !writes && !replies);
	uint32_t transfers = hardware.transactions;

	reenter = true;
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == 1);
	sail_check(service.status.phase == SAIL_RESET_SERVICE_ACKED && retires == 1 &&
		   writes == 2 && replies == 4 && channels[0].enabled && channels[1].enabled);
	/* The SDI-disable notice that follows the reset reply gets its own answer. */
	emit(1, 0xcd0a0000);
	sail_check(sail_reset_poll(&service) == 1);
	sail_check(service.status.phase == SAIL_RESET_SERVICE_COMPLETE && replies == 8 &&
	       service.status.reply_completed == 4 && service.status.sdi_answers == 1 &&
	       service.status.consumed == 6);
	sail_check(hardware.transactions == transfers + 5 && hardware.pmic[0xc9] == 0 &&
	       hardware.pmic[0xcb] == 10 && hardware.pmic[0xcc] == 1);
	sail_check(order_count == 11 && memcmp(order, "RCCAAAAAAAA", 11) == 0);
	sail_check(!channels[0].enabled && !channels[1].enabled &&
		   mode == SAIL_BOOT_PCINIT_DEFERRED && boot_result == MONACO_COLD_PCINIT_REQUIRED);
	sail_check(shutdown_control_reads == 0 &&
	       service.transaction.result.shutdown_control.reads_attempted == 0);
	uint32_t domain_reads = io_count, delay_start = delay_first, history_tests = 0;

	no_retry();
	begin();
	*reg(&hardware, 0xf1149420) = 0xcff8;
	*reg(&hardware, 0xf0300044) = 0x00400ea0;
	sail_check(!start());
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == 1 && !writes && replies == 4);
	/* After the reset reply only the secure SDI-disable notice is answered. */
	const struct {
		uint32_t channel, word;
	} after[] = {{0, 0xcd0a0000}, {1, 0xcd070000}, {1, 0xcd0a0001}, {1, 0xcd060000}};

	for (size_t i = 0; i < ARRAY_SIZE(after); i++) {
		begin();
		sail_check(!start());
		emit(1, 0xcd070000);
		sail_check(sail_reset_poll(&service) == 1 && replies == 4);
		emit(after[i].channel, after[i].word);
		sail_check(sail_reset_poll(&service) == -EPROTO && replies == 4);
		sail_check(service.status.phase == SAIL_RESET_SERVICE_FAILED);
		sail_check(!channels[0].enabled && !channels[1].enabled);
		no_retry();
	}
	/* Unsupported, bit-mutated and wrong-channel inputs never act. */
	const uint32_t denied[] = {0, 0xcc010001, 0xcd060000, 0xcd0a0000, 0xcc020000, 0xcb000002};

	for (size_t i = 0; i < ARRAY_SIZE(denied); i++) {
		begin();
		sail_check(!start());
		emit(0, denied[i]);
		sail_check(sail_reset_poll(&service) < 0 && !retires && !writes && !replies);
		no_retry();
	}
	for (uint32_t bit = 0; bit < 32; bit++) {
		if (bit == 16U) {
			continue;
		} /* This exact mutation is the supported CD06 opcode. */
		begin();
		sail_check(!start());
		emit(1, 0xcd070000 ^ (1U << bit));
		sail_check(sail_reset_poll(&service) < 0 && !writes && !replies);
	}
	begin();
	sail_check(!start());
	emit(0, 0xcd070000);
	sail_check(sail_reset_poll(&service) == -EPERM && !retires);
	begin();
	sail_check(!start());
	emit(0, 0xcb000003);
	sail_check(sail_reset_poll(&service) == 1);
	emit(0, 0xcb000003);
	sail_check(sail_reset_poll(&service) == -EPROTO);
	begin();
	sail_check(!start());
	emit(0, 0xcb000003);
	sail_check(sail_reset_poll(&service) == 1);
	emit(0, 0xcc010000);
	sail_check(sail_reset_poll(&service) == -EPROTO);
	/* The secure boot notice is accepted once, on the secure channel only. */
	begin();
	sail_check(!start());
	emit(1, 0xcb000007);
	sail_check(sail_reset_poll(&service) == 1);
	emit(1, 0xcb000007);
	sail_check(sail_reset_poll(&service) == -EPROTO && !retires);
	begin();
	sail_check(!start());
	emit(0, 0xcb000007);
	sail_check(sail_reset_poll(&service) == -EPROTO && !retires);
	/* An SDI-disable notice before any reset claim is answered and keeps receiving. */
	begin();
	sail_check(!start());
	for (uint32_t i = 1; i <= 2; i++) {
		emit(1, 0xcd0a0000);
		sail_check(sail_reset_poll(&service) == 1 && early_replies == 4 * i && !replies);
		sail_check(!retires && !writes);
		sail_check(service.status.phase == SAIL_RESET_SERVICE_LISTENING);
		sail_check(channels[0].enabled && channels[1].enabled);
	}
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == 1 &&
		   service.status.phase == SAIL_RESET_SERVICE_ACKED && replies == 4 &&
		   early_replies == 8);
	/* A failed SDI answer, before a claim or after the reset answer, is terminal. */
	for (uint32_t i = 1; i <= 4; i++) {
		begin();
		sail_check(!start());
		fail_early = i;
		emit(1, 0xcd0a0000);
		sail_check(sail_reset_poll(&service) == -EFAULT && early_replies == i);
		sail_check(service.status.phase == SAIL_RESET_SERVICE_FAILED &&
		       service.status.sdi_answers == 0);
		sail_check(!channels[0].enabled && !channels[1].enabled && !retires);
		no_retry();
		begin();
		sail_check(!start());
		emit(1, 0xcd070000);
		sail_check(sail_reset_poll(&service) == 1 && replies == 4);
		fail_reply = 4 + i;
		emit(1, 0xcd0a0000);
		sail_check(sail_reset_poll(&service) == -EFAULT && replies == 4 + i);
		sail_check(service.status.phase == SAIL_RESET_SERVICE_FAILED &&
		       service.status.reply_completed == 4 && service.status.sdi_answers == 0);
		sail_check(!channels[0].enabled && !channels[1].enabled);
		no_retry();
	}
	/* The hypervisor boot notice likewise: once, on the non-secure channel only. */
	begin();
	sail_check(!start());
	emit(0, 0xcb00000f);
	sail_check(sail_reset_poll(&service) == 1);
	emit(0, 0xcb00000f);
	sail_check(sail_reset_poll(&service) == -EPROTO && !retires);
	begin();
	sail_check(!start());
	emit(1, 0xcb00000f);
	sail_check(sail_reset_poll(&service) == -EPROTO && !retires);
	/* A second queued event prevents retirement; loss/malformed history is sticky. */
	begin();
	sail_check(!start());
	emit(1, 0xcd070000);
	emit(0, 0xcc010000);
	sail_check(sail_reset_poll(&service) == -EALREADY && !retires);
	begin();
	sail_check(!start());
	for (uint32_t i = 0; i < 17; i++) {
		emit(1, 0xcd070000);
	}
	sail_check(sail_reset_poll(&service) == -ENOBUFS && !retires);
	begin();
	sail_check(!start());
	service.receiver.stats.malformed = 1;
	sail_check(sail_reset_poll(&service) == -EIO);
	begin();
	sail_check(!start());
	service.receiver.stats.callbacks[0] = UINT32_MAX;
	sail_check(sail_reset_poll(&service) < 0);
	/* Cover every control/PMIC boundary and initial, middle and final delay reads. */
	for (uint32_t i = 1; i <= domain_reads; i++) {
		if (i > delay_start + 2 && i < domain_reads - 20 && i != domain_reads / 2) {
			continue;
		}
		history_tests++;
		begin();
		sail_check(!start());
		inject_at = i;
		emit(1, 0xcd070000);
		sail_check(sail_reset_poll(&service) < 0 && !replies);
		no_retry();
	}
	for (uint32_t i = 1; i <= 8; i++) {
		begin();
		sail_check(!start());
		fail_read = i;
		emit(1, 0xcd070000);
		sail_check(sail_reset_poll(&service) < 0 && !replies);
	}
	for (uint32_t i = 1; i <= 2; i++) {
		begin();
		sail_check(!start());
		fail_write = i;
		emit(1, 0xcd070000);
		sail_check(sail_reset_poll(&service) < 0 && !replies &&
		       service.status.preparation.stores_attempted);
	}
	for (uint32_t i = 1; i <= 4; i++) {
		begin();
		sail_check(!start());
		fail_reply = i;
		emit(1, 0xcd070000);
		sail_check(sail_reset_poll(&service) < 0 && service.status.reply_attempted == i &&
		       service.status.reply_completed == i - 1);
		no_retry();
	}
	for (uint32_t i = 1; i <= 4; i++) {
		begin();
		fail_mbox = i;
		sail_check(start() < 0 && !writes && !replies);
		no_retry();
	}
	begin();
	sail_check(!start());
	retire_error = 1;
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == -EIO && !writes && !replies);
	begin();
	sail_check(!start());
	wrong_control = true;
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == -EIO && !replies);
	begin();
	sail_check(!start());
	bad_crc = true;
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == MONACO_PMIC_CRC && !replies);
	begin();
	sail_check(!start());
	stop_timer = true;
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) < 0 && !replies);
	begin();
	sail_check(!start());
	*reg(&hardware, 0xf0300044) = 1;
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == -ESTALE && !writes && !replies);
	begin();
	mode = SAIL_BOOT_NORMAL;
	sail_check(start() == -EPERM && mbox_calls == 0);
	begin();
	sail_check(!sail_reset_timer_enter(&service));
	sail_check(start() == -EBUSY);
	sail_reset_timer_leave(&service);
	sail_check(!start());
	test_thread = 2;
	sail_check(sail_reset_poll(&service) == -EPERM);
	sail_check(sail_reset_timer_enter(&service) == -EBUSY);
	test_thread = 1;
	test_isr = true;
	sail_check(sail_reset_poll(&service) == -EWOULDBLOCK);
	test_isr = false;
	test_irq_depth = 1;
	sail_check(sail_reset_poll(&service) == -EWOULDBLOCK);
	test_irq_depth = 0;
	sail_check(!sail_reset_stop(&service));
	sail_check(sail_reset_poll(&service) == -ESHUTDOWN);
	/* Preserve first error across cleanup failure; explicit stop only retries cleanup. */
	begin();
	sail_check(!start());
	fail_mbox = mbox_calls + 1;
	retire_error = -ECANCELED;
	emit(1, 0xcd070000);
	sail_check(sail_reset_poll(&service) == -ECANCELED);
	sail_check(service.status.error == -ECANCELED && service.status.cleanup_error == -EIO);
	uint32_t original_mbox_calls = mbox_calls;

	sail_check(sail_reset_poll(&service) == -ECANCELED && mbox_calls == original_mbox_calls);
	sail_check(!sail_reset_stop(&service) && service.status.error == -ECANCELED &&
	       service.status.cleanup_error == -EIO);
	shutdown_tests();
	/* Native allowlist, store widths and barriers; bus success is modeled. */
	replies = writes = barriers = 0;
	uint32_t value;
	const uint32_t addresses[] = {0xf1149024, 0xf1149020, 0xf1149420, 0xf0300044};

	for (size_t i = 0; i < 4; i++) {
		sail_check(!monaco_reset_read32(NULL, addresses[i], &value) && value == 0x12345678);
	}
	sail_check(monaco_reset_read32(NULL, 0xf1000404, &value) == -EINVAL);
	sail_check(!monaco_reset_write32(NULL, 0xf1149420, 0xcff8));
	sail_check(!monaco_reset_write32(NULL, 0xf0300044, 0x00400ea0));
	sail_check(monaco_reset_write32(NULL, 0xf1149420, 0xffff) == -EPERM);
	sail_check(monaco_reset_write32(NULL, 0xf0305094, 0) == -EPERM);
	for (uint32_t i = 0; i < 8; i++) {
		sail_check(!monaco_reset_reply(NULL, i < 4 ? 0xaa030000 : 0xaa0c0000, i % 4));
	}
	sail_check(monaco_reset_reply(NULL, 0xaa030000, 4) == -EINVAL);
	sail_check(monaco_reset_reply(NULL, 0xaa010000, 0) == -EINVAL);
	sail_check(replies == 8 && writes == 2 && barriers == 28);
	printf("PASS: actual coordinator/receive/preparation/CRC-PMIC/timer; %u history "
	       "boundaries; terminal/context/partial-effect failures; fixed adapters\n",
	       history_tests);
	return 0;
}
