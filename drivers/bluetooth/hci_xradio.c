// SPDX-License-Identifier: GPL-2.0-only
/* Xradio XR829 Bluetooth UART driver. */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <crypto/sha2.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include "hci_uart.h"

#define XR_FW "xr829/fw_xr829_bt.bin"
#define XR_FW_SIZE 319920
static const u8 xr_digest[32] = { 0x88, 0xd6, 0x89, 0xdb, 0x80, 0x14, 0x45,
				  0x30, 0x0a, 0x0c, 0x34, 0x35, 0xe0, 0x7b,
				  0x11, 0x38, 0x24, 0xe5, 0xf6, 0xd6, 0xad,
				  0x9e, 0xc7, 0x5e, 0x16, 0xe5, 0x46, 0xcf,
				  0xf4, 0x50, 0xa7, 0xf6 };
struct xr {
	struct hci_uart hu;
	struct gpio_desc *reset, *wake, *host_wake;
	int host_wake_irq;
	bool host_wake_enabled;
	struct regulator_bulk_data supplies[2];
	struct clk_bulk_data clocks[2];
	bool powered, registered;
	bool raw_pending;
	struct sk_buff_head txq;
	struct sk_buff *rx_skb;
	spinlock_t rx_lock;
	bool h4, awaiting;
	u8 reply[12];
	size_t expected, received;
	int rx_error;
	struct completion response, sent;
	unsigned long end;
};
static struct xr *xr_of(struct hci_uart *hu)
{
	return container_of(hu, struct xr, hu);
}
static unsigned long xr_left(unsigned long end)
{
	return time_before(jiffies, end) ? end - jiffies : 0;
}
static irqreturn_t xr_host_wake(int irq, void *data)
{
	struct xr *xr = data;

	/* The UART remains active. Account for controller activity without
	 * promising system suspend or controller low-power support.
	 */
	pm_wakeup_event(&xr->hu.serdev->dev, 0);
	return IRQ_HANDLED;
}
static void xr_host_wake_disable(struct xr *xr)
{
	if (xr->host_wake_enabled) {
		disable_irq(xr->host_wake_irq);
		xr->host_wake_enabled = false;
	}
}
static void xr_host_wake_cleanup(void *data)
{
	struct xr *xr = data;

	xr_host_wake_disable(xr);
	device_init_wakeup(&xr->hu.serdev->dev, false);
}
static int xr_power_off(struct xr *xr)
{
	unsigned long flags;
	int err = 0;

	xr_host_wake_disable(xr);
	gpiod_set_value_cansleep(xr->reset, 1);
	gpiod_set_value_cansleep(xr->wake, 0);
	spin_lock_irqsave(&xr->rx_lock, flags);
	xr->h4 = false;
	xr->awaiting = false;
	xr->rx_error = -ESHUTDOWN;
	complete_all(&xr->response);
	kfree_skb(xr->rx_skb);
	xr->rx_skb = NULL;
	spin_unlock_irqrestore(&xr->rx_lock, flags);
	if (xr->powered) {
		msleep(20);
		err = regulator_bulk_disable(ARRAY_SIZE(xr->supplies),
					     xr->supplies);
		if (!err) {
			clk_bulk_disable_unprepare(ARRAY_SIZE(xr->clocks), xr->clocks);
			xr->powered = false;
		}
	}
	return err;
}
static int xr_shutdown(struct hci_dev *hdev)
{
	return xr_power_off(xr_of(hci_get_drvdata(hdev)));
}
static bool xr_wakeup(struct hci_dev *hdev)
{
	return false;
}
static int xr_configure(struct xr *xr, unsigned int baud, bool flow)
{
	unsigned int actual = serdev_device_set_baudrate(xr->hu.serdev, baud);

	if (actual != baud)
		return -ERANGE;
	serdev_device_set_flow_control(xr->hu.serdev, flow);
	return 0;
}
static int xr_power_on(struct xr *xr)
{
	int err;

	err = xr_power_off(xr);
	if (err)
		return err;
	/* Configure the UART before releasing the controller from reset. */
	msleep(500);
	err = clk_bulk_prepare_enable(ARRAY_SIZE(xr->clocks), xr->clocks);
	if (err)
		return err;
	err = regulator_bulk_enable(ARRAY_SIZE(xr->supplies), xr->supplies);
	if (err) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(xr->clocks), xr->clocks);
		return err;
	}
	xr->powered = true;
	if (xr->host_wake) {
		enable_irq(xr->host_wake_irq);
		xr->host_wake_enabled = true;
	}
	msleep(20);
	gpiod_set_value_cansleep(xr->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(xr->wake, 1);
	usleep_range(5000, 6000);
	return 0;
}
static u16 xr_sum(const u8 *p, size_t n)
{
	u16 sum = 0;

	while (n > 1) {
		sum += get_unaligned_le16(p);
		p += 2;
		n -= 2;
	}
	if (n)
		sum += *p;
	return sum;
}
static size_t xr_packet(u8 *p, u8 cmd, u32 arg, const u8 *data, size_t size)
{
	u32 payload = cmd == 9 ? 11 : 5;
	u16 data_cs = data ? ~xr_sum(data, size) : 0, cs;

	memset(p, 0, 23);
	memcpy(p, "BROM", 4);
	p[4] = 4;
	put_unaligned_le32(payload, p + 8);
	p[12] = cmd;
	put_unaligned_le32(arg, p + 13);
	if (cmd == 9) {
		put_unaligned_le32(size, p + 17);
		put_unaligned_le16(data_cs, p + 21);
	}
	cs = ~xr_sum(p, payload + 12);
	put_unaligned_be16(cs, p + 6);
	put_unaligned_be32(payload, p + 8);
	put_unaligned_be32(arg, p + 13);
	if (cmd == 9) {
		put_unaligned_be32(size, p + 17);
		put_unaligned_be16(data_cs, p + 21);
	}
	return payload + 12;
}
/* Raw ROM transfers use the existing serdev TX worker. Completion in dequeue
 * means the preceding skb was fully accepted by the UART driver, not drained
 * electrically. A received ROM reply proves actual transmission completed.
 */
static int xr_transfer(struct xr *xr, const u8 *bytes, size_t size,
		       size_t expected, unsigned long end)
{
	struct sk_buff *skb;
	unsigned long flags, left;
	int err;

	if (!size || size > 1024 || !expected || expected > sizeof(xr->reply))
		return -EINVAL;
	if (time_after(end, xr->end))
		end = xr->end;
	if (!xr_left(end))
		return -ETIMEDOUT;
	skb = bt_skb_alloc(size, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, bytes, size);
	hci_skb_pkt_type(skb) = 0; /* Raw boot bytes, no H4 prefix. */
	flush_work(&xr->hu.write_work);
	reinit_completion(&xr->sent);
	reinit_completion(&xr->response);
	spin_lock_irqsave(&xr->rx_lock, flags);
	xr->expected = expected;
	xr->received = 0;
	xr->rx_error = 0;
	xr->awaiting = true;
	spin_unlock_irqrestore(&xr->rx_lock, flags);
	skb_queue_tail(&xr->txq, skb);
	hci_uart_tx_wakeup(&xr->hu);
	left = xr_left(end);
	if (!left || !wait_for_completion_timeout(&xr->sent, left)) {
		err = -EIO; /* Never retry a write whose completion is unknown. */
		goto done;
	}
	left = xr_left(end);
	if (left)
		wait_for_completion_timeout(&xr->response, left);
	spin_lock_irqsave(&xr->rx_lock, flags);
	err = xr->rx_error;
	if (!err && xr->received != expected)
		err = xr->received ? -EPROTO : -ETIMEDOUT;
	xr->awaiting = false;
	spin_unlock_irqrestore(&xr->rx_lock, flags);
	return err;
done:
	spin_lock_irqsave(&xr->rx_lock, flags);
	xr->awaiting = false;
	spin_unlock_irqrestore(&xr->rx_lock, flags);
	return err;
}
static int xr_ack(struct xr *xr)
{
	u8 p[12];

	memcpy(p, xr->reply, sizeof(p));
	/* Live ROM varies the entire version/reserved byte; stock ignores it
	 * except as part of CHECK checksum. Preserve all bits in the sum.
	 */
	if (memcmp(p, "BROM", 4) || !(p[4] & 2) || (p[4] & ~6) ||
	    get_unaligned_be32(p + 8))
		return -EPROTO;
	if (p[4] & 4) {
		put_unaligned_le16(get_unaligned_be16(p + 6), p + 6);
		if (xr_sum(p, sizeof(p)) != 0xffff)
			return -EBADMSG;
	}
	return 0;
}
static int xr_rom(struct xr *xr, u8 cmd, u32 arg, const u8 *data, size_t size)
{
	u8 packet[23];
	unsigned long end = jiffies + msecs_to_jiffies(2000);
	size_t n = xr_packet(packet, cmd, arg, data, size);
	int err = xr_transfer(xr, packet, n, 12, end);

	if (!err)
		err = xr_ack(xr);
	if (!err && data)
		err = xr_transfer(xr, data, size, 12, end);
	if (!err && data)
		err = xr_ack(xr);
	return err;
}
static int xr_sync(struct xr *xr)
{
	u8 sync = 0x55;
	int err;

	for (unsigned int i = 0; i < 20; i++) {
		err = xr_transfer(xr, &sync, 1, 2,
				  jiffies + msecs_to_jiffies(20));
		if (err == -ETIMEDOUT && xr_left(xr->end))
			continue;
		if (err)
			return err;
		return memcmp(xr->reply, "OK", 2) ? -EPROTO : 0;
	}
	return -ETIMEDOUT;
}
static const struct h4_recv_pkt xr_pkts[] = {
	{ H4_RECV_ACL, .recv = hci_recv_frame },
	{ H4_RECV_SCO, .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
};
static int xr_recv(struct hci_uart *hu, const void *data, int count)
{
	struct xr *xr = xr_of(hu);
	unsigned long flags;

	if (count <= 0)
		return count;
	spin_lock_irqsave(&xr->rx_lock, flags);
	if (xr->h4) {
		xr->rx_skb = h4_recv_buf(hu, xr->rx_skb, data, count, xr_pkts,
					 ARRAY_SIZE(xr_pkts));
		if (IS_ERR(xr->rx_skb)) {
			hu->hdev->stat.err_rx++;
			xr->rx_skb = NULL;
		}
	} else if (xr->awaiting) {
		size_t remaining = xr->expected - xr->received;
		size_t copy = min_t(size_t, count, remaining);

		memcpy(xr->reply + xr->received, data, copy);
		xr->received += copy;
		if (copy != (size_t)count)
			xr->rx_error = -EPROTO;
		if (xr->received == xr->expected || xr->rx_error)
			complete(&xr->response);
	}
	spin_unlock_irqrestore(&xr->rx_lock, flags);
	return count;
}
static int xr_enqueue(struct hci_uart *hu, struct sk_buff *skb)
{
	struct xr *xr = xr_of(hu);

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&xr->txq, skb);
	return 0;
}
static struct sk_buff *xr_dequeue(struct hci_uart *hu)
{
	struct xr *xr = xr_of(hu);
	struct sk_buff *skb;
	unsigned long flags;

	/* The helper calls dequeue again only after accepting all bytes of its
	 * current skb. A partial write remains hu->tx_skb and bypasses dequeue.
	 * Empty wakeups before a raw skb is dequeued must not complete sent.
	 * Serialize completion state with flush, which can run concurrently.
	 */
	spin_lock_irqsave(&xr->txq.lock, flags);
	if (xr->raw_pending) {
		xr->raw_pending = false;
		complete(&xr->sent);
	}
	skb = __skb_dequeue(&xr->txq);
	if (skb && !hci_skb_pkt_type(skb))
		xr->raw_pending = true;
	spin_unlock_irqrestore(&xr->txq.lock, flags);

	return skb;
}
static int xr_flush(struct hci_uart *hu)
{
	struct xr *xr = xr_of(hu);
	struct sk_buff_head purge;
	unsigned long flags;

	skb_queue_head_init(&purge);
	spin_lock_irqsave(&xr->txq.lock, flags);
	xr->raw_pending = false;
	skb_queue_splice_init(&xr->txq, &purge);
	spin_unlock_irqrestore(&xr->txq.lock, flags);
	skb_queue_purge(&purge);

	return 0;
}
static int xr_open(struct hci_uart *hu)
{
	hu->priv = xr_of(hu);
	return 0;
}
static int xr_close(struct hci_uart *hu)
{
	int err = xr_power_off(xr_of(hu));

	xr_flush(hu);
	return err;
}
static int xr_hci_cmd(struct hci_dev *hdev, u16 opcode, const void *params,
		      u8 plen, void *reply, size_t reply_size)
{
	struct xr *xr = xr_of(hci_get_drvdata(hdev));
	unsigned long left =
		min_t(unsigned long, xr_left(xr->end), HCI_INIT_TIMEOUT);
	struct sk_buff *skb;
	int err = 0;

	if (!left)
		return -ETIMEDOUT;
	skb = __hci_cmd_sync(hdev, opcode, plen, params, left);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	if (skb->len != reply_size || !skb->len || skb->data[0])
		err = -EPROTO;
	else if (reply)
		memcpy(reply, skb->data, reply_size);
	kfree_skb(skb);
	return err;
}
static int xr_setup(struct hci_uart *hu)
{
	struct xr *xr = xr_of(hu);
	struct hci_dev *hdev = hu->hdev;
	const struct firmware *fw;
	u8 digest[32], version[9], check[9], addr[7];
	__le32 baud = cpu_to_le32(1500000);
	unsigned long flags;
	int err;

	hdev->shutdown = xr_shutdown;
	hdev->wakeup = xr_wakeup;
	hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	err = request_firmware(&fw, XR_FW, &hu->serdev->dev);
	if (err)
		return err;
	if (fw->size != XR_FW_SIZE) {
		err = -EINVAL;
		goto release;
	}
	sha256(fw->data, fw->size, digest);
	if (memcmp(digest, xr_digest, sizeof(digest))) {
		err = -EBADMSG;
		goto release;
	}
	xr->end = jiffies + msecs_to_jiffies(90000);
	err = xr_configure(xr, 115200, false);
	if (err)
		goto off;
	err = xr_power_on(xr);
	if (err)
		goto off;
	err = xr_sync(xr);
	if (err)
		goto off;
	err = xr_rom(xr, 0x10, 1500000 | 3U << 24, NULL, 0);
	if (err)
		goto off;
	err = xr_configure(xr, 1500000, false);
	if (err)
		goto off;
	err = xr_sync(xr);
	if (err)
		goto off;
	for (size_t offset = 0; offset < fw->size; offset += 1024) {
		err = xr_rom(xr, 9, offset, fw->data + offset,
			     min_t(size_t, 1024, fw->size - offset));
		if (err)
			goto off;
	}
	err = xr_rom(xr, 0x13, 0, NULL, 0);
	if (err)
		goto off;
	msleep(50);
	err = xr_configure(xr, 115200, true);
	if (err)
		goto off;
	msleep(20);
	spin_lock_irqsave(&xr->rx_lock, flags);
	xr->h4 = true;
	spin_unlock_irqrestore(&xr->rx_lock, flags);
	err = xr_hci_cmd(hdev, HCI_OP_RESET, NULL, 0, NULL, 1);
	if (err)
		goto off;
	err = xr_hci_cmd(hdev, HCI_OP_READ_LOCAL_VERSION, NULL, 0, version,
			 sizeof(version));
	if (err)
		goto off;
	err = xr_hci_cmd(hdev, HCI_OP_READ_BD_ADDR, NULL, 0, addr,
			 sizeof(addr));
	if (err)
		goto off;
	err = xr_hci_cmd(hdev, 0xfc18, &baud, sizeof(baud), NULL, 1);
	if (err)
		goto off;
	err = xr_configure(xr, 1500000, true);
	if (err)
		goto off;
	msleep(20);
	err = xr_hci_cmd(hdev, HCI_OP_READ_LOCAL_VERSION, NULL, 0, check,
			 sizeof(check));
	if (err)
		goto off;
	if (memcmp(version, check, sizeof(check))) {
		err = -EPROTO;
		goto off;
	}
	bt_dev_info(
		hdev,
		"stock XR829 profile ready; manufacturer %u revision %u; observed address %pMR",
		get_unaligned_le16(version + 5),
		get_unaligned_le16(version + 2), addr + 1);
	release_firmware(fw);
	return 0;
off:
	bt_dev_err(hdev, "stock profile setup failed: %d", err);
	if (xr_power_off(xr))
		dev_err(&hu->serdev->dev, "failed to release radio supplies\n");
release:
	release_firmware(fw);
	return err;
}
static const struct hci_uart_proto xr_proto = {
	.id = HCI_UART_H4,
	.name = "XR829",
	.manufacturer = 1597,
	.open = xr_open,
	.close = xr_close,
	.flush = xr_flush,
	.setup = xr_setup,
	.recv = xr_recv,
	.enqueue = xr_enqueue,
	.dequeue = xr_dequeue,
};
static int xr_probe(struct serdev_device *serdev)
{
	struct xr *xr;
	int err;

	xr = devm_kzalloc(&serdev->dev, sizeof(*xr), GFP_KERNEL);
	if (!xr)
		return -ENOMEM;
	xr->hu.serdev = serdev;
	skb_queue_head_init(&xr->txq);
	spin_lock_init(&xr->rx_lock);
	init_completion(&xr->response);
	init_completion(&xr->sent);
	xr->supplies[0].supply = "vbat";
	xr->supplies[1].supply = "vddio";
	err = devm_regulator_bulk_get(&serdev->dev, ARRAY_SIZE(xr->supplies),
				      xr->supplies);
	if (err)
		return dev_err_probe(&serdev->dev, err, "radio supplies\n");
	xr->clocks[0].id = "lpo";
	xr->clocks[1].id = "ref";
	err = devm_clk_bulk_get(&serdev->dev, ARRAY_SIZE(xr->clocks), xr->clocks);
	if (err)
		return dev_err_probe(&serdev->dev, err, "radio clocks\n");
	xr->reset = devm_gpiod_get(&serdev->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xr->reset))
		return dev_err_probe(&serdev->dev, PTR_ERR(xr->reset),
				     "reset\n");
	xr->wake = devm_gpiod_get(&serdev->dev, "device-wake", GPIOD_OUT_LOW);
	if (IS_ERR(xr->wake))
		return dev_err_probe(&serdev->dev, PTR_ERR(xr->wake),
				     "device wake\n");
	xr->host_wake = devm_gpiod_get_optional(&serdev->dev, "host-wake", GPIOD_IN);
	if (IS_ERR(xr->host_wake))
		return dev_err_probe(&serdev->dev, PTR_ERR(xr->host_wake),
				     "host wake\n");
	if (xr->host_wake) {
		unsigned long trigger = gpiod_is_active_low(xr->host_wake) ?
			IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;

		xr->host_wake_irq = gpiod_to_irq(xr->host_wake);
		if (xr->host_wake_irq < 0)
			return dev_err_probe(&serdev->dev, xr->host_wake_irq,
					     "host wake IRQ\n");
		err = devm_request_irq(&serdev->dev, xr->host_wake_irq,
				       xr_host_wake, trigger | IRQF_NO_AUTOEN,
				       "xr829-host-wake", xr);
		if (err)
			return dev_err_probe(&serdev->dev, err, "host wake IRQ\n");
		err = device_init_wakeup(&serdev->dev, true);
		if (err)
			return err;
		err = devm_add_action_or_reset(&serdev->dev, xr_host_wake_cleanup, xr);
		if (err)
			return err;
	}
	serdev_device_set_drvdata(serdev, &xr->hu);
	err = hci_uart_register_device(&xr->hu, &xr_proto);
	if (!err)
		xr->registered = true;
	return err;
}
static void xr_remove(struct serdev_device *serdev)
{
	struct hci_uart *hu = serdev_device_get_drvdata(serdev);

	if (xr_of(hu)->registered) {
		xr_of(hu)->registered = false;
		hci_uart_unregister_device(hu);
	}
}
static void xr_system_shutdown(struct serdev_device *serdev)
{
	struct hci_uart *hu = serdev_device_get_drvdata(serdev);
	/* System shutdown unregisters HCI before releasing hardware ownership. */
	if (xr_of(hu)->registered) {
		xr_of(hu)->registered = false;
		hci_uart_unregister_device(hu);
	}
}
static int xr_suspend(struct device *dev)
{
	return -EOPNOTSUPP;
}
static const struct dev_pm_ops xr_pm = { .suspend = xr_suspend,
					 .freeze = xr_suspend,
					 .poweroff = xr_suspend };
static const struct of_device_id xr_match[] = {
	{ .compatible = "xradio,xr829-bt" },
	{},
};
MODULE_DEVICE_TABLE(of, xr_match);
static struct serdev_device_driver xr_driver = {
	.probe = xr_probe,
	.remove = xr_remove,
	.shutdown = xr_system_shutdown,
	.driver = { .name = "hci-xradio",
		    .of_match_table = xr_match,
		    .pm = &xr_pm },
};
module_serdev_device_driver(xr_driver);
MODULE_FIRMWARE(XR_FW);
MODULE_DESCRIPTION("Xradio XR829 Bluetooth UART driver");
MODULE_LICENSE("GPL");
