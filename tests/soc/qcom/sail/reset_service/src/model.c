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
	int rc = read32(ctx, a, v);

	if (active && rc == 0 && a == SE + 0x780 && bad_crc) {
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

	/* A replied reset completes on the SDI-disable notice. */
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
