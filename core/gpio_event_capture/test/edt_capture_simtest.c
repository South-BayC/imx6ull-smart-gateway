/* edt_capture_simtest.c —— 用户态守恒模拟测试（P3-4，替代 KUnit）
 * 手册 5.12.4：KUnit 需内核 5.5+，本项目 4.1.15 不可用，改用用户态模拟验证
 * 驱动核心性质（逻辑与 gpio_event_capture.c 的 enqueue 完全一致）：
 *   T1: FIFO 容量必须为 2 的幂（probe 校验）
 *   T2: 序列号守恒：queued + dropped == irq（FIFO 满时仍消耗序列号）
 *   T3: 溢出行为：小容量下大量注入 → dropped 递增且守恒仍成立
 * 运行：无需板子，任意主机编译运行（gcc edt_capture_simtest.c && ./a.out）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EV_SIZE 16   /* sizeof(struct edt_capture_event) == 16 */

/* ---- 模拟 kfifo（环形数组，容量 2 的幂，语义与内核 kfifo 一致）---- */
struct sim_fifo {
	unsigned char *buf;
	unsigned int   size;   /* 字节容量（2 的幂） */
	unsigned int   in;
	unsigned int   out;
};

static unsigned int sim_fifo_len(const struct sim_fifo *f)
{
	return f->in - f->out;
}

static unsigned int sim_fifo_avail(const struct sim_fifo *f)
{
	return f->size - sim_fifo_len(f);
}

static int sim_fifo_init(struct sim_fifo *f, unsigned int size)
{
	if (!size || (size & (size - 1)))   /* 2 的幂校验（T1 逻辑） */
		return -1;
	f->buf = malloc(size);
	if (!f->buf)
		return -1;
	f->size = size;
	f->in = f->out = 0;
	return 0;
}

static void sim_fifo_in(struct sim_fifo *f, unsigned int n)
{
	f->in += n;
}

/* ---- 统计结构（与 UAPI edt_capture_stats 的守恒字段对应）---- */
struct sim_stats {
	unsigned int  last_sequence;
	unsigned long long irq_events;
	unsigned long long queued_events;
	unsigned long long dropped_events;
};

/* 复刻 gpio_event_capture.c 的 IRQ 路径：
 * 序列号先消耗 → FIFO 有空间入队，否则 dropped（守恒：queued+dropped==irq） */
static void sim_irq(struct sim_fifo *f, struct sim_stats *st)
{
	unsigned int seq = st->last_sequence + 1;
	st->last_sequence = seq;
	st->irq_events++;
	if (sim_fifo_avail(f) >= EV_SIZE) {
		sim_fifo_in(f, EV_SIZE);
		st->queued_events++;
	} else {
		st->dropped_events++;
	}
}

static int g_fail;

#define CHECK(cond, msg) do {						\
	if (cond)							\
		printf("[PASS] %s\n", msg);				\
	else {								\
		printf("[FAIL] %s\n", msg);				\
		g_fail++;						\
	}								\
} while (0)

/* T1: FIFO 容量 2 的幂规则（probe 里 is_power_of_2 校验） */
static void test_fifo_depth_rule(void)
{
	struct sim_fifo f;

	printf("== T1: FIFO 容量规则 ==\n");
	CHECK(sim_fifo_init(&f, 0) != 0, "容量 0 → 拒绝");
	CHECK(sim_fifo_init(&f, 1000) != 0, "容量 1000（非 2 的幂）→ 拒绝");
	CHECK(sim_fifo_init(&f, 1024) == 0, "容量 1024（2 的幂）→ 接受");
	free(f.buf);
}

/* T2: 100k 事件注入，守恒 queued + dropped == irq */
static void test_sequence_conservation(void)
{
	struct sim_fifo f;
	struct sim_stats st = { 0 };
	int i;

	printf("== T2: 100k 事件序列号守恒 ==\n");
	sim_fifo_init(&f, 1024 * EV_SIZE);   /* 1024 事件容量 */
	for (i = 0; i < 100000; i++)
		sim_irq(&f, &st);

	printf("      irq=%llu queued=%llu dropped=%llu\n",
	       st.irq_events, st.queued_events, st.dropped_events);
	CHECK(st.irq_events == 100000, "irq_events == 100000");
	CHECK(st.queued_events + st.dropped_events == st.irq_events,
	      "queued + dropped == irq（守恒）");
	CHECK(st.last_sequence == 100000,
	      "序列号连续消耗（满时也消耗）");
	free(f.buf);
}

/* T3: 小容量溢出行为 */
static void test_overflow(void)
{
	struct sim_fifo f;
	struct sim_stats st = { 0 };
	int i;

	printf("== T3: 溢出行为（容量 4 事件，注入 100 次）==\n");
	sim_fifo_init(&f, 4 * EV_SIZE);
	for (i = 0; i < 100; i++)
		sim_irq(&f, &st);

	printf("      irq=%llu queued=%llu dropped=%llu\n",
	       st.irq_events, st.queued_events, st.dropped_events);
	CHECK(st.dropped_events > 0, "溢出 → dropped_events > 0");
	CHECK(st.queued_events == 4, "容量 4 → 最多入队 4（快照逻辑同）");
	CHECK(st.queued_events + st.dropped_events == st.irq_events,
	      "溢出下守恒仍成立");
	CHECK(st.last_sequence == 100, "序列号仍连续（满时消耗）");
	free(f.buf);
}

int main(void)
{
	test_fifo_depth_rule();
	test_sequence_conservation();
	test_overflow();
	printf("== simtest done: %s ==\n", g_fail ? "FAIL" : "ALL PASS");
	return g_fail ? 1 : 0;
}