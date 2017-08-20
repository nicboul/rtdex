/*
 * RTDEx FIFO DMA
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/circ_buf.h>
#include <asm/uaccess.h>
#include <asm/sizes.h>
#include <asm/dma.h>
#include <asm/io.h>
#include <asm/atomic.h>
#include <mach/pl330.h>
#include <linux/of.h>
#include <stdbool.h>

#include "rtdex_axi_driver.h"

/* Define debugging for use during our driver bringup */
#undef PDEBUG
#define PDEBUG(fmt, args...)
//#define PDEBUG(fmt, args...) printk(KERN_INFO fmt, ## args)

/* Offsets for control registers in the AXI MM2S FIFO */
#define AXI_TXFIFO				0x0
#define AXI_TX_FIFO_CTRL			0x4
#define AXI_TX_FIFO_TIMER			0x8

#define MODULE_NAME				"rtdex"
#define RTDEX_MINOR				0

#define RTDEX_BUF_SIZE				(1048576*8)	/* 2^20, 1MB * 8 */
#define RTDEX_FIFO_SIZE				65532		/* 16383 (sample) * 4bytes (burst size) */
#define RTDEX_FIFO_SIZE_HALF			32768
#define RTDEX_BURST_SIZE			4		/* FPGA core specific bus width */

#define RTDEX_CTRL_READ_OVERFLOW		0x00000001
#define RTDEX_CTRL_READ_UNDERFLOW		0x00000002
#define RTDEX_CTRL_READ_OVERFLOW_CLEAR		0x00000004
#define RTDEX_CTRL_READ_UNDERFLOW_CLEAR		0x00000008
#define RTDEX_CTRL_READ_INTERRUPT_EN		0x00000010
#define RTDEX_CTRL_READ_FIFO_RESET		0x00000040
#define RTDEX_CTRL_READ_FIFO_EMPTY		0x00000080
#define RTDEX_CTRL_READ_FIFO_FULL		0x00000100
#define RTDEX_CTRL_READ_CAST_MODE		0x00000600
#define RTDEX_CTRL_READ_CAST_MODE_OFFSET	9

#define RTDEX_CTRL_WRITE_OVERFLOW		0x00010000
#define RTDEX_CTRL_WRITE_UNDERFLOW		0x00020000
#define RTDEX_CTRL_WRITE_OVERFLOW_CLEAR		0x00040000
#define RTDEX_CTRL_WRITE_UNDERFLOW_CLEAR	0x00080000
#define RTDEX_CTRL_WRITE_INTERRUPT_EN		0x00100000
#define RTDEX_CTRL_WRITE_FIFO_RESET		0x00400000
#define RTDEX_CTRL_WRITE_FIFO_EMPTY		0x00800000
#define RTDEX_CTRL_WRITE_FIFO_FULL		0x01000000
#define RTDEX_CTRL_WRITE_CAST_MODE		0x06000000
#define RTDEX_CTRL_WRITE_CAST_MODE_OFFSET	25

typedef struct {
	unsigned fifo;
	unsigned ctrl;
	volatile unsigned read_byte;
	volatile unsigned write_byte;
} rtdex_regs;

int rtdex_major = 60;
module_param(rtdex_major, int, 0);

struct rtdex_dev {

	/* driver reference counts */
	u32 writers;
	u32 readers;

	dev_t devno;
	struct cdev cdev;
	struct platform_device *pdev;

	struct pl330_client_data *tx_client_data;
	struct pl330_client_data *rx_client_data;

	struct mutex gen_mutex;
	struct mutex tx_mutex;
	struct mutex rx_mutex;

	struct task_struct *tx_dma_task;
	struct task_struct *rx_dma_task;

	uint8_t rx_dma_task_stop;
	uint8_t tx_dma_task_stop;

	/* metrics */
	u32 rx_irq;
	u32 tx_irq;

	u32 rx_dma;
	u32 tx_dma;

	u32 tx_current_dma_cnt;
	u32 rx_current_dma_cnt;

	uint8_t rx_dma_done2;
	uint8_t tx_dma_done2;

	uint8_t rx_dma_done;
	uint8_t tx_dma_done;

	wait_queue_head_t rx_wq;	/* wait queue */
	wait_queue_head_t tx_wq;	/* wait queue */

	uint8_t rx_data_avail;
	uint8_t tx_data_avail;

	wait_queue_head_t rx_data_avail_wq;	/* wait queue */
	wait_queue_head_t tx_data_avail_wq;	/* wait queue */

	u32 tx_overflow;
	u32 tx_underflow;

	u32 rx_overflow;
	u32 rx_underflow;

	/* lock when DMA is running */
	struct mutex tx_dma_transfer_lock;
	struct mutex rx_dma_transfer_lock;

	dma_addr_t buffer_d_addr_tx;
	dma_addr_t buffer_d_addr_rx;

	u32 tx_dma_channel;
	u32 rx_dma_channel;

	/* circular buffer */
	struct circ_buf tx_buffer;
	struct circ_buf rx_buffer;

	/* hardware device constants */
	u32 dev_physaddr;
	void *dev_virtaddr;
	u32 dev_addrsize;

	int irq_write_byte;
	int irq_read_byte;

	u32 block_size;
	u32 burst_length; /* 1 to 16 */
};

struct rtdex_dev *rtdex_dev;

static void rtdex_done_tx_callback(unsigned int channel, void *data);
static void rtdex_done_rx_callback(unsigned int channel, void *data);
static void rtdex_rx_fault_callback(unsigned int channel, unsigned int fault_type, unsigned int fault_address, void *data);
static void rtdex_tx_fault_callback(unsigned int channel, unsigned int fault_type, unsigned int fault_address, void *data);
static int rtdex_dma_tx_transfer(void);
static int rtdex_dma_rx_transfer(void);
static int rx_dma_thread(void *data);
static int tx_dma_thread(void *data);

static int rtdex_init_rx(void)
{
	int retval;
	volatile rtdex_regs *regs = rtdex_dev->dev_virtaddr;

	PDEBUG("rtdex_init_rx\n");

	rtdex_dev->rx_buffer.head = 0;
	rtdex_dev->rx_buffer.tail = 0;

	rtdex_dev->rx_irq = 0;
	rtdex_dev->rx_dma = 0;

	rtdex_dev->rx_overflow = 0;
	rtdex_dev->rx_underflow = 0;

	rtdex_dev->rx_buffer.buf = dma_alloc_coherent(&rtdex_dev->pdev->dev, RTDEX_BUF_SIZE, &rtdex_dev->buffer_d_addr_rx, GFP_KERNEL);
	if (!rtdex_dev->rx_buffer.buf) {
		dev_err(&rtdex_dev->pdev->dev,
		"Rx coherent DMA buffer allocation failed\n");
		retval = -ENOMEM;
		goto fail1;
	}

	if (request_dma(rtdex_dev->rx_dma_channel, MODULE_NAME)) {
		dev_err(&rtdex_dev->pdev->dev,
		"unable to alloc DMA channel %d\n",
		rtdex_dev->rx_dma_channel);
		retval = -EBUSY;
		goto fail2;
	}

	set_pl330_client_data(rtdex_dev->rx_dma_channel, rtdex_dev->rx_client_data);
	set_pl330_done_callback(rtdex_dev->rx_dma_channel, rtdex_done_rx_callback, rtdex_dev);
	set_pl330_fault_callback(rtdex_dev->rx_dma_channel, rtdex_rx_fault_callback, rtdex_dev);
	set_pl330_incr_dev_addr(rtdex_dev->rx_dma_channel, 0);

	PDEBUG("rx dma buffer alloc - d @0x%0x v @0x%0x\n", (u32)rtdex_dev->buffer_d_addr_rx, (u32)rtdex_dev->rx_buffer.buf);

	rtdex_dev->rx_data_avail = 0;
	rtdex_dev->rx_dma_done = 0;
	rtdex_dev->rx_dma_task_stop = 0;
	rtdex_dev->rx_dma_task = kthread_run(&rx_dma_thread, NULL, "rtdex_rx_dma");
	set_user_nice(rtdex_dev->rx_dma_task, -20);

	/* reset FIFO */
	regs->ctrl |= RTDEX_CTRL_READ_FIFO_RESET;
	regs->ctrl &= ~RTDEX_CTRL_READ_FIFO_RESET;

	return 0;

fail2:
	dma_free_coherent(&rtdex_dev->pdev->dev, RTDEX_BUF_SIZE, rtdex_dev->rx_buffer.buf, rtdex_dev->buffer_d_addr_rx);
fail1:
	return retval;
}

static int rtdex_init_tx(void)
{
	int ret;
	volatile rtdex_regs *regs = rtdex_dev->dev_virtaddr;

	/* Empty circular buffer */
	rtdex_dev->tx_buffer.head = 0;
	rtdex_dev->tx_buffer.tail = 0;

	rtdex_dev->tx_irq = 0;
	rtdex_dev->tx_dma = 0;

	rtdex_dev->tx_overflow = 0;
	rtdex_dev->tx_underflow = 0;

	rtdex_dev->tx_buffer.buf = dma_alloc_coherent(&rtdex_dev->pdev->dev, RTDEX_BUF_SIZE, &rtdex_dev->buffer_d_addr_tx, GFP_KERNEL);

	if (!rtdex_dev->tx_buffer.buf) {
		dev_err(&rtdex_dev->pdev->dev,
		"Tx coherent DMA buffer allocation failed\n");
		ret = -ENOMEM;
		goto fail1;
	}

	if (request_dma(rtdex_dev->tx_dma_channel, MODULE_NAME)) {
		dev_err(&rtdex_dev->pdev->dev,
		"unable to alloc DMA channel %d\n",
		rtdex_dev->tx_dma_channel);
		ret = -EBUSY;
		goto fail2;
	}

	set_pl330_client_data(rtdex_dev->tx_dma_channel, rtdex_dev->tx_client_data);
	set_pl330_done_callback(rtdex_dev->tx_dma_channel, rtdex_done_tx_callback, rtdex_dev);
	set_pl330_fault_callback(rtdex_dev->tx_dma_channel, rtdex_tx_fault_callback, rtdex_dev);
	set_pl330_incr_dev_addr(rtdex_dev->tx_dma_channel, 0);

	PDEBUG("tx dma buffer alloc - d @0x%0x v @0x%0x\n", (u32)rtdex_dev->buffer_d_addr_tx, (u32)rtdex_dev->tx_buffer.buf);

	rtdex_dev->tx_data_avail = 0;
	rtdex_dev->tx_dma_done = 0;
	rtdex_dev->tx_dma_task_stop = 0;
	rtdex_dev->tx_dma_task = kthread_run(&tx_dma_thread, NULL, "rtdex_tx_dma");
	set_user_nice(rtdex_dev->tx_dma_task, -20);

	/* reset FIFO */
	regs->ctrl |= RTDEX_CTRL_WRITE_FIFO_RESET;
	regs->ctrl &= ~RTDEX_CTRL_WRITE_FIFO_RESET;

	return 0;

fail2:
	dma_free_coherent(&rtdex_dev->pdev->dev, RTDEX_BUF_SIZE, rtdex_dev->tx_buffer.buf, rtdex_dev->buffer_d_addr_tx);
fail1:
	return ret;
}

static void rtdex_rx_fault_callback(unsigned int channel,
                                 unsigned int fault_type,
                                 unsigned int fault_address,
                                 void *data)
{
	PDEBUG("rtdex_rx_fault_callback\n");

	dev_err(&rtdex_dev->pdev->dev,
		"DMA fault type %x at address 0x%0x on channel %d\n",
		fault_type, fault_address, channel);

	mutex_unlock(&rtdex_dev->rx_dma_transfer_lock);

	rtdex_dev->rx_dma_done = 1;
	rtdex_dev->rx_dma_done2 = 1;
	wake_up(&rtdex_dev->rx_wq);
}

static void rtdex_tx_fault_callback(unsigned int channel,
                                 unsigned int fault_type,
                                 unsigned int fault_address,
                                 void *data)
{
	PDEBUG("rtdex_tx_fault_callback\n");

	dev_err(&rtdex_dev->pdev->dev,
		"DMA fault type %x at address 0x%0x on channel %d\n",
		fault_type, fault_address, channel);

	mutex_unlock(&rtdex_dev->tx_dma_transfer_lock);

	rtdex_dev->tx_dma_done = 1;
	rtdex_dev->tx_dma_done2 = 1;
	wake_up(&rtdex_dev->tx_wq);
}

static void rtdex_done_rx_callback(unsigned int channel, void *data)
{
	struct circ_buf *rx_buf = &rtdex_dev->rx_buffer;
	volatile rtdex_regs *rtdex_regs = rtdex_dev->dev_virtaddr;

	PDEBUG("rtdex_done_rx_callback\n");

	smp_mb();

	rx_buf->head = (rx_buf->head + rtdex_dev->rx_current_dma_cnt) & (RTDEX_BUF_SIZE - 1);

	rtdex_dev->rx_dma++;
	mutex_unlock(&rtdex_dev->rx_dma_transfer_lock);

	if (rtdex_regs->ctrl & RTDEX_CTRL_READ_OVERFLOW)
		rtdex_dev->rx_overflow++;

	if (rtdex_regs->ctrl & RTDEX_CTRL_READ_UNDERFLOW)
		rtdex_dev->rx_underflow++;

	rtdex_regs->ctrl |= RTDEX_CTRL_READ_OVERFLOW_CLEAR | RTDEX_CTRL_READ_UNDERFLOW_CLEAR;
	rtdex_regs->ctrl &= ~(RTDEX_CTRL_READ_OVERFLOW_CLEAR | RTDEX_CTRL_READ_UNDERFLOW_CLEAR);

	rtdex_regs->ctrl |= RTDEX_CTRL_READ_INTERRUPT_EN;

	rtdex_dev->rx_dma_done = 1;
	rtdex_dev->rx_dma_done2 = 1;
	wake_up(&rtdex_dev->rx_wq);
}

static void rtdex_done_tx_callback(unsigned int channel, void *data)
{
	struct circ_buf *tx_buf = &rtdex_dev->tx_buffer;
	volatile rtdex_regs *rtdex_regs = rtdex_dev->dev_virtaddr;
	unsigned long tail = tx_buf->tail;

	PDEBUG("rtdex_done_tx_callback\n");

	tx_buf->tail = (tail + rtdex_dev->tx_current_dma_cnt) & (RTDEX_BUF_SIZE - 1);

	rtdex_dev->tx_dma++;
	mutex_unlock(&rtdex_dev->tx_dma_transfer_lock);

	if (rtdex_regs->ctrl & RTDEX_CTRL_WRITE_OVERFLOW)
		rtdex_dev->tx_overflow++;

	if (rtdex_regs->ctrl & RTDEX_CTRL_WRITE_UNDERFLOW)
		rtdex_dev->tx_underflow++;

	rtdex_regs->ctrl |= RTDEX_CTRL_WRITE_OVERFLOW_CLEAR | RTDEX_CTRL_WRITE_UNDERFLOW_CLEAR;
	rtdex_regs->ctrl &= ~(RTDEX_CTRL_WRITE_OVERFLOW_CLEAR | RTDEX_CTRL_WRITE_UNDERFLOW_CLEAR);

	rtdex_regs->ctrl |= RTDEX_CTRL_WRITE_INTERRUPT_EN;

	rtdex_dev->tx_dma_done = 1;
	rtdex_dev->tx_dma_done2 = 1;
	wake_up(&rtdex_dev->tx_wq);
}

static int rtdex_dma_rx_transfer(void)
{
	size_t fifo_byte_avail;
	size_t transfer_size;
	size_t circ_space;
	struct circ_buf *rx_buf = &rtdex_dev->rx_buffer;
	unsigned long tail, head;
	int flags;

	volatile rtdex_regs *rtdex_regs;
	unsigned ctrl_reg;

	if (mutex_trylock(&rtdex_dev->rx_dma_transfer_lock)) {

		rtdex_regs = rtdex_dev->dev_virtaddr;
		ctrl_reg = rtdex_regs->ctrl;
		fifo_byte_avail = rtdex_regs->read_byte;

		head = rx_buf->head;
		tail = ACCESS_ONCE(rx_buf->tail);

		circ_space = CIRC_SPACE(head, tail, RTDEX_BUF_SIZE);
		if (circ_space >= RTDEX_BURST_SIZE * rtdex_dev->burst_length) {

			transfer_size = circ_space;
			if (transfer_size > fifo_byte_avail)
				transfer_size = fifo_byte_avail;

			transfer_size -= transfer_size % (RTDEX_BURST_SIZE * rtdex_dev->burst_length);

			/* enable interrupt, do DMA transfer asap */
			if (transfer_size == 0) {/* waiting for data in the FIFO */
				mutex_unlock(&rtdex_dev->rx_dma_transfer_lock);
				rtdex_regs->ctrl |= RTDEX_CTRL_READ_INTERRUPT_EN;
				return 0;
			}
			else {
				rtdex_regs = rtdex_dev->dev_virtaddr;
				fifo_byte_avail = rtdex_regs->read_byte;

				/* transfer the max contiguous data */
				if (head + transfer_size > RTDEX_BUF_SIZE)
					transfer_size = RTDEX_BUF_SIZE - head;

				rtdex_dev->rx_current_dma_cnt = transfer_size;

				flags = claim_dma_lock();
				disable_dma(rtdex_dev->rx_dma_channel);
				clear_dma_ff(rtdex_dev->rx_dma_channel);
				set_dma_mode(rtdex_dev->rx_dma_channel, DMA_MODE_READ);
				set_dma_count(rtdex_dev->rx_dma_channel, transfer_size);
				set_dma_addr(rtdex_dev->rx_dma_channel, rtdex_dev->buffer_d_addr_rx + head);
				enable_dma(rtdex_dev->rx_dma_channel);
				release_dma_lock(flags);
			}
		} else {
			mutex_unlock(&rtdex_dev->rx_dma_transfer_lock);
			return 0;
		}
	}

	return 1;
}

static int rtdex_dma_tx_transfer(void)
{
	size_t fifo_byte_used;
	size_t transfer_size;
	size_t circ_cnt;
	struct circ_buf *tx_buf = &rtdex_dev->tx_buffer;
	unsigned long tail, head;
	int flags;

	volatile rtdex_regs *rtdex_regs;
	unsigned ctrl_reg;

	if (mutex_trylock(&rtdex_dev->tx_dma_transfer_lock)) {

		rtdex_regs = rtdex_dev->dev_virtaddr;
		ctrl_reg = rtdex_regs->ctrl;
		fifo_byte_used = rtdex_regs->write_byte;

		head = ACCESS_ONCE(tx_buf->head);
		tail = tx_buf->tail;

		/* if data is present in the circ buffer, kick off the DMA transfert */
		circ_cnt = CIRC_CNT(head, tail, RTDEX_BUF_SIZE);
		if (circ_cnt >= RTDEX_BURST_SIZE * rtdex_dev->burst_length) {

			/* read index before reading contents at that index */
			smp_read_barrier_depends();

			transfer_size = circ_cnt;
			if (circ_cnt > RTDEX_FIFO_SIZE - fifo_byte_used)
				transfer_size = RTDEX_FIFO_SIZE - fifo_byte_used;

			transfer_size -= transfer_size % (RTDEX_BURST_SIZE * rtdex_dev->burst_length);

			/* enable interrupt, do DMA transfer asap */
			if (transfer_size == 0) {/* waiting for space in the FIFO */

				mutex_unlock(&rtdex_dev->tx_dma_transfer_lock);
				rtdex_regs->ctrl |= RTDEX_CTRL_WRITE_INTERRUPT_EN;
				return 0;
			}
			else {

				/* transfer the max contiguous data */
				if (transfer_size + tail > RTDEX_BUF_SIZE)
					transfer_size = RTDEX_BUF_SIZE - tail;

				rtdex_dev->tx_current_dma_cnt = transfer_size;

				flags = claim_dma_lock();
				disable_dma(rtdex_dev->tx_dma_channel);
				clear_dma_ff(rtdex_dev->tx_dma_channel);
				set_dma_mode(rtdex_dev->tx_dma_channel, DMA_MODE_WRITE);
				set_dma_count(rtdex_dev->tx_dma_channel, transfer_size);
				set_dma_addr(rtdex_dev->tx_dma_channel, rtdex_dev->buffer_d_addr_tx + tail);
				enable_dma(rtdex_dev->tx_dma_channel);
				release_dma_lock(flags);
			}
		} else {
			mutex_unlock(&rtdex_dev->tx_dma_transfer_lock);
			return 0;
		}
	}

	return 1;
}

static int rx_dma_thread(void *data)
{
	volatile rtdex_regs *regs;
	regs = rtdex_dev->dev_virtaddr;

	while (1) {

		if (wait_event_interruptible(rtdex_dev->rx_data_avail_wq, rtdex_dev->rx_data_avail != 0) != 0)
			return -ERESTARTSYS;

		rtdex_dev->rx_data_avail = 0;

		if (rtdex_dev->rx_dma_task_stop)
			break;

		if (rtdex_dma_rx_transfer() == 1)
			if (wait_event_interruptible(rtdex_dev->rx_wq, rtdex_dev->rx_dma_done != 0) != 0)
				return -ERESTARTSYS;

		rtdex_dev->rx_dma_done = 0;

		if (rtdex_dev->rx_dma_task_stop)
			break;
	}

	return 0;
}

static int tx_dma_thread(void *data)
{
	volatile rtdex_regs *regs;
	regs = rtdex_dev->dev_virtaddr;

	while (1) {

		if (wait_event_interruptible(rtdex_dev->tx_data_avail_wq, rtdex_dev->tx_data_avail != 0) != 0)
			return -ERESTARTSYS;

		rtdex_dev->tx_data_avail = 0;

		if (rtdex_dev->tx_dma_task_stop)
			break;

		if (rtdex_dma_tx_transfer() == 1)
			if (wait_event_interruptible(rtdex_dev->tx_wq, rtdex_dev->tx_dma_done != 0) != 0)
				return -ERESTARTSYS;

		rtdex_dev->tx_dma_done = 0;

		if (rtdex_dev->tx_dma_task_stop)
			break;
	}
	return 0;
}

irqreturn_t rx_space_available(int irq, void *dev_id)
{
	volatile rtdex_regs *regs;

	PDEBUG("rx_irq\n");

	regs = rtdex_dev->dev_virtaddr;

	/* disable interrupt */
	regs->ctrl &= ~RTDEX_CTRL_READ_INTERRUPT_EN;
	rtdex_dev->rx_irq++;

	/* tell rx_dma_thread some data is available */
	rtdex_dev->rx_data_avail = 1;
	wake_up(&rtdex_dev->rx_data_avail_wq);

	return IRQ_HANDLED;
}

irqreturn_t tx_space_available(int irq, void *dev_id)
{
	volatile rtdex_regs *regs;

	PDEBUG("tx_irq\n");

	regs = rtdex_dev->dev_virtaddr;

	/* disable interrupt */
	regs->ctrl &= ~RTDEX_CTRL_WRITE_INTERRUPT_EN;
	rtdex_dev->tx_irq++;

	/* tell tx_dma_thread some data is available */
	rtdex_dev->tx_data_avail = 1;
	wake_up(&rtdex_dev->tx_data_avail_wq);

	return IRQ_HANDLED;
}

unsigned int rtdex_poll(struct file *filp, poll_table *wait)
{
	unsigned int mask = 0;

	unsigned long head = 0;
	struct circ_buf *rx_buf = &rtdex_dev->rx_buffer;
	size_t circ_cnt;

	unsigned long tail = 0;
	struct circ_buf *tx_buf = &rtdex_dev->tx_buffer;
	size_t circ_space;

	head = ACCESS_ONCE(rx_buf->head);
	circ_cnt = CIRC_CNT(head, rx_buf->tail, RTDEX_BUF_SIZE);
	smp_read_barrier_depends();

	tail = ACCESS_ONCE(tx_buf->tail);
	circ_space = CIRC_SPACE(tx_buf->head, tail, RTDEX_BUF_SIZE);

	poll_wait(filp, &rtdex_dev->rx_wq, wait);
	poll_wait(filp, &rtdex_dev->tx_wq, wait);

	if (circ_cnt > 0)
		mask |= POLLIN | POLLRDNORM;	/* readable */

	if (circ_space > 0)
		mask |= POLLOUT | POLLWRNORM;	/* writable */

	return mask;
}

ssize_t rtdex_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	int ret;
	struct circ_buf *rx_buf = &rtdex_dev->rx_buffer;
	volatile rtdex_regs *regs;
	unsigned long head;

	size_t circ_cnt;
	size_t transfer_size;
	size_t chunk_size1;
	size_t chunk_size2;

	if (mutex_lock_interruptible(&rtdex_dev->rx_mutex)) {
		PDEBUG("rtdex read locked\n");
		return -EINTR;
	}

	regs = rtdex_dev->dev_virtaddr;
	head = ACCESS_ONCE(rx_buf->head);
	circ_cnt = CIRC_CNT(head, rx_buf->tail, RTDEX_BUF_SIZE);
	smp_read_barrier_depends();

	if ((filp->f_flags & O_NONBLOCK) && (circ_cnt == 0)) {
		mutex_unlock(&rtdex_dev->rx_mutex);
		return -EAGAIN;
	}

	transfer_size = count;

	if (filp->f_flags & O_NONBLOCK) {
		if (transfer_size > circ_cnt)
			transfer_size = circ_cnt;
	} else {

		if (transfer_size > RTDEX_FIFO_SIZE_HALF)
			transfer_size = RTDEX_FIFO_SIZE_HALF;

		while (transfer_size > circ_cnt) {

			rtdex_dev->rx_dma_done2 = 0;
			regs->read_byte = transfer_size - circ_cnt;

			if (!mutex_is_locked(&rtdex_dev->rx_dma_transfer_lock)) {
				regs->ctrl |= RTDEX_CTRL_READ_INTERRUPT_EN;
				PDEBUG("rtdex_read1:%d\n", rtdex_dev->rx_dma_done);
			}

			if (wait_event_interruptible(rtdex_dev->rx_wq, rtdex_dev->rx_dma_done2 != 0) != 0) {
				mutex_unlock(&rtdex_dev->rx_mutex);
				return -ERESTARTSYS;
			}

			head = ACCESS_ONCE(rx_buf->head);
			circ_cnt = CIRC_CNT(head, rx_buf->tail, RTDEX_BUF_SIZE);
		}
		regs->read_byte = rtdex_dev->block_size;
	}

	if (transfer_size + rx_buf->tail > RTDEX_BUF_SIZE) {

		chunk_size1 = RTDEX_BUF_SIZE - rx_buf->tail;
		ret = copy_to_user(buf, &rx_buf->buf[rx_buf->tail], chunk_size1);
		if (ret != 0) {
			transfer_size = -EFAULT;
			goto out;
		}
		smp_mb();
		rx_buf->tail = (rx_buf->tail + chunk_size1) & (RTDEX_BUF_SIZE - 1);

		chunk_size2 = transfer_size - chunk_size1;
		ret = copy_to_user(buf + chunk_size1, &rx_buf->buf[rx_buf->tail], chunk_size2);
		if (ret != 0) {
			transfer_size = chunk_size1;
			goto out;
		}
		smp_mb();
		rx_buf->tail = (rx_buf->tail + chunk_size2) & (RTDEX_BUF_SIZE - 1);

	} else {

		ret = copy_to_user(buf, &rx_buf->buf[rx_buf->tail], transfer_size);
		if (ret != 0) {
			transfer_size = -EFAULT;
			goto out;
		}
		smp_mb(); /* finish reading descriptor before incrementing tail */
		rx_buf->tail = (rx_buf->tail + transfer_size) & (RTDEX_BUF_SIZE - 1);
	}

out:
	/* enable interrupt if the circ buf can receive a data block */
	if (RTDEX_BUF_SIZE - (circ_cnt - transfer_size) > rtdex_dev->block_size)
		if (!mutex_is_locked(&rtdex_dev->rx_dma_transfer_lock))
			regs->ctrl |= RTDEX_CTRL_READ_INTERRUPT_EN;

	mutex_unlock(&rtdex_dev->rx_mutex);
	return transfer_size;
}

ssize_t rtdex_write(struct file *filp, const char __user *buf, size_t count,
	loff_t *f_pos)
{
	struct circ_buf *tx_buf = &rtdex_dev->tx_buffer;
	volatile rtdex_regs *regs = rtdex_dev->dev_virtaddr;
	unsigned long tail;

	size_t circ_space;
	size_t transfer_size;
	size_t chunk_size1;
	size_t chunk_size2;
	int ret;

	if (mutex_lock_interruptible(&rtdex_dev->tx_mutex)) {
		return -EINTR;
	}

	transfer_size = count;

	tail = ACCESS_ONCE(tx_buf->tail);
	circ_space = CIRC_SPACE(tx_buf->head, tail, RTDEX_BUF_SIZE);

	if ((filp->f_flags & O_NONBLOCK) && (circ_space == 0)) {
		mutex_unlock(&rtdex_dev->tx_mutex);
		return -EAGAIN;
	}

	if (filp->f_flags & O_NONBLOCK) {
		if (transfer_size > circ_space)
			transfer_size = circ_space;
	} else {

		if (transfer_size > RTDEX_FIFO_SIZE_HALF)
			transfer_size = RTDEX_FIFO_SIZE_HALF;

		/* block until enough space in circ buf */
		while (transfer_size > circ_space) {

			rtdex_dev->tx_dma_done2 = 0;

			if (!mutex_is_locked(&rtdex_dev->tx_dma_transfer_lock))
				regs->ctrl |= RTDEX_CTRL_WRITE_INTERRUPT_EN;

			if (wait_event_interruptible(rtdex_dev->tx_wq, rtdex_dev->tx_dma_done2 != 0) != 0) {
				mutex_unlock(&rtdex_dev->tx_mutex);
				return -ERESTARTSYS;
			}
			tail = ACCESS_ONCE(tx_buf->tail);
			circ_space = CIRC_SPACE(tx_buf->head, tail, RTDEX_BUF_SIZE);
		}
	}

	/* copy data from user */
	if (transfer_size + tx_buf->head > RTDEX_BUF_SIZE) {

		/* the available space is fragmented in two pieces
			[     |xxxxxxxxx|     ]
                              ^         ^
			      Tail      Head
		 */
		chunk_size1 = RTDEX_BUF_SIZE - tx_buf->head;
		ret = copy_from_user(&tx_buf->buf[tx_buf->head], buf, chunk_size1);
		if (ret != 0) {
			transfer_size = -EFAULT;
			goto out;
		}
		smp_wmb();
		tx_buf->head = (tx_buf->head + chunk_size1) & (RTDEX_BUF_SIZE - 1);

		chunk_size2 = transfer_size - chunk_size1;
		ret = copy_from_user(&tx_buf->buf[tx_buf->head], buf + chunk_size1, chunk_size2);
		if (ret != 0) {
			transfer_size = chunk_size1;
			goto out;
		}
		smp_wmb();
		tx_buf->head = (tx_buf->head + chunk_size2) & (RTDEX_BUF_SIZE - 1);

	} else {
		ret = copy_from_user(&tx_buf->buf[tx_buf->head], buf, transfer_size);
		if (ret != 0) {
			transfer_size = -EFAULT;
			goto out;
		}
		smp_wmb(); /* commit the item before incrementing the head */
		tx_buf->head = (tx_buf->head + transfer_size) & (RTDEX_BUF_SIZE - 1);
	}

out:

	if (!mutex_is_locked(&rtdex_dev->tx_dma_transfer_lock))
		regs->ctrl |= RTDEX_CTRL_WRITE_INTERRUPT_EN;

	mutex_unlock(&rtdex_dev->tx_mutex);
	return transfer_size;
}

static void rtdex_close_rx(void)
{
	rtdex_dev->rx_dma_task_stop = 1;
	rtdex_dev->rx_data_avail = 1;
	rtdex_dev->rx_dma_done = 1;

	wake_up(&rtdex_dev->rx_data_avail_wq);
	wake_up(&rtdex_dev->rx_wq);

	kthread_stop(rtdex_dev->rx_dma_task);

	free_dma(rtdex_dev->rx_dma_channel);
	dma_free_coherent(&rtdex_dev->pdev->dev, RTDEX_BUF_SIZE, rtdex_dev->rx_buffer.buf,
		rtdex_dev->buffer_d_addr_rx);
}

static void rtdex_close_tx(void)
{
	rtdex_dev->tx_dma_task_stop = 1;
	rtdex_dev->tx_data_avail = 1;
	rtdex_dev->tx_dma_done = 1;

	wake_up(&rtdex_dev->tx_data_avail_wq);
	wake_up(&rtdex_dev->tx_wq);

	kthread_stop(rtdex_dev->tx_dma_task);

	free_dma(rtdex_dev->tx_dma_channel);
	dma_free_coherent(&rtdex_dev->pdev->dev, RTDEX_BUF_SIZE, rtdex_dev->tx_buffer.buf,
		rtdex_dev->buffer_d_addr_tx);
}

int rtdex_release(struct inode *inode, struct file *filp)
{
	int ret;
	volatile rtdex_regs *regs = rtdex_dev->dev_virtaddr;
	unsigned interrupt_mask;

	if (mutex_lock_interruptible(&rtdex_dev->gen_mutex)) {
		return -EINTR;
	}

	/* Manage writes via reference counts */
	switch (filp->f_flags & O_ACCMODE) {
	case O_RDONLY:
		rtdex_dev->readers--;
		break;
	case O_WRONLY:
		rtdex_dev->writers--;
		break;
	case O_RDWR:
		rtdex_dev->writers--;
		rtdex_dev->readers--;
	default:
		ret = -EINVAL;
	}

	interrupt_mask = 0;
	if (!rtdex_dev->readers) {
		interrupt_mask |= RTDEX_CTRL_READ_INTERRUPT_EN;
	}

	regs->ctrl &= ~interrupt_mask;

	mutex_unlock(&rtdex_dev->gen_mutex);

	return ret;
}

/* File operations */
int rtdex_open(struct inode *inode, struct file *filp)
{
	int ret = 0;
	volatile rtdex_regs *regs;
	unsigned interrupt_mask;

	PDEBUG("rtdex_open\n");

	regs = rtdex_dev->dev_virtaddr;
	filp->private_data = rtdex_dev;

	if (mutex_lock_interruptible(&rtdex_dev->gen_mutex)) {
		return -ERESTARTSYS;
	}

	/* We're only going to allow one read and/or write at a time, so manage that via
	 * reference counts
	 */

	switch (filp->f_flags & O_ACCMODE) {
	case O_RDONLY:
		PDEBUG("rtdex_open: O_RDONLY\n");
		interrupt_mask = RTDEX_CTRL_READ_INTERRUPT_EN;

		if (rtdex_dev->readers)
			ret = -EBUSY;
		else
			rtdex_dev->readers++;
		break;

	case O_WRONLY:
		PDEBUG("rtdex_open: O_WRONLY\n");

		if (rtdex_dev->writers)
			ret = -EBUSY;
		else
			rtdex_dev->writers++;
		break;

	case O_RDWR:
		PDEBUG("rtdex_open: O_RDWR\n");
		interrupt_mask = RTDEX_CTRL_READ_INTERRUPT_EN;

		if (rtdex_dev->writers || rtdex_dev->readers) {
			ret = -EBUSY;
		}
		else {
			rtdex_dev->writers++;
			rtdex_dev->readers++;
		}
		break;

	default:
		ret = -EINVAL;
		goto out;
	}

	/* enable interrupts */
	regs->ctrl |= interrupt_mask;

out:
	mutex_unlock(&rtdex_dev->gen_mutex);
	return ret;
}

static long rtdex_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	volatile rtdex_regs *regs = rtdex_dev->dev_virtaddr;

	if (mutex_lock_interruptible(&rtdex_dev->gen_mutex)) {
		ret = -EINTR;
		goto out;
	}

	if (mutex_lock_interruptible(&rtdex_dev->rx_mutex)) {
		ret = -EINTR;
		goto out;
	}

	if (mutex_lock_interruptible(&rtdex_dev->tx_mutex)) {
		ret = -EINTR;
		mutex_unlock(&rtdex_dev->rx_mutex);
		goto out;
	}

	if (mutex_lock_interruptible(&rtdex_dev->tx_dma_transfer_lock)) {
		ret = -EINTR;
		mutex_unlock(&rtdex_dev->rx_mutex);
		mutex_unlock(&rtdex_dev->tx_mutex);
		goto out;
	}

	if (mutex_lock_interruptible(&rtdex_dev->rx_dma_transfer_lock)) {
		ret = -EINTR;
		mutex_unlock(&rtdex_dev->rx_mutex);
		mutex_unlock(&rtdex_dev->tx_mutex);
		mutex_unlock(&rtdex_dev->tx_dma_transfer_lock);
		goto out;
	}

	switch (cmd) {
	case RTDEX_RESET_ALL:

		rtdex_close_rx();
		rtdex_close_tx();

		rtdex_init_tx();
		rtdex_init_rx();

		break;

	case RTDEX_SET_BURST_LENGTH:

		if (arg > 16 || arg < 1) {
			ret = -EINVAL;
			goto out;
		}

		rtdex_dev->burst_length = arg;

		/* Set tx DMA */
		rtdex_dev->tx_client_data->dev_bus_des.burst_len = rtdex_dev->burst_length;
		rtdex_dev->tx_client_data->mem_bus_des.burst_len = rtdex_dev->burst_length;
		set_pl330_client_data(rtdex_dev->tx_dma_channel, rtdex_dev->tx_client_data);

		/* Set rx DMA */
		rtdex_dev->rx_client_data->dev_bus_des.burst_len = rtdex_dev->burst_length;
		rtdex_dev->rx_client_data->mem_bus_des.burst_len = rtdex_dev->burst_length;
		set_pl330_client_data(rtdex_dev->rx_dma_channel, rtdex_dev->rx_client_data);

		break;

	case RTDEX_SET_BLOCK_SIZE:

		if (arg < 1 || arg > RTDEX_FIFO_SIZE) {
			ret = -EINVAL;
			goto out;
		}

		rtdex_dev->block_size = arg;
		regs->read_byte = arg;
		regs->write_byte = RTDEX_FIFO_SIZE - arg;

		break;

	case RTDEX_SET_READ_CAST_MODE:

		if (arg < 0 || arg > 2) {
			ret = -EINVAL;
			goto out;
		}

		regs->ctrl &= ~(RTDEX_CTRL_READ_CAST_MODE);
		mb();
		regs->ctrl |= (arg & 0x3) << RTDEX_CTRL_READ_CAST_MODE_OFFSET;
		mb();

		break;

	case RTDEX_SET_WRITE_CAST_MODE:

		if (arg < 0 || arg > 2) {
			ret = -EINVAL;
			goto out;
		}

		regs->ctrl &= ~(RTDEX_CTRL_WRITE_CAST_MODE);
		mb();
		regs->ctrl |= (arg & 0x3) << RTDEX_CTRL_WRITE_CAST_MODE_OFFSET;
		mb();

		break;
	}

	mutex_unlock(&rtdex_dev->rx_mutex);
	mutex_unlock(&rtdex_dev->tx_mutex);
	mutex_unlock(&rtdex_dev->rx_dma_transfer_lock);
	mutex_unlock(&rtdex_dev->tx_dma_transfer_lock);

	if (cmd == RTDEX_RESET_ALL)
		regs->ctrl |= RTDEX_CTRL_READ_INTERRUPT_EN;

out:
	mutex_unlock(&rtdex_dev->gen_mutex);
	return ret;
}

struct file_operations rtdex_fops = {
	.owner = THIS_MODULE,
	.poll = rtdex_poll,
	.read = rtdex_read,
	.write = rtdex_write,
	.release = rtdex_release,
	.open = rtdex_open,
	.unlocked_ioctl = rtdex_ioctl
};

/* Driver /proc filesystem operations so that we can show some statistics */
static void *rtdex_proc_seq_start(struct seq_file *s, loff_t *pos)
{
	if (*pos == 0) {
		return rtdex_dev;
	}

	return NULL;
}

static void *rtdex_proc_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	return NULL;
}

static void rtdex_proc_seq_stop(struct seq_file *s, void *v)
{

}

static int rtdex_proc_seq_show(struct seq_file *s, void *v)
{
	volatile rtdex_regs *regs;
	unsigned long tx_head, tx_tail;
	unsigned long rx_head, rx_tail;

	if (mutex_lock_interruptible(&rtdex_dev->gen_mutex)) {
		return -EINTR;
	}

	regs = rtdex_dev->dev_virtaddr;

	tx_head = rtdex_dev->tx_buffer.head;
	tx_tail = ACCESS_ONCE(rtdex_dev->tx_buffer.tail);

	rx_head = ACCESS_ONCE(rtdex_dev->rx_buffer.head);
	rx_tail = rtdex_dev->rx_buffer.tail;

	seq_printf(s, "\nNutaq RTDEx FIFO DMA:\n\n");
	seq_printf(s, "Device Physical Address:	0x%0x\n", rtdex_dev->dev_physaddr);
	seq_printf(s, "Device Virtual Address:		0x%0x\n", (u32)rtdex_dev->dev_virtaddr);
	seq_printf(s, "Device Address Space:		%d bytes\n", rtdex_dev->dev_addrsize);
	seq_printf(s, "TX DMA Channel:			%d\n", rtdex_dev->tx_dma_channel);
	seq_printf(s, "RX DMA Channel:			%d\n", rtdex_dev->rx_dma_channel);
	seq_printf(s, "Block size:			%d\n", rtdex_dev->block_size);
	seq_printf(s, "Burst Length:			%d\n", rtdex_dev->burst_length);
	seq_printf(s, "\n");
	seq_printf(s, "TX overflow:			%d\n", rtdex_dev->tx_overflow);
	seq_printf(s, "TX underflow:			%d\n", rtdex_dev->tx_underflow);
	seq_printf(s, "RX overflow:			%d\n", rtdex_dev->rx_overflow);
	seq_printf(s, "RX underflow:			%d\n", rtdex_dev->rx_underflow);
	seq_printf(s, "\n");
	seq_printf(s, "FIFO w space:			%d\n", RTDEX_FIFO_SIZE - regs->write_byte);
	seq_printf(s, "FIFO w count:			%d\n", regs->write_byte);
	seq_printf(s, "FIFO r space:			%d\n", RTDEX_FIFO_SIZE - regs->read_byte);
	seq_printf(s, "FIFO r count:			%d\n", regs->read_byte);
	seq_printf(s, "\n");
	seq_printf(s, "TX space:			%lu\n", CIRC_SPACE(tx_head, tx_tail, RTDEX_BUF_SIZE));
	seq_printf(s, "TX count:			%lu\n", CIRC_CNT(tx_head, tx_tail, RTDEX_BUF_SIZE));
	seq_printf(s, "RX space:			%lu\n", CIRC_SPACE(rx_head, rx_tail, RTDEX_BUF_SIZE));
	seq_printf(s, "RX count:			%lu\n", CIRC_CNT(rx_head, rx_tail, RTDEX_BUF_SIZE));
	seq_printf(s, "\n");
	seq_printf(s, "TX irq:				%d\n", rtdex_dev->tx_irq);
	seq_printf(s, "RX irq:				%d\n", rtdex_dev->rx_irq);
	seq_printf(s, "\n");
	seq_printf(s, "TX dma:				%d\n", rtdex_dev->tx_dma);
	seq_printf(s, "RX dma:				%d\n", rtdex_dev->rx_dma);

	mutex_unlock(&rtdex_dev->gen_mutex);
	return 0;
}

/* SEQ operations for /proc */
static struct seq_operations rtdex_proc_seq_ops = {
	.start = rtdex_proc_seq_start,
	.next = rtdex_proc_seq_next,
	.stop = rtdex_proc_seq_stop,
	.show = rtdex_proc_seq_show
};

static int rtdex_proc_open(struct inode *inode, struct file *file)
{
    return seq_open(file, &rtdex_proc_seq_ops);
}

static struct file_operations rtdex_proc_ops = {
	.owner = THIS_MODULE,
	.open = rtdex_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release
};

#ifdef CONFIG_OF
static struct of_device_id rtdex_of_match[] __devinitdata = {
	{ .compatible = "xlnx,fifo-dma", },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, rtdex_of_match);
#else
#define rtdex_of_match NULL
#endif /* CONFIG_OF */

static int rtdex_remove(struct platform_device *pdev)
{
	free_irq(rtdex_dev->irq_write_byte, rtdex_dev);
	free_irq(rtdex_dev->irq_read_byte, rtdex_dev);

	cdev_del(&rtdex_dev->cdev);
	remove_proc_entry("driver/rtdex", NULL);
	unregister_chrdev_region(rtdex_dev->devno, 1);

	/* Unmap the I/O memory */
	if (rtdex_dev->dev_virtaddr) {
		iounmap(rtdex_dev->dev_virtaddr);
		release_mem_region(rtdex_dev->dev_physaddr,
			rtdex_dev->dev_addrsize);
	}

	/* Free the PL330 buffer client data descriptors */
	if (rtdex_dev->tx_client_data) {
		kfree(rtdex_dev->tx_client_data);
	}

	if (rtdex_dev->rx_client_data) {
		kfree(rtdex_dev->rx_client_data);
	}

	if (rtdex_dev) {
		kfree(rtdex_dev);
	}

	return 0;
}

static int rtdex_probe(struct platform_device *pdev)
{
	int ret;
	struct proc_dir_entry *proc_entry;
	struct resource *rtdex_resource;
	volatile rtdex_regs *regs;

	/* Get our platform device resources */
	PDEBUG("We have %d resources\n", pdev->num_resources);
	rtdex_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (rtdex_resource == NULL) {
		dev_err(&pdev->dev, "No resources found\n");
		return -ENODEV;
	}

	/* Allocate a private structure to manage this device */
	rtdex_dev = kmalloc(sizeof(struct rtdex_dev), GFP_KERNEL);
	if (rtdex_dev == NULL) {
		dev_err(&pdev->dev,
			"unable to allocate device structure\n");
		return -ENOMEM;
	}
	memset(rtdex_dev, 0, sizeof(struct rtdex_dev));

	/* Get our device properties from the device tree, if they exist */
	if (pdev->dev.of_node) {
		if (of_property_read_u32(pdev->dev.of_node, "dma-channel",
			&rtdex_dev->tx_dma_channel) < 0) {
			dev_warn(&pdev->dev,
				"DMA channel unspecified - assuming 0\n");
			rtdex_dev->tx_dma_channel = 0;
		}
		dev_info(&pdev->dev,
			"read DMA channel is %d\n", rtdex_dev->rx_dma_channel);

		if (of_property_read_u32(pdev->dev.of_node, "block-size",
			&rtdex_dev->block_size) < 0) {
			dev_warn(&pdev->dev,
				"block size unspecified - assuming 4096\n");
			rtdex_dev->block_size = 4096;
		}

		if (of_property_read_u32(pdev->dev.of_node, "burst-length",
			&rtdex_dev->burst_length) < 0) {
			dev_warn(&pdev->dev,
				"burst length unspecified - assuming 0\n");
			rtdex_dev->burst_length = 0;
		}

		if (of_property_read_u32(pdev->dev.of_node, "interrupt_read",
			&rtdex_dev->irq_read_byte) < 0) {
			dev_warn(&pdev->dev,
				"read irq not specified, disabling irq");
			rtdex_dev->irq_read_byte = 0;
		}
		dev_info(&pdev->dev,
			"read irq: %d\n", rtdex_dev->irq_read_byte);

		if (of_property_read_u32(pdev->dev.of_node, "interrupt_write",
			&rtdex_dev->irq_write_byte) < 0) {
			dev_warn(&pdev->dev,
				"write irq not specified, disabling irq");
			rtdex_dev->irq_write_byte = 0;
		}
		dev_info(&pdev->dev,
			"write irq: %d\n", rtdex_dev->irq_write_byte);

		dev_info(&pdev->dev,
			"DMA burst length is %d\n", rtdex_dev->burst_length);

		dev_info(&pdev->dev, "DMA block size is %d\n", rtdex_dev->block_size);
	}

	rtdex_dev->pdev = pdev;

	rtdex_dev->devno = MKDEV(rtdex_major, RTDEX_MINOR);
	PDEBUG("devno is 0x%0x, pdev id is %d %d\n", rtdex_dev->devno, rtdex_major, RTDEX_MINOR);

	ret = register_chrdev_region(rtdex_dev->devno, 1, MODULE_NAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to register chrdev %d\n", rtdex_major);
		goto fail;
	}

	/* Register with the kernel as a character device */
	cdev_init(&rtdex_dev->cdev, &rtdex_fops);
	rtdex_dev->cdev.owner = THIS_MODULE;
	rtdex_dev->cdev.ops = &rtdex_fops;

	/* Initialize our device mutex */
	mutex_init(&rtdex_dev->gen_mutex);
	mutex_init(&rtdex_dev->tx_mutex);
	mutex_init(&rtdex_dev->rx_mutex);

	rtdex_dev->dev_physaddr = rtdex_resource->start;
	rtdex_dev->dev_addrsize = rtdex_resource->end -
		rtdex_resource->start + 1;

	PDEBUG("physical address start: 0x%x end: 0x%x\n", rtdex_resource->start, rtdex_resource->end);
	if (!request_mem_region(rtdex_dev->dev_physaddr, rtdex_dev->dev_addrsize, MODULE_NAME)) {
		dev_err(&pdev->dev, "can't reserve i/o memory at 0x%08X\n", rtdex_dev->dev_physaddr);
		ret = -ENODEV;
		goto fail;
	}

	rtdex_dev->dev_virtaddr = ioremap(rtdex_dev->dev_physaddr, rtdex_dev->dev_addrsize);
	PDEBUG("rtdex: mapped 0x%0x to 0x%0x\n", rtdex_dev->dev_physaddr,
		(unsigned int)rtdex_dev->dev_virtaddr);

	rtdex_dev->tx_client_data = kmalloc(sizeof(struct pl330_client_data), GFP_KERNEL);
	if (!rtdex_dev->tx_client_data) {
		dev_err(&pdev->dev, "can't allocate PL330 client data\n");
		goto fail;
	}
	memset(rtdex_dev->tx_client_data, 0, sizeof(struct pl330_client_data));

	rtdex_dev->rx_client_data = kmalloc(sizeof(struct pl330_client_data), GFP_KERNEL);
	if (!rtdex_dev->tx_client_data) {
		dev_err(&pdev->dev, "can't allocate PL330 client data\n");
		goto fail;
	}
	memset(rtdex_dev->rx_client_data, 0, sizeof(struct pl330_client_data));

	init_waitqueue_head(&rtdex_dev->rx_wq);
	init_waitqueue_head(&rtdex_dev->tx_wq);

	init_waitqueue_head(&rtdex_dev->rx_data_avail_wq);
	init_waitqueue_head(&rtdex_dev->tx_data_avail_wq);

	rtdex_dev->writers = 0;
	rtdex_dev->readers = 0;

	/* Program interrupt value */
	regs = rtdex_dev->dev_virtaddr;
	regs->read_byte = rtdex_dev->block_size;
	regs->write_byte = RTDEX_FIFO_SIZE - rtdex_dev->block_size;

	/* disable interrupt */
	regs->ctrl &= ~RTDEX_CTRL_WRITE_INTERRUPT_EN;
	regs->ctrl &= ~RTDEX_CTRL_READ_INTERRUPT_EN;

	/* Set tx DMA */
	rtdex_dev->tx_client_data->dev_addr = rtdex_dev->dev_physaddr + AXI_TXFIFO;
	rtdex_dev->tx_client_data->dev_bus_des.burst_size = RTDEX_BURST_SIZE;
	rtdex_dev->tx_client_data->dev_bus_des.burst_len = rtdex_dev->burst_length;
	rtdex_dev->tx_client_data->mem_bus_des.burst_size = RTDEX_BURST_SIZE;
	rtdex_dev->tx_client_data->mem_bus_des.burst_len = rtdex_dev->burst_length;

	/* Set rx DMA */
	rtdex_dev->rx_client_data->dev_addr = rtdex_dev->dev_physaddr + AXI_TXFIFO;
	rtdex_dev->rx_client_data->dev_bus_des.burst_size = RTDEX_BURST_SIZE;
	rtdex_dev->rx_client_data->dev_bus_des.burst_len = rtdex_dev->burst_length;
	rtdex_dev->rx_client_data->mem_bus_des.burst_size = RTDEX_BURST_SIZE;
	rtdex_dev->rx_client_data->mem_bus_des.burst_len = rtdex_dev->burst_length;

	ret = cdev_add(&rtdex_dev->cdev, rtdex_dev->devno, 1);

	/* Create statistics entry under /proc */
	proc_entry = create_proc_entry("driver/rtdex", 0, NULL);
	if (proc_entry) {
		proc_entry->proc_fops = &rtdex_proc_ops;
	}

	/* request IRQ */
	if (request_irq(rtdex_dev->irq_write_byte, (irq_handler_t)tx_space_available, 0, "rtdex", rtdex_dev)) {
		dev_err(&pdev->dev, "can't get write interrupt\n");
	}

	if (request_irq(rtdex_dev->irq_read_byte, (irq_handler_t)rx_space_available, 0, "rtdex", rtdex_dev)) {
		dev_err(&pdev->dev, "can't get read interrupt\n");
	}

	/* initialize circular buffers */
	rtdex_init_rx();
	rtdex_init_tx();

	mutex_init(&rtdex_dev->tx_dma_transfer_lock);
	mutex_init(&rtdex_dev->rx_dma_transfer_lock);

	dev_info(&pdev->dev, "Loaded Nutaq RTDEx FIFO DMA successfully\n");

	return 0;

fail:
	rtdex_remove(pdev);
	return ret;
}

static struct platform_driver rtdex_driver = {
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = rtdex_of_match,
	},
		.remove = rtdex_remove,
		.probe = rtdex_probe,
};

static void __exit rtdex_exit(void)
{
	rtdex_close_rx();
	rtdex_close_tx();

	platform_driver_unregister(&rtdex_driver);
}

static int __init rtdex_init(void)
{
	return platform_driver_register(&rtdex_driver);
}

module_init(rtdex_init);
module_exit(rtdex_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Nutaq RTDEx FIFO DMA");
MODULE_AUTHOR("Nutaq, Inc.");
MODULE_VERSION("1.00");

