/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_lpc84x_gpio

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/dt-bindings/clock/lpc84x-clock.h>
#include <zephyr/logging/log.h>

#include <fsl_gpio.h>
#include <fsl_iocon.h>
LOG_MODULE_REGISTER(gpio_lpc84x, CONFIG_GPIO_LOG_LEVEL);

#include <soc.h>

struct gpio_lpc84x_shared {
	SYSCON_Type *syscon;
	PINT_Type *pint;
	uint8_t nirqs;
	uint8_t pint_used_mask;
};

struct gpio_lpc84x_config {
	struct gpio_driver_config common;
	GPIO_Type *base;
	uint8_t port_no;
	uint8_t ngpios;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pcfg;
	struct gpio_lpc84x_shared *shared;
};

struct gpio_lpc84x_data {
	struct gpio_driver_data common;
	sys_slist_t callbacks;
};

static int gpio_lpc84x_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_lpc84x_config *config = dev->config;
	struct gpio_lpc84x_data *data = dev->data;
	uint32_t pinconfig = 0;

	if (pin > 31) {
		pin = (pin & 0x1f);
	}

	if (pin >= config->ngpios) {
		return -EINVAL;
	}

	/* PIO0_10 & PIO0_11 default pins of I2C for open-drain operation only*/
	if (config->port_no == 0 && (pin == 10 || pin == 11) &&
	    ((flags & GPIO_OPEN_DRAIN) == 0 || (flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)))) {
		return -EINVAL;
	}

	if (flags & GPIO_PULL_UP) {
		pinconfig |= IOCON_PIO_MODE(0x2);
	} else if (flags & GPIO_PULL_DOWN) {
		pinconfig |= IOCON_PIO_MODE(0x1);
	} else {
		pinconfig |= IOCON_PIO_MODE(0x0);
	}

	if (flags & GPIO_OPEN_DRAIN) {
		pinconfig |= IOCON_PIO_OD(0x1);
	}

	if (flags & GPIO_OUTPUT_INIT_LOW) {
		GPIO_PinWrite(config->base, config->port_no, pin, 0);
	} else if (flags & GPIO_OUTPUT_INIT_HIGH) {
		GPIO_PinWrite(config->base, config->port_no, pin, 1);
	}

	if (flags & GPIO_OUTPUT) {
		config->base->DIRSET[config->port_no] = BIT(pin);
	} else {
		/* GPIO_INPUT and GPIO_DISCONNECTED (= 0): both become high-Z input. */
		config->base->DIRCLR[config->port_no] = BIT(pin);
	}

	uint8_t idx = lpc84x_iocon_index(config->port_no, pin);
	pinctrl_soc_pin_t pin_cfg = {.swm_cfg = LPC84X_SWM_NONE,
				     .iocon_cfg = idx | ((pinconfig >> 3) << 8)};

	pinctrl_configure_pins(&pin_cfg, 1, 0);

	if (flags & GPIO_ACTIVE_LOW) {
		data->common.invert |= (gpio_port_pins_t)BIT(pin);
	} else {
		data->common.invert &= ~(gpio_port_pins_t)BIT(pin);
	}

	return 0;
}

static int gpio_lpc84x_port_set_masked_raw(const struct device *dev, uint32_t mask, uint32_t value)
{
	const struct gpio_lpc84x_config *config = dev->config;

	GPIO_PortSet(config->base, config->port_no, (mask & value));

	GPIO_PortClear(config->base, config->port_no, (mask & ~value));

	return 0;
}

static int gpio_lpc84x_port_clear_bits_raw(const struct device *dev, uint32_t mask)
{
	const struct gpio_lpc84x_config *config = dev->config;

	GPIO_PortClear(config->base, config->port_no, mask);

	return 0;
}

static int gpio_lpc84x_port_get_raw(const struct device *dev, uint32_t *value)
{
	const struct gpio_lpc84x_config *config = dev->config;

	*value = GPIO_PortRead(config->base, config->port_no);

	return 0;
}

static int gpio_lpc84x_port_set_bits_raw(const struct device *dev, uint32_t mask)
{
	const struct gpio_lpc84x_config *config = dev->config;

	GPIO_PortSet(config->base, config->port_no, mask);

	return 0;
}

static int gpio_lpc84x_port_toggle_bits(const struct device *dev, uint32_t mask)
{
	const struct gpio_lpc84x_config *config = dev->config;

	GPIO_PortToggle(config->base, config->port_no, mask);

	return 0;
}

static int pin_attach(struct gpio_lpc84x_shared *shared, uint8_t intpin)
{
	uint8_t irq;

	for (irq = 0; irq < shared->nirqs; irq++) {
		if ((shared->syscon->PINTSEL[irq] & BIT_MASK(6)) == intpin &&
		    (shared->pint_used_mask & BIT(irq))) {
			return irq;
		}
	}

	for (irq = 0; irq < shared->nirqs; irq++) {
		if ((shared->pint_used_mask & BIT(irq)) == 0) {
			shared->syscon->PINTSEL[irq] = intpin;
			shared->pint_used_mask |= BIT(irq);
			return irq;
		}
	}

	return -EBUSY;
}

static int pin_detach(struct gpio_lpc84x_shared *shared, uint8_t intpin)
{
	uint8_t irq;

	for (irq = 0; irq < shared->nirqs; irq++) {
		if ((shared->syscon->PINTSEL[irq] & BIT_MASK(6)) == intpin &&
		    shared->pint_used_mask & BIT(irq)) {
			shared->pint_used_mask &= ~BIT(irq);
			return irq;
		}
	}

	return -EINVAL;
}

static int gpio_lpc84x_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					       enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	const struct gpio_lpc84x_config *config = dev->config;
	struct gpio_lpc84x_shared *shared = config->shared;
	uint8_t intpin = pin + (config->port_no * 32);
	uint32_t irq = 0;

	if (pin >= config->ngpios) {
		return -EINVAL;
	}

	if (mode == GPIO_INT_MODE_DISABLED) {
		irq = pin_detach(shared, intpin);
		if (irq >= 0) {
			shared->pint->CIENR = BIT(irq);
			shared->pint->CIENF = BIT(irq);
			shared->pint->ISEL &= ~BIT(irq);
			shared->pint->IST = BIT(irq);
		}
		return 0;
	}

	irq = pin_attach(shared, intpin);
	if (irq < 0) {
		return irq;
	}

	/* Disable and clear any stale latch before reconfiguring. */
	shared->pint->CIENR = BIT(irq);
	shared->pint->CIENF = BIT(irq);
	shared->pint->ISEL &= ~BIT(irq);
	shared->pint->IST = BIT(irq);

	switch (mode) {
	case GPIO_INT_MODE_EDGE:
		/* Edge interrupt mode. */
		shared->pint->ISEL &= ~BIT(irq);
		if (trig & GPIO_INT_TRIG_LOW) {
			shared->pint->SIENF = BIT(irq);
		} else {
			shared->pint->CIENF = BIT(irq);
		}

		if (trig & GPIO_INT_TRIG_HIGH) {
			shared->pint->SIENR = BIT(irq);
		} else {
			shared->pint->CIENR = BIT(irq);
		}
		break;
	case GPIO_INT_MODE_LEVEL:
		/* Level interrupt mode. */
		shared->pint->ISEL |= BIT(irq);
		/* Level-sensitive interrupts, IENR enables the interrupt. */
		shared->pint->SIENR = BIT(irq);
		if (trig & GPIO_INT_TRIG_LOW) {
			/* Active LOW interrupt selected. */
			shared->pint->SIENF = BIT(irq);
		} else {
			/* Active HIGH interrupt selected. */
			shared->pint->CIENF = BIT(irq);
		}
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int gpio_lpc84x_manage_callback(const struct device *dev, struct gpio_callback *call_back,
				       bool set)
{
	struct gpio_lpc84x_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, call_back, set);
}

static void gpio_lpc84x_isr(const void *arg)
{
	struct gpio_lpc84x_shared *shared = (struct gpio_lpc84x_shared *)arg;
	uint32_t active_pins[2] = {0, 0};

	for (int irq = 0; irq < shared->nirqs; irq++) {
		if (shared->pint->IST & BIT(irq)) {
			if (!(shared->pint->ISEL & BIT(irq))) {
				shared->pint->IST = BIT(irq);
			}

			uint8_t intpin = shared->syscon->PINTSEL[irq] & BIT_MASK(6);

			if (intpin < 32) {
				active_pins[0] |= BIT(intpin);
			} else if (intpin < 54) {
				active_pins[1] |= BIT(intpin - 32);
			}
		}
	}
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpio0))
	if (active_pins[0]) {
		const struct device *port = DEVICE_DT_GET(DT_NODELABEL(gpio0));
		struct gpio_lpc84x_data *data = port->data;

		gpio_fire_callbacks(&data->callbacks, port, active_pins[0]);
	}
#endif

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpio1))
	if (active_pins[1]) {
		const struct device *port = DEVICE_DT_GET(DT_NODELABEL(gpio1));
		struct gpio_lpc84x_data *data = port->data;

		gpio_fire_callbacks(&data->callbacks, port, active_pins[1]);
	}
#endif
}

static struct gpio_lpc84x_shared gpio_lpc84x_shared = {
	.syscon = (SYSCON_Type *)DT_REG_ADDR(DT_NODELABEL(syscon)),
	.pint = (PINT_Type *)DT_REG_ADDR(DT_NODELABEL(pint)),
	.nirqs = DT_NUM_IRQS(DT_NODELABEL(pint)),
	.pint_used_mask = 0,
};

#define PINT_NODE DT_NODELABEL(pint)

#define PINT_IRQ_CONNECT(n)                                                                        \
	do {                                                                                       \
		IRQ_CONNECT(DT_IRQ_BY_IDX(PINT_NODE, n, irq),                                      \
			    DT_IRQ_BY_IDX(PINT_NODE, n, priority), gpio_lpc84x_isr,                \
			    &gpio_lpc84x_shared, 0);                                               \
		irq_enable(DT_IRQ_BY_IDX(PINT_NODE, n, irq));                                      \
	} while (false)

static int gpio_lpc84x_init(const struct device *dev)
{
	const struct gpio_lpc84x_config *config = dev->config;
	int err;
	static bool initialized;

	initialized = false;

	if (!device_is_ready(config->clock_dev)) {
		LOG_ERR("Clock device not ready");
		return -ENODEV;
	}

	err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (err && err != -ENOENT) {
		LOG_ERR("Failed to apply pinctrl state (err %d)", err);
		return err;
	}

	err = clock_control_on(config->clock_dev, config->clock_subsys);
	if (err < 0) {
		LOG_ERR("Failed to enable clock (err %d)", err);
		return err;
	}

	if (!initialized) {
		err = clock_control_on(config->clock_dev,
				       (clock_control_subsys_t)LPC84X_CLK_GPIOINT);
		if (err < 0) {
			LOG_ERR("failed to enable the gpio interrupt clock (err %d)", err);
		}

		PINT_IRQ_CONNECT(0);
		PINT_IRQ_CONNECT(1);
		PINT_IRQ_CONNECT(2);
		PINT_IRQ_CONNECT(3);
		PINT_IRQ_CONNECT(4);
		PINT_IRQ_CONNECT(5);
		PINT_IRQ_CONNECT(6);
		PINT_IRQ_CONNECT(7);

		initialized = true;
	}

	GPIO_PortInit(config->base, config->port_no);

	return 0;
}

static const struct gpio_driver_api gpio_lpc84x_driver_api = {
	.pin_configure = gpio_lpc84x_pin_configure,
	.port_get_raw = gpio_lpc84x_port_get_raw,
	.port_set_masked_raw = gpio_lpc84x_port_set_masked_raw,
	.port_set_bits_raw = gpio_lpc84x_port_set_bits_raw,
	.port_clear_bits_raw = gpio_lpc84x_port_clear_bits_raw,
	.port_toggle_bits = gpio_lpc84x_port_toggle_bits,
	.pin_interrupt_configure = gpio_lpc84x_pin_interrupt_configure,
	.manage_callback = gpio_lpc84x_manage_callback,
};

#define GPIO_LPC84X_INIT(id)                                                                       \
	PINCTRL_DT_INST_DEFINE(id);                                                                \
                                                                                                   \
	static const struct gpio_lpc84x_config gpio_lpc84x_config_##id = {                         \
		.common =                                                                          \
			{                                                                          \
				.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(id),              \
			},                                                                         \
		.base = (GPIO_Type *)DT_REG_ADDR(DT_INST_PARENT(id)),                              \
		.port_no = DT_INST_PROP(id, port_no),                                              \
		.ngpios = DT_INST_PROP(id, ngpios),                                                \
		.clock_dev = DEVICE_DT_GET(DT_INST_PHANDLE(id, clocks)),                           \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(id, name),             \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(id),                                        \
		.shared = &gpio_lpc84x_shared,                                                     \
	};                                                                                         \
                                                                                                   \
	static struct gpio_lpc84x_data gpio_lpc84x_data_##id;                                      \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(id, gpio_lpc84x_init, NULL, &gpio_lpc84x_data_##id,                  \
			      &gpio_lpc84x_config_##id, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,   \
			      &gpio_lpc84x_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_LPC84X_INIT)
