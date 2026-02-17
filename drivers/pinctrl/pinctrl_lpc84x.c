/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_lpc84x_swm

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>

#include <fsl_iocon.h>
#include <fsl_swm.h>

LOG_MODULE_REGISTER(pinctrl_lpc84x, CONFIG_PINCTRL_LOG_LEVEL);

/* Switch Matrix (SWM) Register Base */
#define PINCTRL_SWM_BASE ((SWM_Type *)DT_REG_ADDR(DT_NODELABEL(swm)))

/* I/O Configuration (IOCON) Register Base */
#define PINCTRL_IOCON_BASE ((IOCON_Type *)DT_REG_ADDR(DT_NODELABEL(iocon)))

/* Bit limit for PINENABLE0 register */
#define SWM_FIXED_PINENABLE0_LIMIT 32

/* Bit flag used in SDK's to denote PINENABLE1 register */
#define SWM_FIXED_REG_SEL_BIT 31

struct lpc84x_pinctrl_config {
	const struct device *clock_dev;
	clock_control_subsys_t swm_clk;
	clock_control_subsys_t iocon_clk;
};

static void lpc_pinctrl_apply_swm(pinctrl_soc_pin_t pin)
{
	if (pin.swm_cfg == LPC84X_SWM_NONE) {
		return;
	}

	uint8_t swm_pin = LPC84X_SWM_PIN(pin.swm_cfg);
	uint8_t func_id = LPC84X_SWM_FUNC(pin.swm_cfg);

	if (LPC84X_SWM_IS_FIXED(pin.swm_cfg)) {
		uint32_t mask;
		/*
		 * Fixed-function mapping.
		 * func_id represents the bit position in PINENABLE0 or PINENABLE1.
		 * Pins in PINENABLE0 are 0-31, PINENABLE1 are 32+.
		 * SDK expects BIT 31 to be set for PINENABLE1.
		 */
		if (func_id < SWM_FIXED_PINENABLE0_LIMIT) {
			mask = BIT(func_id);
		} else {
			mask = BIT(func_id - SWM_FIXED_PINENABLE0_LIMIT) |
			       BIT(SWM_FIXED_REG_SEL_BIT);
		}
		SWM_SetFixedPinSelect(PINCTRL_SWM_BASE, (swm_select_fixed_pin_t)mask, true);
	} else {
		/* Movable function mapping */
		SWM_SetMovablePinSelect(PINCTRL_SWM_BASE, (swm_select_movable_t)func_id,
					(swm_port_pin_type_t)swm_pin);
	}
}

static void lpc_pinctrl_apply_iocon(pinctrl_soc_pin_t pin)
{
	uint8_t cfg = LPC84X_IOCON_CFG(pin.iocon_cfg);
	uint8_t iocon_idx = LPC84X_IOCON_INDEX(pin.iocon_cfg);

	IOCON_PinMuxSet(PINCTRL_IOCON_BASE, iocon_idx, (uint32_t)cfg << 3);
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0; i < pin_cnt; i++) {
		lpc_pinctrl_apply_swm(pins[i]);
		lpc_pinctrl_apply_iocon(pins[i]);
	}

	return 0;
}

static int lpc84x_pinctrl_init(const struct device *dev)
{
	const struct lpc84x_pinctrl_config *config = dev->config;
	int err;

	if (!device_is_ready(config->clock_dev)) {
		LOG_ERR("clock control device not ready");
		return -ENODEV;
	}

	err = clock_control_on(config->clock_dev, config->swm_clk);
	if (err) {
		LOG_ERR("failed to enable SWM clock (err %d)", err);
		return err;
	}

	err = clock_control_on(config->clock_dev, config->iocon_clk);
	if (err) {
		LOG_ERR("failed to enable IOCON clock (err %d)", err);
		return err;
	}

	return 0;
}

#define LPC84X_PINCTRL_INIT(n)                                                                     \
	static const struct lpc84x_pinctrl_config lpc84x_pinctrl_##n##_config = {                  \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.swm_clk = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),                   \
		.iocon_clk = (clock_control_subsys_t)DT_CLOCKS_CELL(DT_NODELABEL(iocon), name),    \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, &lpc84x_pinctrl_init, NULL, NULL, &lpc84x_pinctrl_##n##_config,   \
			      PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

DT_INST_FOREACH_STATUS_OKAY(LPC84X_PINCTRL_INIT)
