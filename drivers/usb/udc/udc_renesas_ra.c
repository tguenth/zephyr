/*
 * Copyright (c) 2024 Renesas Electronics Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "udc_common.h"

#include <soc.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/clock_control/renesas_ra_cgc.h>
#include <zephyr/drivers/usb/udc.h>
#include "r_usb_device.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_renesas_ra, CONFIG_UDC_DRIVER_LOG_LEVEL);

struct udc_renesas_ra_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device **clocks;
	size_t num_of_clocks;
	size_t num_of_eps;
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	void (*make_thread)(const struct device *dev);
	int speed_idx;
};

struct udc_renesas_ra_data {
	struct k_thread thread_data;
	struct st_usbd_instance_ctrl udc;
	struct st_usbd_cfg udc_cfg;
};

enum udc_renesas_ra_event_type {
	/* An event generated by the HAL driver */
	UDC_RENESAS_RA_EVT_HAL,
	/* Shim driver event to trigger next transfer */
	UDC_RENESAS_RA_EVT_XFER,
	/* Let controller perform status stage */
	UDC_RENESAS_RA_EVT_STATUS,
};

struct udc_renesas_ra_evt {
	enum udc_renesas_ra_event_type type;
	usbd_event_t hal_evt;
	uint8_t ep;
};

K_MSGQ_DEFINE(drv_msgq, sizeof(struct udc_renesas_ra_evt), CONFIG_UDC_RENESAS_RA_MAX_QMESSAGES,
	      sizeof(uint32_t));

extern void usb_device_isr(void);

static void udc_renesas_ra_event_handler(usbd_callback_arg_t *p_args)
{
	const struct device *dev = p_args->p_context;
	struct udc_renesas_ra_evt evt;

	switch (p_args->event.event_id) {
	case USBD_EVENT_BUS_RESET:
		udc_submit_event(dev, UDC_EVT_RESET, 0);
		break;

	case USBD_EVENT_VBUS_RDY:
		udc_submit_event(dev, UDC_EVT_VBUS_READY, 0);
		break;

	case USBD_EVENT_VBUS_REMOVED:
		udc_submit_event(dev, UDC_EVT_VBUS_REMOVED, 0);
		break;

	case USBD_EVENT_SUSPEND:
		udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
		break;

	case USBD_EVENT_RESUME:
		udc_submit_event(dev, UDC_EVT_RESUME, 0);
		break;

	case USBD_EVENT_SOF:
		udc_submit_event(dev, UDC_EVT_SOF, 0);
		break;

	default:
		evt.type = UDC_RENESAS_RA_EVT_HAL;
		evt.hal_evt = p_args->event;
		k_msgq_put(&drv_msgq, &evt, K_NO_WAIT);
		break;
	}
}

static void udc_renesas_ra_interrupt_handler(void *arg)
{
	ARG_UNUSED(arg);
	usb_device_isr();
}

static void udc_event_xfer_next(const struct device *dev, const uint8_t ep)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);
	struct net_buf *buf;

	if (udc_ep_is_busy(dev, ep)) {
		return;
	}

	buf = udc_buf_peek(dev, ep);
	if (buf != NULL) {
		int err;

		if (USB_EP_DIR_IS_IN(ep)) {
			err = R_USBD_XferStart(&data->udc, ep, buf->data, buf->len);
		} else {
			err = R_USBD_XferStart(&data->udc, ep, buf->data, buf->size);
		}

		if (err != FSP_SUCCESS) {
			LOG_ERR("ep 0x%02x error", ep);
			udc_submit_ep_event(dev, buf, -ECONNREFUSED);
		} else {
			udc_ep_set_busy(dev, ep, true);
		}
	}
}

static int usbd_ctrl_feed_dout(const struct device *dev, const size_t length)
{
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT);
	struct net_buf *buf;
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	buf = udc_ctrl_alloc(dev, USB_CONTROL_EP_OUT, length);
	if (buf == NULL) {
		return -ENOMEM;
	}

	k_fifo_put(&cfg->fifo, buf);

	if (FSP_SUCCESS != R_USBD_XferStart(&data->udc, cfg->addr, buf->data, buf->size)) {
		return -EIO;
	}

	return 0;
}

static int udc_event_xfer_setup(const struct device *dev, struct udc_renesas_ra_evt *evt)
{
	struct net_buf *buf;
	int err;

	struct usb_setup_packet *setup_packet =
		(struct usb_setup_packet *)&evt->hal_evt.setup_received;

	buf = udc_ctrl_alloc(dev, USB_CONTROL_EP_OUT, sizeof(struct usb_setup_packet));
	if (buf == NULL) {
		LOG_ERR("Failed to allocate for setup");
		return -ENOMEM;
	}

	udc_ep_buf_set_setup(buf);
	net_buf_add_mem(buf, setup_packet, sizeof(struct usb_setup_packet));

	/* Update to next stage of control transfer */
	udc_ctrl_update_stage(dev, buf);

	if (udc_ctrl_stage_is_data_out(dev)) {
		/*  Allocate and feed buffer for data OUT stage */
		LOG_DBG("s:%p|feed for -out-", buf);
		err = usbd_ctrl_feed_dout(dev, udc_data_stage_length(buf));
		if (err == -ENOMEM) {
			err = udc_submit_ep_event(dev, buf, err);
		}
	} else if (udc_ctrl_stage_is_data_in(dev)) {
		err = udc_ctrl_submit_s_in_status(dev);
	} else {
		err = udc_ctrl_submit_s_status(dev);
	}

	return err;
}

static void udc_event_xfer_ctrl_in(const struct device *dev, struct net_buf *const buf)
{
	if (udc_ctrl_stage_is_status_in(dev) || udc_ctrl_stage_is_no_data(dev)) {
		/* Status stage finished, notify upper layer */
		udc_ctrl_submit_status(dev, buf);
	}

	/* Update to next stage of control transfer */
	udc_ctrl_update_stage(dev, buf);

	if (udc_ctrl_stage_is_status_out(dev)) {
		/* IN transfer finished, perform status stage OUT and release buffer */
		usbd_ctrl_feed_dout(dev, 0);
		net_buf_unref(buf);
	}
}

static void udc_event_status_in(const struct device *dev)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);
	struct net_buf *buf;

	buf = udc_buf_get(dev, USB_CONTROL_EP_IN);
	if (unlikely(buf == NULL)) {
		LOG_DBG("ep 0x%02x queue is empty", USB_CONTROL_EP_IN);
		return;
	}

	/* Perform status stage IN */
	R_USBD_XferStart(&data->udc, USB_CONTROL_EP_IN, NULL, 0);

	udc_event_xfer_ctrl_in(dev, buf);
}

static void udc_event_xfer_ctrl_out(const struct device *dev, struct net_buf *const buf,
				    uint32_t len)
{
	net_buf_add(buf, len);

	if (udc_ctrl_stage_is_status_out(dev)) {
		/* Status stage finished, notify upper layer */
		udc_ctrl_submit_status(dev, buf);
	}

	/* Update to next stage of control transfer */
	udc_ctrl_update_stage(dev, buf);

	if (udc_ctrl_stage_is_status_in(dev)) {
		udc_ctrl_submit_s_out_status(dev, buf);
	}
}

static void udc_event_xfer_complete(const struct device *dev, struct udc_renesas_ra_evt *evt)
{
	struct net_buf *buf;
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	uint8_t ep = evt->hal_evt.xfer_complete.ep_addr;
	usbd_xfer_result_t result = evt->hal_evt.xfer_complete.result;
	uint32_t len = evt->hal_evt.xfer_complete.len;

	udc_ep_set_busy(dev, ep, false);

	buf = udc_buf_peek(dev, ep);
	if (buf == NULL) {
		return;
	}

	if (result != USBD_XFER_RESULT_SUCCESS) {
		goto error;
	}

	if (USB_EP_DIR_IS_IN(ep) && udc_ep_buf_has_zlp(buf)) {
		/* Send ZLP, notification about transfer complete should come again */
		udc_ep_buf_clear_zlp(buf);
		if (FSP_SUCCESS != R_USBD_XferStart(&data->udc, ep, NULL, 0)) {
			goto error;
		}

		return;
	}

	buf = udc_buf_get(dev, ep);

	if (ep == USB_CONTROL_EP_IN) {
		udc_event_xfer_ctrl_in(dev, buf);
	} else if (ep == USB_CONTROL_EP_OUT) {
		udc_event_xfer_ctrl_out(dev, buf, len);
	} else {
		if (USB_EP_DIR_IS_OUT(ep)) {
			net_buf_add(buf, len);
		}
		udc_submit_ep_event(dev, buf, 0);
	}

	return;
error:
	udc_submit_ep_event(dev, buf, -EIO);
}

static ALWAYS_INLINE void renesas_ra_thread_handler(void *const arg)
{
	const struct device *dev = (const struct device *)arg;

	LOG_DBG("Driver %p thread started", dev);
	while (true) {
		struct udc_renesas_ra_evt evt;

		k_msgq_get(&drv_msgq, &evt, K_FOREVER);
		switch (evt.type) {
		case UDC_RENESAS_RA_EVT_HAL: {
			switch (evt.hal_evt.event_id) {
			case USBD_EVENT_SETUP_RECEIVED:
				udc_event_xfer_setup(dev, &evt);
				break;

			case USBD_EVENT_XFER_COMPLETE:
				udc_event_xfer_complete(dev, &evt);
				break;

			default:
				break;
			}
			break;
		}

		case UDC_RENESAS_RA_EVT_XFER:
			udc_event_xfer_next(dev, evt.ep);
			break;

		case UDC_RENESAS_RA_EVT_STATUS:
			udc_event_status_in(dev);
			break;

		default:
			break;
		}
	}
}

static int udc_renesas_ra_ep_enqueue(const struct device *dev, struct udc_ep_config *const cfg,
				     struct net_buf *buf)
{
	struct udc_renesas_ra_evt evt = {};

	LOG_DBG("%p enqueue %p", dev, buf);

	udc_buf_put(cfg, buf);

	evt.ep = cfg->addr;
	if (cfg->addr == USB_CONTROL_EP_IN && buf->len == 0) {
		evt.type = UDC_RENESAS_RA_EVT_STATUS;
	} else {
		evt.type = UDC_RENESAS_RA_EVT_XFER;
	}

	k_msgq_put(&drv_msgq, &evt, K_NO_WAIT);

	if (cfg->stat.halted) {
		LOG_DBG("ep 0x%02x halted", cfg->addr);
	}

	return 0;
}

static int udc_renesas_ra_ep_dequeue(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);
	unsigned int lock_key;
	struct net_buf *buf;

	lock_key = irq_lock();

	buf = udc_buf_get_all(dev, cfg->addr);
	if (buf != NULL) {
		udc_submit_ep_event(dev, buf, -ECONNABORTED);
	}

	if (FSP_SUCCESS != R_USBD_XferAbort(&data->udc, cfg->addr)) {
		return -EIO;
	}

	udc_ep_set_busy(dev, cfg->addr, false);

	irq_unlock(lock_key);

	return 0;
}

static int udc_renesas_ra_ep_enable(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);
	usbd_desc_endpoint_t ep_desc;

	if (USB_EP_GET_IDX(cfg->addr) == 0) {
		return 0;
	}

	ep_desc.bLength = sizeof(struct usb_ep_descriptor);
	ep_desc.bDescriptorType = USB_DESC_ENDPOINT;
	ep_desc.bEndpointAddress = cfg->addr;
	ep_desc.bmAttributes = cfg->attributes;
	ep_desc.wMaxPacketSize = cfg->mps;
	ep_desc.bInterval = cfg->interval;

	if (FSP_SUCCESS != R_USBD_EdptOpen(&data->udc, &ep_desc)) {
		return -EIO;
	}

	LOG_DBG("Enable ep 0x%02x", cfg->addr);

	return 0;
}

static int udc_renesas_ra_ep_disable(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	if (USB_EP_GET_IDX(cfg->addr) == 0) {
		return 0;
	}

	if (FSP_SUCCESS != R_USBD_EdptClose(&data->udc, cfg->addr)) {
		return -EIO;
	}

	LOG_DBG("Disable ep 0x%02x", cfg->addr);

	return 0;
}

static int udc_renesas_ra_ep_set_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	LOG_DBG("Set halt ep 0x%02x", cfg->addr);

	if (FSP_SUCCESS != R_USBD_EdptStall(&data->udc, cfg->addr)) {
		return -EIO;
	}

	cfg->stat.halted = true;

	return 0;
}

static int udc_renesas_ra_ep_clear_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	LOG_DBG("Clear halt ep 0x%02x", cfg->addr);

	if (FSP_SUCCESS != R_USBD_EdptClearStall(&data->udc, cfg->addr)) {
		return -EIO;
	}

	cfg->stat.halted = false;

	return 0;
}

static int udc_renesas_ra_set_address(const struct device *dev, const uint8_t addr)
{
	/* The USB controller will automatically perform a response to the SET_ADRRESS request. */
	LOG_DBG("Set new address %u for %p", addr, dev);

	return 0;
}

static int udc_renesas_ra_host_wakeup(const struct device *dev)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	if (FSP_SUCCESS != R_USBD_RemoteWakeup(&data->udc)) {
		return -EIO;
	}

	LOG_DBG("Remote wakeup from %p", dev);

	return 0;
}

static enum udc_bus_speed udc_renesas_ra_device_speed(const struct device *dev)
{
	struct udc_data *data = dev->data;

	return data->caps.hs ? UDC_BUS_SPEED_HS : UDC_BUS_SPEED_FS;
}

static int udc_renesas_ra_enable(const struct device *dev)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	if (FSP_SUCCESS != R_USBD_Connect(&data->udc)) {
		return -EIO;
	}

	LOG_DBG("Enable device %p", dev);

	return 0;
}

static int udc_renesas_ra_disable(const struct device *dev)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	if (FSP_SUCCESS != R_USBD_Disconnect(&data->udc)) {
		return -EIO;
	}

	LOG_DBG("Disable device %p", dev);

	return 0;
}

static int udc_renesas_ra_init(const struct device *dev)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	if (FSP_SUCCESS != R_USBD_Open(&data->udc, &data->udc_cfg)) {
		return -EIO;
	}

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT, USB_EP_TYPE_CONTROL, 64, 0)) {
		LOG_ERR("Failed to enable control endpoint");
		return -EIO;
	}

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_IN, USB_EP_TYPE_CONTROL, 64, 0)) {
		LOG_ERR("Failed to enable control endpoint");
		return -EIO;
	}

#if DT_HAS_COMPAT_STATUS_OKAY(renesas_ra_usbhs)
	if (data->udc_cfg.hs_irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_enable(data->udc_cfg.hs_irq);
	}
#endif

	if (data->udc_cfg.irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_enable(data->udc_cfg.irq);
	}

	if (data->udc_cfg.irq_r != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_enable(data->udc_cfg.irq_r);
	}

	return 0;
}

static int udc_renesas_ra_shutdown(const struct device *dev)
{
	struct udc_renesas_ra_data *data = udc_get_private(dev);

	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT)) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_IN)) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	if (FSP_SUCCESS != R_USBD_Close(&data->udc)) {
		return -EIO;
	}

	return 0;
}

static int udc_renesas_ra_clock_check(const struct device *dev)
{
	const struct udc_renesas_ra_config *config = dev->config;

#if USBHS_PHY_CLOCK_SOURCE_IS_XTAL
	if (config->speed_idx == UDC_BUS_SPEED_HS) {
		if (BSP_CFG_XTAL_HZ == 0) {
			LOG_ERR("XTAL clock should be provided");
			return -EINVAL;
		}

		return 0;
	}
#endif

	for (size_t i = 0; i < config->num_of_clocks; i++) {
		const struct device *clock_dev = *(config->clocks + i);
		const struct clock_control_ra_pclk_cfg *clock_cfg = clock_dev->config;
		uint32_t clk_src_rate;
		uint32_t clock_rate;

		if (!device_is_ready(clock_dev)) {
			LOG_ERR("%s is not ready", clock_dev->name);
			return -ENODEV;
		}

		clk_src_rate = R_BSP_SourceClockHzGet(clock_cfg->clk_src);
		clock_rate = clk_src_rate / clock_cfg->clk_div;

		if (strcmp(clock_dev->name, "uclk") == 0 && clock_rate != MHZ(48)) {
			LOG_ERR("Setting for uclk should be 48Mhz");
			return -ENOTSUP;
		}

#if DT_HAS_COMPAT_STATUS_OKAY(renesas_ra_usbhs)
		if (strcmp(clock_dev->name, "u60clk") == 0 && clock_rate != MHZ(60)) {
			LOG_ERR("Setting for u60clk should be 60Mhz");
			return -ENOTSUP;
		}
#endif
	}

	return 0;
}

static int udc_renesas_ra_driver_preinit(const struct device *dev)
{
	const struct udc_renesas_ra_config *config = dev->config;
	struct udc_renesas_ra_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;
	uint16_t mps = 1023;
	int err;

#if !USBHS_PHY_CLOCK_SOURCE_IS_XTAL
	if (priv->udc_cfg.usb_speed == USBD_SPEED_HS) {
		LOG_ERR("High-speed operation is not supported in case PHY clock source is not "
			"XTAL");
		return -ENOTSUP;
	}
#endif

	if (config->speed_idx == UDC_BUS_SPEED_HS) {
		if (!(priv->udc_cfg.usb_speed == USBD_SPEED_HS ||
		      priv->udc_cfg.usb_speed == USBD_SPEED_FS)) {
			LOG_ERR("USBHS module only support high-speed and full-speed device");
			return -ENOTSUP;
		}
	} else {
		/* config->speed_idx == UDC_BUS_SPEED_FS */
		if (priv->udc_cfg.usb_speed != USBD_SPEED_FS) {
			LOG_ERR("USBFS module only support full-speed device");
			return -ENOTSUP;
		}
	}

	err = udc_renesas_ra_clock_check(dev);
	if (err < 0) {
		return err;
	}

	err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		return err;
	}

	k_mutex_init(&data->mutex);

	data->caps.rwup = true;
	data->caps.mps0 = UDC_MPS0_64;
	if (priv->udc_cfg.usb_speed == USBD_SPEED_HS) {
		data->caps.hs = true;
		mps = 1024;
	}

	for (int i = 0; i < config->num_of_eps; i++) {
		config->ep_cfg_out[i].caps.out = 1;
		if (i == 0) {
			config->ep_cfg_out[i].caps.control = 1;
			config->ep_cfg_out[i].caps.mps = 64;
		} else {
			config->ep_cfg_out[i].caps.bulk = 1;
			config->ep_cfg_out[i].caps.interrupt = 1;
			config->ep_cfg_out[i].caps.iso = 1;
			config->ep_cfg_out[i].caps.mps = mps;
		}

		config->ep_cfg_out[i].addr = USB_EP_DIR_OUT | i;
		err = udc_register_ep(dev, &config->ep_cfg_out[i]);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

	for (int i = 0; i < config->num_of_eps; i++) {
		config->ep_cfg_in[i].caps.in = 1;
		if (i == 0) {
			config->ep_cfg_in[i].caps.control = 1;
			config->ep_cfg_in[i].caps.mps = 64;
		} else {
			config->ep_cfg_in[i].caps.bulk = 1;
			config->ep_cfg_in[i].caps.interrupt = 1;
			config->ep_cfg_in[i].caps.iso = 1;
			config->ep_cfg_in[i].caps.mps = mps;
		}

		config->ep_cfg_in[i].addr = USB_EP_DIR_IN | i;
		err = udc_register_ep(dev, &config->ep_cfg_in[i]);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

#if DT_HAS_COMPAT_STATUS_OKAY(renesas_ra_usbhs)
	if (priv->udc_cfg.hs_irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		R_ICU->IELSR[priv->udc_cfg.hs_irq] = ELC_EVENT_USBHS_USB_INT_RESUME;
	}
#endif

	if (priv->udc_cfg.irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		R_ICU->IELSR[priv->udc_cfg.irq] = ELC_EVENT_USBFS_INT;
	}

	if (priv->udc_cfg.irq_r != (IRQn_Type)BSP_IRQ_DISABLED) {
		R_ICU->IELSR[priv->udc_cfg.irq_r] = ELC_EVENT_USBFS_RESUME;
	}

	config->make_thread(dev);
	LOG_INF("Device %p (max. speed %d)", dev, priv->udc_cfg.usb_speed);

	return 0;
}

static void udc_renesas_ra_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}

static void udc_renesas_ra_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

static const struct udc_api udc_renesas_ra_api = {
	.lock = udc_renesas_ra_lock,
	.unlock = udc_renesas_ra_unlock,
	.device_speed = udc_renesas_ra_device_speed,
	.init = udc_renesas_ra_init,
	.enable = udc_renesas_ra_enable,
	.disable = udc_renesas_ra_disable,
	.shutdown = udc_renesas_ra_shutdown,
	.set_address = udc_renesas_ra_set_address,
	.host_wakeup = udc_renesas_ra_host_wakeup,
	.ep_enable = udc_renesas_ra_ep_enable,
	.ep_disable = udc_renesas_ra_ep_disable,
	.ep_set_halt = udc_renesas_ra_ep_set_halt,
	.ep_clear_halt = udc_renesas_ra_ep_clear_halt,
	.ep_enqueue = udc_renesas_ra_ep_enqueue,
	.ep_dequeue = udc_renesas_ra_ep_dequeue,
};

#define DT_DRV_COMPAT renesas_ra_udc

#define USB_RENESAS_RA_MODULE_NUMBER(id) (DT_REG_ADDR(id) == R_USB_FS0_BASE ? 0 : 1)

#define USB_RENESAS_RA_IRQ_GET(id, name, cell)                                                     \
	COND_CODE_1(DT_IRQ_HAS_NAME(id, name), (DT_IRQ_BY_NAME(id, name, cell)),                   \
		    ((IRQn_Type) BSP_IRQ_DISABLED))

#define USB_RENESAS_RA_MAX_SPEED_IDX(id)                                                           \
	(DT_NODE_HAS_COMPAT(id, renesas_ra_usbhs) ? UDC_BUS_SPEED_HS : UDC_BUS_SPEED_FS)

#define USB_RENESAS_RA_SPEED_IDX(id)                                                               \
	(DT_NODE_HAS_COMPAT(id, renesas_ra_usbhs)                                                  \
		 ? DT_ENUM_IDX_OR(id, maximum_speed, UDC_BUS_SPEED_HS)                             \
		 : DT_ENUM_IDX_OR(id, maximum_speed, UDC_BUS_SPEED_FS))

#define USB_RENESAS_RA_IRQ_CONNECT(idx, n)                                                         \
	IRQ_CONNECT(DT_IRQ_BY_IDX(DT_INST_PARENT(n), idx, irq),                                    \
		    DT_IRQ_BY_IDX(DT_INST_PARENT(n), idx, priority),                               \
		    udc_renesas_ra_interrupt_handler, DEVICE_DT_INST_GET(n), 0)

#define USB_RENESAS_RA_CLOCKS_GET(idx, id)                                                         \
	DEVICE_DT_GET_OR_NULL(DT_PHANDLE_BY_IDX(id, phys_clock, idx))

#define UDC_RENESAS_RA_DEVICE_DEFINE(n)                                                            \
	PINCTRL_DT_DEFINE(DT_INST_PARENT(n));                                                      \
	K_THREAD_STACK_DEFINE(udc_renesas_ra_stack_##n, CONFIG_UDC_RENESAS_RA_STACK_SIZE);         \
                                                                                                   \
	static const struct device *udc_renesas_ra_clock_dev_##n[] = {                             \
		LISTIFY(DT_PROP_LEN_OR(DT_INST_PARENT(n), phys_clock, 0),                          \
			USB_RENESAS_RA_CLOCKS_GET, (,), DT_INST_PARENT(n))                         \
	};                                                                                         \
                                                                                                   \
	static void udc_renesas_ra_thread_##n(void *dev, void *arg1, void *arg2)                   \
	{                                                                                          \
		renesas_ra_thread_handler(dev);                                                    \
	}                                                                                          \
                                                                                                   \
	static void udc_renesas_ra_make_thread_##n(const struct device *dev)                       \
	{                                                                                          \
		struct udc_renesas_ra_data *priv = udc_get_private(dev);                           \
                                                                                                   \
		k_thread_create(&priv->thread_data, udc_renesas_ra_stack_##n,                      \
				K_THREAD_STACK_SIZEOF(udc_renesas_ra_stack_##n),                   \
				udc_renesas_ra_thread_##n, (void *)dev, NULL, NULL,                \
				K_PRIO_COOP(CONFIG_UDC_RENESAS_RA_THREAD_PRIORITY), K_ESSENTIAL,   \
				K_NO_WAIT);                                                        \
		k_thread_name_set(&priv->thread_data, dev->name);                                  \
	}                                                                                          \
                                                                                                   \
	static struct udc_ep_config ep_cfg_in##n[DT_PROP(DT_INST_PARENT(n), num_bidir_endpoints)]; \
	static struct udc_ep_config                                                                \
		ep_cfg_out##n[DT_PROP(DT_INST_PARENT(n), num_bidir_endpoints)];                    \
                                                                                                   \
	static const struct udc_renesas_ra_config udc_renesas_ra_config_##n = {                    \
		.pcfg = PINCTRL_DT_DEV_CONFIG_GET(DT_INST_PARENT(n)),                              \
		.clocks = udc_renesas_ra_clock_dev_##n,                                            \
		.num_of_clocks = DT_PROP_LEN_OR(DT_INST_PARENT(n), phys_clock, 0),                 \
		.num_of_eps = DT_PROP(DT_INST_PARENT(n), num_bidir_endpoints),                     \
		.ep_cfg_in = ep_cfg_in##n,                                                         \
		.ep_cfg_out = ep_cfg_out##n,                                                       \
		.make_thread = udc_renesas_ra_make_thread_##n,                                     \
		.speed_idx = USB_RENESAS_RA_MAX_SPEED_IDX(DT_INST_PARENT(n)),                      \
	};                                                                                         \
                                                                                                   \
	static struct udc_renesas_ra_data udc_priv_##n = {                                         \
		.udc_cfg = {                                                                       \
			.module_number = USB_RENESAS_RA_MODULE_NUMBER(DT_INST_PARENT(n)),          \
			.usb_speed = USB_RENESAS_RA_SPEED_IDX(DT_INST_PARENT(n)),                  \
			.irq = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_i, irq),            \
			.irq_r = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_r, irq),          \
			.hs_irq = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbhs_ir, irq),        \
			.ipl = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_i, priority),       \
			.ipl_r = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_r, priority),     \
			.hsipl = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbhs_ir, priority),    \
			.p_context = DEVICE_DT_INST_GET(n),                                        \
			.p_callback = udc_renesas_ra_event_handler,                                \
		},                                                                                 \
	};                                                                                         \
                                                                                                   \
	static struct udc_data udc_data_##n = {                                                    \
		.mutex = Z_MUTEX_INITIALIZER(udc_data_##n.mutex),                                  \
		.priv = &udc_priv_##n,                                                             \
	};                                                                                         \
                                                                                                   \
	int udc_renesas_ra_driver_preinit##n(const struct device *dev)                             \
	{                                                                                          \
		LISTIFY(DT_NUM_IRQS(DT_INST_PARENT(n)), USB_RENESAS_RA_IRQ_CONNECT, (;), n);       \
		return udc_renesas_ra_driver_preinit(dev);                                         \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, udc_renesas_ra_driver_preinit##n, NULL, &udc_data_##n,            \
			      &udc_renesas_ra_config_##n, POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &udc_renesas_ra_api);

DT_INST_FOREACH_STATUS_OKAY(UDC_RENESAS_RA_DEVICE_DEFINE)
