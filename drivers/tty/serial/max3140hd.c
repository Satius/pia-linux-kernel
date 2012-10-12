/*
 *
 *  Copyright (C) 2012 Bjoern Krombholz <b.krombholz@pironex.de>
 *    based on max3100.c by Christian Pellegrin <chripell@evolware.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *
 * Notes: The MAX3140 is basically a MAX3100 combined with an RS485 driver
 *        the MAX3140 is using RTS signal to control DE of RS485 driver
 *        real flow control is not supported
 *        we need to use calculate on bus end of TX time to disable the
 *        driver immediately
 *
 * Example platform data:

 static struct plat_max3140hd max3140_plat_data = {
 .loopback = 0,
 .crystal = 0,
 .poll_time = 10,
 };

 static struct spi_board_info spi_board_info[] = {
 {
 .modalias	= "max3140hd",
 .platform_data	= &max3140_plat_data,
 .irq		= IRQ_EINT12,
 .max_speed_hz	= 4*1000*1000,
 .chip_select	= 0,
 },
 };

 * The initial minor number is 209 in the low-density serial port:
 * mknod /dev/ttyMAX0 c 204 209
 * This is shared with the max3100 driver.
 */

#define MAX3100_MAJOR 204
#define MAX3100_MINOR 209
/* 4 MAX3100s should be enough for everyone */
#define MAX_MAX3140 4

#define THREADED
#define DEBUG
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/spi/spi.h>
#include <linux/freezer.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#include <linux/kthread.h>
#include <linux/serial_max3140hd.h>

#define MAX3100_C    (1<<14)
#define MAX3100_D    (0<<14)
#define MAX3100_W    (1<<15)
#define MAX3100_RX   (0<<15)

#define MAX3100_WC   (MAX3100_W  | MAX3100_C)
#define MAX3100_RC   (MAX3100_RX | MAX3100_C)
#define MAX3100_WD   (MAX3100_W  | MAX3100_D)
#define MAX3100_RD   (MAX3100_RX | MAX3100_D)
#define MAX3100_CMD  (3 << 14)

#define MAX3100_T    (1<<14)
#define MAX3100_R    (1<<15)

#define MAX3100_FEN  (1<<13)
#define MAX3100_SHDN (1<<12)
#define MAX3100_TM   (1<<11)
#define MAX3100_RM   (1<<10)
#define MAX3100_PM   (1<<9)
#define MAX3100_RAM  (1<<8)
#define MAX3100_IR   (1<<7)
#define MAX3100_ST   (1<<6)
#define MAX3100_PE   (1<<5)
#define MAX3100_L    (1<<4)
#define MAX3100_BAUD (0xf)

#define MAX3100_TE   (1<<10)
#define MAX3100_RAFE (1<<10)
#define MAX3100_RTS  (1<<9)
#define MAX3100_CTS  (1<<9)
#define MAX3100_PT   (1<<8)
#define MAX3100_DATA (0xff)

#define MAX3100_RT   (MAX3100_R | MAX3100_T)
#define MAX3100_RTC  (MAX3100_RT | MAX3100_CTS | MAX3100_RAFE)

/* the following simulate a status reg for ignore_status_mask */
#define MAX3100_STATUS_PE 1
#define MAX3100_STATUS_FE 2
#define MAX3100_STATUS_OE 4

#define MAX3100_RX_FIFOLEN 8
#define MAX3100_TX_FIFOLEN 2
#define MAX3140_WORDSIZE   2 /* size in bytes per spi word */

#define MAX3140_READ_SINGLE

struct max3140hd_port {
	struct uart_port port;
	struct spi_device *spi;
	const char *name;
	u16 spi_rxbuf[MAX3100_RX_FIFOLEN];
	u16 spi_txbuf[MAX3100_RX_FIFOLEN];
	struct spi_message *spi_msg;

#define BIT_DRIVER_DISABLE	1
#define BIT_IRQ_PENDING		2
//#define BIT_RX_PENDING		3
#define BIT_TX_STARTED		4
#define BIT_CONF_COMMIT		5
	unsigned long irqflags;
#define BIT_WAIT_FOR_DD		0
#define BIT_RX_PENDING		1
#define BIT_TX_EMPTY		2
	unsigned long runflags;

	int cts;	        /* last CTS received for flow ctrl */
//	int tx_empty;		/* last TX empty bit */
	spinlock_t conf_lock;	/* shared data */
	int conf;		/* configuration for the MAX31000
				 * (bits 0-7, bits 8-11 are irqs) */
//	int rts_commit;	        /* need to change rts */
//	int rts;		/* rts status */
	int baud;		/* current baud rate */

	int parity;		/* keeps track if we should send parity */
#define MAX3100_PARITY_ON 1
#define MAX3100_PARITY_ODD 2
#define MAX3100_7BIT 4
	int rx_enabled;	        /* if we should rx chars */
	int rx_count;           /* track number of chars in rx flip buffer */

	int irq;		/* irq assigned to the max3100 */

	int minor;               /* minor number */
	int crystal;             /* 1 if 3.6864Mhz crystal 0 for 1.8432 */
	int loopback;            /* 1 if we are in loopback mode */
	int invert_rts;          /* 1 if RTS output logic is inverted */

	/* time stamps to calculate precise driver disable timeout */
	s64 rts_sleep;           /* wait time for rts release, driver disable */
	struct timespec prev_ts;
	struct timespec now_ts;
	int dd;
	wait_queue_head_t wq_dd;

	int force_end_work; /* 1 to force stop */
	int suspending; /* 1 when suspend started, 0 when resumed */

	/* timers */
	int poll_time;
	struct timer_list	poll_timer;
	struct hrtimer drv_dis_timer;

	wait_queue_head_t wq;
	struct task_struct *main_thread;
	struct mutex thread_mutex;
};

static struct max3140hd_port *max3140s[MAX_MAX3140]; /* the chips */
static DEFINE_MUTEX(max3140s_lock);		   /* race on probe */

static inline int max3140_do_parity(struct max3140hd_port *s, u16 c)
{
	int parity;

	if (s->parity & MAX3100_PARITY_ODD)
		parity = 1;
	else
		parity = 0;

	if (s->parity & MAX3100_7BIT)
		c &= 0x7f;
	else
		c &= 0xff;

	parity = parity ^ (hweight8(c) & 1);
	return parity;
}

static inline int max3140_check_parity(struct max3140hd_port *s, u16 c)
{
	return max3140_do_parity(s, c) == ((c >> 8) & 1);
}

static inline void max3140_calc_parity(struct max3140hd_port *s, u16 *c)
{
	if (s->parity & MAX3100_7BIT)
		*c &= 0x7f;
	else
		*c &= 0xff;

	if (s->parity & MAX3100_PARITY_ON)
		*c |= max3140_do_parity(s, *c) << 8;
}

static void max3140_timeout(unsigned long data)
{
	struct max3140hd_port *s = (struct max3140hd_port *)data;
	int timer_state = -1;
	if (s->port.state) {
		if (!test_and_set_bit(BIT_IRQ_PENDING, &s->irqflags));
			wake_up_process(s->main_thread);
		timer_state = mod_timer(&s->poll_timer, jiffies + s->poll_time);
		dev_dbg(&s->spi->dev, "poll timeout %lu\n", jiffies + s->poll_time);
	}
}

#ifndef MAX3140_READ_SINGLE
static int max3140_sr(struct max3140hd_port *s, const void *txbuf, void *rxbuf,
                      unsigned len)
{
	struct spi_device *spi = s->spi;
	struct spi_message message;
	struct spi_transfer x[len];
	int ret, i;
	u16 *rx = rxbuf;
	const u16 *tx = txbuf;

	spi_message_init(&message);
	memset(&x, 0, (sizeof(x[0]) * len));
	for (i = 0; i < len; ++i) {
		x[i].len = MAX3140_WORDSIZE;
		x[i].tx_buf = &tx[i];
		x[i].rx_buf = &rx[i];
		x[i].speed_hz = spi->max_speed_hz;
		x[i].cs_change = 1;
		spi_message_add_tail(&x[i], &message);
	}

	ret = spi_sync(spi, &message);
	if (!ret) {
		s->tx_empty = (rx[i-1] & MAX3100_T);
		if (rx[i-1] & MAX3100_R)
			set_bit(BIT_RX_PENDING, &s->runflags);
		else
			clear_bit(BIT_RX_PENDING, &s->runflags);
	} else {
		dev_warn(&s->spi->dev, "%s: error %d\n", __func__, ret);
	}

	return ret;
}
#endif /* MAX3140_READ_SINGLE */

static int max3140_sr1(struct max3140hd_port *s, const void *txbuf, void *rxbuf)
{
	struct spi_device *spi = s->spi;
	struct spi_message	message;
	struct spi_transfer	x;
	u16 *rx = rxbuf;
	int ret;

	spi_message_init(&message);
	memset(&x, 0, sizeof x);
	x.len = MAX3140_WORDSIZE;
	x.tx_buf = txbuf;
	x.rx_buf = rxbuf;
	x.speed_hz = spi->max_speed_hz;
	//x.cs_change = 1;
	spi_message_add_tail(&x, &message);

	ret = spi_sync(spi, &message);

	if (!ret) {
		if (*rx & MAX3100_T)
			set_bit(BIT_TX_EMPTY, &s->runflags);
		else
			clear_bit(BIT_TX_EMPTY, &s->runflags);
		//s->tx_empty = ((*rx & MAX3100_T) > 0);
		if (*rx & MAX3100_R)
			set_bit(BIT_RX_PENDING, &s->runflags);
		else
			clear_bit(BIT_RX_PENDING, &s->runflags);
	} else {
		dev_warn(&s->spi->dev, "%s: error %d\n", __func__, ret);
	}

	return ret;
}

static void max3140_receive_chars(struct max3140hd_port *s, unsigned char *str, int len)
{
	struct uart_port *port = &s->port;
	struct tty_struct *tty;
	int usable;

	/* If uart is not opened, just return */
	if (!port->state)
		return;

	tty = port->state->port.tty;
	if (!tty)
		return;

	while (len) {
		usable = tty_buffer_request_room(tty, len);
		if (usable) {
			tty_insert_flip_string(tty, str, usable);
			str += usable;
			port->icount.rx += usable;
		}
		len -= usable;
	}
	//tty_flip_buffer_push(tty);
}

/* send single command to max3140 */
static inline int max3140_cmd(struct max3140hd_port *s, u16 tx)
{
	u16 obuf, ibuf;
	u16 rx;
	u8 ch;
	int ret;
	struct tty_struct *tty;

	obuf = tx;
	ret = max3140_sr1(s, &obuf, &ibuf);
	//dev_dbg(&s->spi->dev, "%s: t0x%04x i0x%04x\n", __func__, tx, ibuf);
	if (ret) {
		dev_warn(&s->spi->dev, "%s: error %d while sending 0x%x\n",
				__func__, ret, obuf);
		//goto exit;
		return ret;
	}

	rx = ibuf;

	/* If some valid data is read back */
	if (test_bit(BIT_RX_PENDING, &s->runflags)) {
		ch = rx & 0xff;
		max3140_receive_chars(s, &ch, 1);
		tty = s->port.state->port.tty;
		if (tty)
			tty_flip_buffer_push(tty);
	}

	return 0;
//exit:
//	kfree(buf);
//	return ret;
}

#ifndef MAX3140_READ_SINGLE
static void spidev_complete(void *arg)
{
	struct max3140hd_port *s = arg;
	struct spi_message *m = s->spi_msg;
	int status = m->status;
	struct spi_transfer *spi_tran = list_first_entry(&m->transfers,
			struct spi_transfer, transfer_list);
	if (status == 0) {
		status = m->actual_length;
		dev_dbg(&s->spi->dev, "RTS timeout done\n");
	} else {
		dev_warn(&s->spi->dev, "%s: couldn't send %d\n", __func__,
				status);
	}

	kfree(spi_tran->rx_buf);
	kfree(spi_tran->tx_buf);
	spi_message_free(m);
}
#endif

#define MAX3100_SETRTS(r) \
	(r ? (s->invert_rts ? 0 : MAX3100_RTS) : (s->invert_rts ? MAX3100_RTS : 0))
static enum hrtimer_restart max3140_drv_dis_handler(struct hrtimer *handle)
{
	struct max3140hd_port *s =
			container_of(handle, struct max3140hd_port, drv_dis_timer);
#if 0
	if (!test_and_set_bit(BIT_DRIVER_DISABLE, &s->irqflags))
		wake_up_process(s->main_thread);
#elif 1
	s->dd = 1;
	wake_up(&s->wq_dd);
#else
	//DECLARE_COMPLETION_ONSTACK(done);
	struct spi_transfer *spi_tran;
	struct spi_message *spi_msg = spi_message_alloc(1, GFP_ATOMIC);
	u16 *rx, *tx;
	s->spi_msg = spi_msg;

	int status;
	if (!spi_msg) {
		return HRTIMER_RESTART;
	}
	spi_tran = list_first_entry(&spi_msg->transfers,
			struct spi_transfer, transfer_list);
	rx = kzalloc(2, GFP_ATOMIC);
	tx = kzalloc(2, GFP_ATOMIC);
	spi_tran->tx_buf = tx;
	spi_tran->rx_buf = rx;
	spi_tran->len = 2;
	spi_msg->context = s;
	spi_msg->complete = spidev_complete;

	*tx = MAX3100_WD | MAX3100_TE | MAX3100_SETRTS(1);

	status = spi_async(s->spi, spi_msg);
	if (status < 0) {
		dev_warn(&s->spi->dev, "error while calling sr_async2\n");

		return HRTIMER_RESTART;
	}
#endif
	return HRTIMER_NORESTART;
}

static int max3140_read_fifo(struct max3140hd_port *s)
{
	int i, j = 0;
	//u16 *obuf;
	u16 *ibuf;
	u16 cur;
	u8 str[MAX3100_RX_FIFOLEN];
	int len = MAX3100_RX_FIFOLEN * sizeof (u16);

	//dev_dbg(&s->spi->dev, "%s\n", __func__);

	memset(s->spi_txbuf, 0, len);
	memset(s->spi_rxbuf, 0, len);

	ibuf = s->spi_rxbuf;
#ifdef MAX3140_READ_SINGLE
	for (i = 0; i < MAX3100_RX_FIFOLEN; ++i) {
		if (max3140_sr1(s, &s->spi_txbuf[i], &s->spi_rxbuf[i]/*, 2 len*/)) {
			return 0;
		}
		cur = *ibuf;
		if (test_bit(BIT_RX_PENDING, &s->runflags)) {
			str[j++] = cur & 0xff;
		} else {
			break;
		}

		ibuf++;
		schedule();
	}
#else
	if (max3140_sr(s, s->spi_txbuf, s->spi_rxbuf, MAX3100_RX_FIFOLEN)) {
		dev_warn(&s->spi->dev, "%s: read error \n", __func__);
		return 0;
	}
	for (i = 0; i < MAX3100_RX_FIFOLEN; ++i) {
		cur = *ibuf;
		if (cur & MAX3100_R) {
			str[j++] = cur & 0xff;
		}

		ibuf++;
	}

#endif

	if (j) {
		max3140_receive_chars(s, str, j);
	}
	if (j > 6)
		dev_dbg(&s->spi->dev, "%s cnt: %d, tx_empty %d\n", __func__, j,
			test_bit(BIT_TX_EMPTY, &s->runflags));

	return j;
}
static int max3140_send_and_receive(struct max3140hd_port *s)
{
	struct timespec now, start, end;
	int rxchars, rxlen;
	int txchars = 0;
	u16 tx, rx;
	u8 crx;
	struct circ_buf *xmit = &s->port.state->xmit;
	s64 diff_ns;

	//dev_dbg(&s->spi->dev, "%s\n", __func__);
	rxchars = 0;
	rxlen = 0;
	getrawmonotonic(&start);

	do {
		txchars = 0;
		rxchars += max3140_read_fifo(s);
//		while (!test_bit(BIT_DRIVER_DISABLE, &s->flags) &&
//				!uart_circ_empty(xmit) &&

//		if (test_bit(BIT_DRIVER_DISABLE, &s->irqflags) ||
//			uart_circ_empty(xmit) ||
//			!test_bit(BIT_TX_EMPTY, &s->runflags)) {
//			continue;
//		} else {

		if (test_bit(BIT_TX_EMPTY, &s->runflags)) {
		tx = 0xffff;
		if (s->port.x_char) {
			tx = s->port.x_char;
			s->port.icount.tx++;
			s->port.x_char = 0;
		} else if (!uart_circ_empty(xmit) && !uart_tx_stopped(&s->port)) {
			tx = xmit->buf[xmit->tail];
			xmit->tail = (xmit->tail + 1) &
					(UART_XMIT_SIZE - 1);
			s->port.icount.tx++;
		}

		if (tx != 0xffff) {
			//char o =tx;
			max3140_calc_parity(s, &tx);
			// force driver on while sending
			tx |= MAX3100_WD | MAX3100_SETRTS(0);
			max3140_sr1(s, &tx, &rx);
			/* get the time right after SPI transfer finished
			 * it is sufficient because of the slow RS485 speeds */
			getrawmonotonic(&now);
			txchars++;

			//dev_dbg(&s->spi->dev, "TX:%04x RX:%04x\n", tx, rx);
			s->prev_ts.tv_nsec = s->now_ts.tv_nsec;
			s->prev_ts.tv_sec = s->now_ts.tv_sec;

			// prev_ts not finished
			if (timespec_compare(&s->prev_ts, &now) > 0) {
				// prev byte not yet complete
				// now becomes prev_ts as base of last byte delay
				//dev_dbg(&s->spi->dev, ">%02x\n", (u8)o);
				s->now_ts.tv_nsec = s->prev_ts.tv_nsec;
				s->now_ts.tv_sec = s->prev_ts.tv_sec;
			} else {
				//	dev_dbg(&s->spi->dev, "<%02x\n", (u8)o);
				s->now_ts.tv_nsec = now.tv_nsec;
				s->now_ts.tv_sec = now.tv_sec;
			}
			// 1 byte delay + rest of previous byte
			//dev_dbg(&s->spi->dev, "old:%lld\n", timespec_to_ns(&s->now_ts));
			timespec_add_ns(&s->now_ts, s->rts_sleep);
			//dev_dbg(&s->spi->dev, "net:%lld\n", timespec_to_ns(&s->now_ts));

			// wait until all data sent
			if (uart_circ_empty(xmit) ||
					uart_tx_stopped(&s->port)) {
				getrawmonotonic(&now);
				diff_ns = timespec_to_ns(&s->now_ts) -
						timespec_to_ns(&now);
				if (diff_ns > 0) {
#if 1
					ndelay(diff_ns);
#else
					hrtimer_start(&s->drv_dis_timer,
							ktime_set(0, diff_ns),
							HRTIMER_MODE_REL);
					wait_event(s->wq_dd, s->dd != 0);
#endif
					//dev_dbg(&s->spi->dev, "ns_diff:%lld, %lld, %lld.%lld\n",
					//        diff_ns, s->rts_sleep, timespec_to_ns(&s->now_ts),
					//timespec_to_ns(&now));
				}
				max3140_cmd(s, MAX3100_WD | MAX3100_TE |
						MAX3100_SETRTS(1));
				s->dd = 0;
				//clear_bit(BIT_TX_STARTED, &s->flags);
			}
			/* unlikely to receive something here
			 * keep RX_PENDING as we only read one byte */
			if (test_bit(BIT_RX_PENDING, &s->runflags)) {
				crx = rx & 0xff;
				rxchars++;
				max3140_receive_chars(s, &crx, 1);
			}
		}
		}
		/* don't read if TX active */
		//if (txchars == 0 || test_bit(BIT_RX_PENDING, &s->flags))

		if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
			uart_write_wakeup(&s->port);

		if (rxchars > 16 && s->port.state->port.tty != NULL) {
			tty_flip_buffer_push(s->port.state->port.tty);
			rxchars = 0;
		}
	} while (!test_bit(BIT_DRIVER_DISABLE, &s->irqflags) &&
			!s->force_end_work &&
			!freezing(current) &&
		 ((test_bit(BIT_RX_PENDING, &s->runflags)) ||
		  (test_bit(BIT_TX_EMPTY, &s->runflags) &&
		   !uart_circ_empty(xmit) &&
		   !uart_tx_stopped(&s->port))));

	if (rxchars > 0 && s->port.state->port.tty != NULL) {
		tty_flip_buffer_push(s->port.state->port.tty);
	}
	getrawmonotonic(&end);
//	dev_dbg(&s->spi->dev, "%s %ld.%ld - %ld.%ld\n", __func__,
//	        start.tv_sec, start.tv_nsec,
//	        end.tv_sec, end.tv_nsec);

	return 0;
}
static inline void max3140_driver_disable(struct max3140hd_port *s, u8 dis)
{
	u16 cmd = MAX3100_WD | MAX3100_TE | MAX3100_SETRTS(dis);
	max3140_cmd(s, cmd);
	//dev_dbg(&s->spi->dev, "%s\n", __func__);
}

static int max3140_main_thread(void *_max)
{
	struct max3140hd_port *s = _max;
	wait_queue_head_t *wq = &s->wq;
	int ret = 0;

	init_waitqueue_head(wq);
	dev_dbg(&s->spi->dev, "%s\n", __func__);

	do {
		wait_event_interruptible(*wq, s->irqflags || kthread_should_stop());

		mutex_lock(&s->thread_mutex);

		if (test_and_clear_bit(BIT_DRIVER_DISABLE, &s->irqflags)) {
			//max3140_driver_disable(s, 1);
			max3140_cmd(s, MAX3100_WD | MAX3100_TE | MAX3100_SETRTS(1));
			dev_dbg(&s->spi->dev, "dd\n");
		}
		if (test_and_clear_bit(BIT_CONF_COMMIT, &s->irqflags)) {
			max3140_cmd(s, MAX3100_WC | s->conf);
			dev_dbg(&s->spi->dev, "cc\n");
		}
		if (test_and_clear_bit(BIT_IRQ_PENDING, &s->irqflags)) {
			//dev_dbg(&s->spi->dev, "irq\n");
			max3140_send_and_receive(s);
		}
		if ((test_and_clear_bit(BIT_TX_STARTED, &s->irqflags)) /*||
				test_and_clear_bit(BIT_IRQ_PENDING, &s->flags)*/) {
			//clear_bit(BIT_IRQ_PENDING, &s->flags);
			max3140_send_and_receive(s);
			//dev_dbg(&s->spi->dev, "tx\n");
		}
		if (test_and_clear_bit(BIT_RX_PENDING, &s->runflags))
			set_bit(BIT_IRQ_PENDING, &s->irqflags);

		mutex_unlock(&s->thread_mutex);
		//dev_dbg(&s->spi->dev, "%s %x %x\n", __func__,s->irqflags, s->runflags);

	} while (!kthread_should_stop());

	return ret;
}

static irqreturn_t max3140_irq(int irq, void *dev_id)
{
	struct max3140hd_port *s = dev_id;
	int w;

	/* max3140's irq is a falling edge, not level triggered,
	 * so no need to disable the irq */
	set_bit(BIT_IRQ_PENDING, &s->irqflags);
	w = wake_up_process(s->main_thread);
	//printk("w%d\n", w);

	return IRQ_HANDLED;
}

static void max3140_enable_ms(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	if (s->poll_time > 0)
		mod_timer(&s->poll_timer, jiffies);
	dev_dbg(&s->spi->dev, "%s %d\n", __func__, s->poll_time);
}

static void max3140_start_tx(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	if (!test_and_set_bit(BIT_TX_STARTED, &s->irqflags))
		wake_up_process(s->main_thread);
}

static void max3140_stop_rx(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	s->rx_enabled = 0;
	s->conf &= ~MAX3100_RM;
	if (!test_and_set_bit(BIT_CONF_COMMIT, &s->irqflags))
		wake_up_process(s->main_thread);
}

static unsigned int max3140_tx_empty(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	/* may not be truly up-to-date */
	//FIXME remove max3100_dowork(s);
	return test_bit(BIT_TX_EMPTY, &s->runflags);
}

static unsigned int max3140_get_mctrl(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	/* may not be truly up-to-date */
	// FIXME remove max3100_dowork(s);
	/* always assert DCD and DSR since these lines are not wired */
	// FIXME check if we need to evaluate cts
	return (s->cts ? TIOCM_CTS : 0) | TIOCM_DSR | TIOCM_CAR;
}

static void max3140_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s NOOP\n", __func__);
}

static void
max3140_set_termios(struct uart_port *port, struct ktermios *termios,
		    struct ktermios *old)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);
	int baud = 0;
	unsigned cflag;
	int bits_per_byte = 10;
	u32 param_new, param_mask, parity = 0;

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	cflag = termios->c_cflag;
	param_new = 0;
	param_mask = 0;

	baud = tty_termios_baud_rate(termios);
	param_new = s->conf & MAX3100_BAUD;
	switch (baud) {
	case 300:
		if (s->crystal)
			baud = s->baud;
		else
			param_new = 15;
		break;
	case 600:
		param_new = 14 + s->crystal;
		break;
	case 1200:
		param_new = 13 + s->crystal;
		break;
	case 2400:
		param_new = 12 + s->crystal;
		break;
	case 4800:
		param_new = 11 + s->crystal;
		break;
	case 9600:
		param_new = 10 + s->crystal;
		break;
	case 19200:
		param_new = 9 + s->crystal;
		break;
	case 38400:
		param_new = 8 + s->crystal;
		break;
	case 57600:
		param_new = 1 + s->crystal;
		break;
	case 115200:
		param_new = 0 + s->crystal;
		break;
	case 230400:
		if (s->crystal)
			param_new = 0;
		else
			baud = s->baud;
		break;
	default:
		baud = s->baud;
	}
	tty_termios_encode_baud_rate(termios, baud, baud);
	s->baud = baud;
	param_mask |= MAX3100_BAUD;

	if ((cflag & CSIZE) == CS8) {
		param_new &= ~MAX3100_L;
		parity &= ~MAX3100_7BIT;
	} else {
		param_new |= MAX3100_L;
		parity |= MAX3100_7BIT;
		cflag = (cflag & ~CSIZE) | CS7;
		bits_per_byte--;
	}
	param_mask |= MAX3100_L;

	if (cflag & CSTOPB) {
		param_new |= MAX3100_ST;
		bits_per_byte++;
	} else {
		param_new &= ~MAX3100_ST;
	}
	param_mask |= MAX3100_ST;

	if (cflag & PARENB) {
		param_new |= MAX3100_PE;
		parity |= MAX3100_PARITY_ON;
		bits_per_byte++;
	} else {
		param_new &= ~MAX3100_PE;
		parity &= ~MAX3100_PARITY_ON;
	}
	param_mask |= MAX3100_PE;

	if (cflag & PARODD)
		parity |= MAX3100_PARITY_ODD;
	else
		parity &= ~MAX3100_PARITY_ODD;

	// pause after last tx byte depending on bits per byte
	s->rts_sleep = 1000000*bits_per_byte/baud*1000;
	dev_notice(&s->spi->dev, "Sleep setup for %d bits/byte\n", bits_per_byte);

	/* mask termios capabilities we don't support */
	cflag &= ~CMSPAR;
	termios->c_cflag = cflag;

	s->port.ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		s->port.ignore_status_mask |=
			MAX3100_STATUS_PE | MAX3100_STATUS_FE |
			MAX3100_STATUS_OE;

	/* we are sending char from a workqueue so enable */
	s->port.state->port.tty->low_latency = 1;

	if (s->poll_time > 0)
		del_timer_sync(&s->poll_timer);

	uart_update_timeout(port, termios->c_cflag, baud);

	spin_lock(&s->conf_lock);
	s->conf = (s->conf & ~param_mask) | (param_new & param_mask);
	s->parity = parity;
	spin_unlock(&s->conf_lock);

	set_bit(BIT_CONF_COMMIT, &s->irqflags);
	set_bit(BIT_DRIVER_DISABLE, &s->irqflags);
	wake_up_process(s->main_thread);

	if (UART_ENABLE_MS(&s->port, termios->c_cflag))
		max3140_enable_ms(&s->port);
}

static void max3140_shutdown(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);
	u16 tx;

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	if (s->suspending)
		return;

	s->force_end_work = 1;
	mutex_lock(&s->thread_mutex);

	s->irqflags = 0;
	if (s->poll_time > 0)
		del_timer_sync(&s->poll_timer);
	hrtimer_cancel(&s->drv_dis_timer);

	if (s->irq)
		free_irq(s->irq, s);

	mutex_unlock(&s->thread_mutex);
	/* set shutdown mode to save power */
	tx = (MAX3100_WC | s->conf | MAX3100_SHDN);
	max3140_cmd(s, tx);
	//set_bit(BIT_CONF_COMMIT, &s->flags);
	//wake_up_process(s->main_thread);
}

static int max3140_startup(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	s->conf = MAX3100_RM | MAX3100_TM;
	s->baud = s->crystal ? 230400 : 115200;
	s->rx_enabled = 1;

	if (s->suspending)
		return 0;

	s->force_end_work = 0;
	s->parity = 0;

	if (request_irq(s->irq, max3140_irq,
			IRQF_TRIGGER_FALLING, s->name, s) < 0) {
		dev_warn(&s->spi->dev, "cannot allocate irq %d\n", s->irq);
		s->irq = 0;
		return -EBUSY;
	}

	if (s->loopback) {
		u16 tx = 0x4001;
		max3140_cmd(s, tx);
	}

	set_bit(BIT_CONF_COMMIT, &s->irqflags);
	wake_up_process(s->main_thread);
	//max3140_cmd(s, s->conf | MAX3100_WC);
	/* wait for clock to settle */
	msleep(50);

	max3140_enable_ms(&s->port);

	return 0;
}

static const char *max3140_type(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	return s->port.type == PORT_MAX3100 ? "MAX3100" : NULL;
}

static void max3140_release_port(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s NOOP\n", __func__);
}

static void max3140_config_port(struct uart_port *port, int flags)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	if (flags & UART_CONFIG_TYPE)
		s->port.type = PORT_MAX3100;
}

static int max3140_verify_port(struct uart_port *port,
			       struct serial_struct *ser)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);
	int ret = -EINVAL;

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	if (ser->type == PORT_UNKNOWN || ser->type == PORT_MAX3100)
		ret = 0;
	return ret;
}

static void max3140_stop_tx(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s NOOP\n", __func__);
}

static int max3140_request_port(struct uart_port *port)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s NOOP\n", __func__);
	return 0;
}

static void max3140_break_ctl(struct uart_port *port, int break_state)
{
	struct max3140hd_port *s = container_of(port,
					      struct max3140hd_port,
					      port);

	dev_dbg(&s->spi->dev, "%s NOOP\n", __func__);
}

static struct uart_ops max3100_ops = {
	.tx_empty     = max3140_tx_empty,
	.set_mctrl    = max3140_set_mctrl,
	.get_mctrl    = max3140_get_mctrl,
	.stop_tx      = max3140_stop_tx,
	.start_tx     = max3140_start_tx,
	.stop_rx      = max3140_stop_rx,
	.enable_ms    = max3140_enable_ms,
	.break_ctl    = max3140_break_ctl,
	.startup      = max3140_startup,
	.shutdown     = max3140_shutdown,
	.set_termios  = max3140_set_termios,
	.type         = max3140_type,
	.release_port = max3140_release_port,
	.request_port = max3140_request_port,
	.config_port  = max3140_config_port,
	.verify_port  = max3140_verify_port,
};

static struct uart_driver max3140hd_uart_driver = {
	.owner          = THIS_MODULE,
	.driver_name    = "ttyMAX",
	.dev_name       = "ttyMAX",
	.major          = MAX3100_MAJOR,
	.minor          = MAX3100_MINOR,
	.nr             = MAX_MAX3140,
};
static int uart_driver_registered;

static int __devinit max3140_probe(struct spi_device *spi)
{
	int i, retval;
	struct plat_max3140hd *pdata;
	char name[16];
	u16 tx;
	/* make sure we handle interrupts as soon as possible
	 * in chip FIFO is too short to handle even short delays */
	struct sched_param scheduler_param = { .sched_priority = 50 };
	//struct sched_param scheduler_param_dd = { .sched_priority = 99 };

	mutex_lock(&max3140s_lock);

	/* driver basics */
	if (!uart_driver_registered) {
		uart_driver_registered = 1;
		retval = uart_register_driver(&max3140hd_uart_driver);
		if (retval) {
			printk(KERN_ERR "Couldn't register max3100 uart driver\n");
			mutex_unlock(&max3140s_lock);
			return retval;
		}
	}

	for (i = 0; i < MAX_MAX3140; i++)
		if (!max3140s[i])
			break;
	if (i == MAX_MAX3140) {
		dev_warn(&spi->dev, "too many MAX3100 chips\n");
		mutex_unlock(&max3140s_lock);
		return -ENOMEM;
	}

	max3140s[i] = kzalloc(sizeof(struct max3140hd_port), GFP_KERNEL);
	if (!max3140s[i]) {
		dev_warn(&spi->dev,
			 "kmalloc for max3100 structure %d failed!\n", i);
		mutex_unlock(&max3140s_lock);
		return -ENOMEM;
	}
	snprintf(name, 16, "max3140-%d", i);
	max3140s[i]->name = kstrndup(name, 16, GFP_KERNEL);

	/* SPI basics */
	spi->bits_per_word = 16;
	max3140s[i]->spi = spi;

	/* main thread */
	mutex_init(&max3140s[i]->thread_mutex);
	max3140s[i]->irqflags = 0;
	max3140s[i]->main_thread = kthread_run(max3140_main_thread,
					max3140s[i], "max3140_main");
	if (IS_ERR(max3140s[i]->main_thread)) {
		int ret = PTR_ERR(max3140s[i]->main_thread);
		return ret;
	}
	if (sched_setscheduler(max3140s[i]->main_thread, SCHED_FIFO,
			&scheduler_param))
		dev_warn(&spi->dev, "Error setting scheduler, using default.\n");

	/* interrupt */
	max3140s[i]->irq = spi->irq;

	/* driver options */
	spin_lock_init(&max3140s[i]->conf_lock);
	dev_set_drvdata(&spi->dev, max3140s[i]);
	pdata = spi->dev.platform_data;
	max3140s[i]->crystal = pdata->crystal;
	max3140s[i]->loopback = pdata->loopback;
	max3140s[i]->invert_rts = pdata->invert_rts;
	max3140s[i]->minor = i;
	max3140s[i]->dd = 0;
	init_waitqueue_head(&max3140s[i]->wq_dd);

	/* poll timer */
	max3140s[i]->poll_time = pdata->poll_time * HZ / 1000;
	if (pdata->poll_time > 0 && max3140s[i]->poll_time == 0)
		max3140s[i]->poll_time = 1;

	init_timer(&max3140s[i]->poll_timer);
	max3140s[i]->poll_timer.function = max3140_timeout;
	max3140s[i]->poll_timer.data = (unsigned long) max3140s[i];

	/* driver disable HR timer */
	hrtimer_init(&max3140s[i]->drv_dis_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	max3140s[i]->drv_dis_timer.function = max3140_drv_dis_handler;

	dev_dbg(&spi->dev, "%s: adding port %d\n", __func__, i);

	/* port setup */
	max3140s[i]->port.irq = max3140s[i]->irq;
	max3140s[i]->port.uartclk = max3140s[i]->crystal ? 3686400 : 1843200;
	max3140s[i]->port.fifosize = 2; /* TX "FIFO" is only 2 bytes */
	max3140s[i]->port.ops = &max3100_ops;
	max3140s[i]->port.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF;
	max3140s[i]->port.line = i;
	max3140s[i]->port.type = PORT_MAX3100;
	max3140s[i]->port.dev = &spi->dev;

	retval = uart_add_one_port(&max3140hd_uart_driver, &max3140s[i]->port);
	if (retval < 0)
		dev_warn(&spi->dev,
			 "uart_add_one_port failed for line %d with error %d\n",
			 i, retval);

	/* set shutdown mode to save power. Will be woken-up on open */
	tx = MAX3100_WC | MAX3100_SHDN;
	max3140_cmd(max3140s[i], tx);
	mutex_unlock(&max3140s_lock);
	return 0;
}

static int __devexit max3140_remove(struct spi_device *spi)
{
	struct max3140hd_port *s = dev_get_drvdata(&spi->dev);
	int i;

	mutex_lock(&max3140s_lock);

	/* find out the index for the chip we are removing */
	for (i = 0; i < MAX_MAX3140; i++)
		if (max3140s[i] == s)
			break;

	dev_dbg(&spi->dev, "%s: removing port %d\n", __func__, i);
	uart_remove_one_port(&max3140hd_uart_driver, &max3140s[i]->port);
	kfree(max3140s[i]->name);
	kfree(max3140s[i]);
	max3140s[i] = NULL;

	/* check if this is the last chip we have */
	for (i = 0; i < MAX_MAX3140; i++)
		if (max3140s[i]) {
			mutex_unlock(&max3140s_lock);
			return 0;
		}
	pr_debug("removing max3140hd driver\n");
	uart_unregister_driver(&max3140hd_uart_driver);

	mutex_unlock(&max3140s_lock);
	return 0;
}

#ifdef CONFIG_PM

static int max3140_suspend(struct spi_device *spi, pm_message_t state)
{
	struct max3140hd_port *s = dev_get_drvdata(&spi->dev);
	u16 tx;

	dev_dbg(&s->spi->dev, "%s\n", __func__);

	disable_irq(s->irq);

	s->suspending = 1;
	uart_suspend_port(&max3140hd_uart_driver, &s->port);

	/* no HW suspend, so do SW one */
	tx = (s->conf | MAX3100_WC | MAX3100_SHDN);
	max3140_cmd(s, tx);

	return 0;
}

static int max3140_resume(struct spi_device *spi)
{
	struct max3140hd_port *s = dev_get_drvdata(&spi->dev);

	dev_err(&s->spi->dev, "%s\n", __func__);

	uart_resume_port(&max3140hd_uart_driver, &s->port);
	s->suspending = 0;

	enable_irq(s->irq);

	set_bit(BIT_CONF_COMMIT, &s->irqflags);
	wake_up_process(s->main_thread);

	return 0;
}

#else
#define max3140_suspend NULL
#define max3140_resume  NULL
#endif

static struct spi_driver max3140_driver = {
	.driver = {
		.name		= "max3140hd",
		.bus		= &spi_bus_type,
		.owner		= THIS_MODULE,
	},

	.probe		= max3140_probe,
	.remove		= __devexit_p(max3140_remove),
	.suspend	= max3140_suspend,
	.resume		= max3140_resume,
};

static int __init max3140_init(void)
{
	return spi_register_driver(&max3140_driver);
}
module_init(max3140_init);

static void __exit max3140_exit(void)
{
	spi_unregister_driver(&max3140_driver);
}
module_exit(max3140_exit);

MODULE_DESCRIPTION("MAX3140 driver");
MODULE_AUTHOR("Bjoern Krombholz <b.krombholz@pironex.de>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:max3140hd");
