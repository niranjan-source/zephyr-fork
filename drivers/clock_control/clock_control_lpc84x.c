/* Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_lpc84x_clock

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/dt-bindings/clock/lpc84x-clock.h>

#include <fsl_clock.h>
#include <fsl_power.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(clock_control_lpc84x, CONFIG_CLOCK_CONTROL_LOG_LEVEL);

static const clock_fro_osc_freq_t lpc84x_fro_freq_map[] = {
	kCLOCK_FroOscOut18M,
	kCLOCK_FroOscOut24M,
	kCLOCK_FroOscOut30M,
};

static const clock_main_clk_src_t lpc84x_mainclk_src_map[] = {
	kCLOCK_MainClkSrcFro,
	kCLOCK_MainClkSrcExtClk,
	kCLOCK_MainClkSrcWdtOsc,
	kCLOCK_MainClkSrcFroDiv,
};

struct lpc84x_clock_config {
	SYSCON_Type *base;
};

struct lpc84x_clock_data {
	struct k_mutex lock;
};

#define UART_CLK_MUX_SEL(n) DT_ENUM_IDX_OR(n, clock_source, 0)

#define UART_CLK_SELECT(n, inst)                                                                   \
	IF_ENABLED(DT_NODE_HAS_STATUS(n, okay), \
		(CLOCK_Select(CLK_MUX_DEFINE(FCLKSEL[inst], \
			UART_CLK_MUX_SEL(n)));))

#define SPI_CLK_MUX_SEL(n) DT_ENUM_IDX_OR(n, clock_source, 0)

#define SPI_CLK_SELECT(n, inst)                                                                    \
	IF_ENABLED(DT_NODE_HAS_STATUS(n, okay),					\
			(CLOCK_Select(CLK_MUX_DEFINE(FCLKSEL[inst + 9],		\
						     SPI_CLK_MUX_SEL(n)));))

static int lpc84x_clock_control_on(const struct device *dev, clock_control_subsys_t sub_system)
{
	struct lpc84x_clock_data *data = dev->data;
	uint32_t clk_id = (uint32_t)sub_system;

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (clk_id) {
	case LPC84X_CLK_UART0:
		UART_CLK_SELECT(DT_NODELABEL(uart0), 0);
		break;
	case LPC84X_CLK_UART1:
		UART_CLK_SELECT(DT_NODELABEL(uart1), 1);
		break;
	case LPC84X_CLK_UART2:
		UART_CLK_SELECT(DT_NODELABEL(uart2), 2);
		break;
	case LPC84X_CLK_UART3:
		UART_CLK_SELECT(DT_NODELABEL(uart3), 3);
		break;
	case LPC84X_CLK_UART4:
		UART_CLK_SELECT(DT_NODELABEL(uart4), 4);
		break;
	case LPC84X_CLK_SPI0:
		SPI_CLK_SELECT(DT_NODELABEL(spi0), 0);
		break;
	case LPC84X_CLK_SPI1:
		SPI_CLK_SELECT(DT_NODELABEL(spi1), 1);
		break;

	default:
		break;
	}

	CLOCK_EnableClock((clock_ip_name_t)clk_id);

	k_mutex_unlock(&data->lock);

	return 0;
}

static int lpc84x_clock_control_off(const struct device *dev, clock_control_subsys_t sub_system)
{
	struct lpc84x_clock_data *data = dev->data;
	uint32_t clk_id = (uint32_t)sub_system;

	k_mutex_lock(&data->lock, K_FOREVER);

	CLOCK_DisableClock((clock_ip_name_t)clk_id);

	k_mutex_unlock(&data->lock);

	return 0;
}

static uint32_t lpc84x_get_spi_clk_freq(uint8_t fclksel_idx)
{
	switch (SYSCON->FCLKSEL[fclksel_idx]) {
	case 0U:
		return CLOCK_GetFroFreq();
	case 1U:
		return CLOCK_GetMainClkFreq();
	case 2U:
		return CLOCK_GetFRG0ClkFreq();
	case 3U:
		return CLOCK_GetFRG1ClkFreq();
	case 4U:
		return CLOCK_GetFroFreq() >> 1U;
	default:
		return 0U;
	}
}

static int lpc84x_clock_control_get_rate(const struct device *dev,
					 clock_control_subsys_t sub_system, uint32_t *rate)
{
	ARG_UNUSED(dev);

	uint32_t clock_id = (clock_ip_name_t)sub_system;

	switch (clock_id) {
	case LPC84X_CLK_UART0:
		*rate = CLOCK_GetUart0ClkFreq();
		break;
	case LPC84X_CLK_UART1:
		*rate = CLOCK_GetUart1ClkFreq();
		break;
	case LPC84X_CLK_UART2:
		*rate = CLOCK_GetUart2ClkFreq();
		break;
	case LPC84X_CLK_UART3:
		*rate = CLOCK_GetUart3ClkFreq();
		break;
	case LPC84X_CLK_UART4:
		*rate = CLOCK_GetUart4ClkFreq();
		break;
	case LPC84X_CLK_SPI0:
		*rate = lpc84x_get_spi_clk_freq(9);
		break;
	case LPC84X_CLK_SPI1:
		*rate = lpc84x_get_spi_clk_freq(10);
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static DEVICE_API(clock_control, lpc84x_clock_api) = {
	.on = lpc84x_clock_control_on,
	.off = lpc84x_clock_control_off,
	.get_rate = lpc84x_clock_control_get_rate,
};

#define LPC84X_CLOCK_CONTROL_INIT(inst)                                                            \
	static int lpc84x_clock_init_##inst(const struct device *dev)                              \
	{                                                                                          \
		const struct lpc84x_clock_config *config = dev->config;                            \
		struct lpc84x_clock_data *data = dev->data;                                        \
                                                                                                   \
		k_mutex_init(&data->lock);                                                         \
                                                                                                   \
		/* ---- FRO Initialization ---- */                                                 \
		if (DT_INST_PROP(inst, enable_fro)) {                                              \
			POWER_DisablePD(kPDRUNCFG_PD_FRO);                                         \
                                                                                                   \
			uint32_t idx = DT_INST_ENUM_IDX(inst, fro_freq);                           \
			if (idx >= ARRAY_SIZE(lpc84x_fro_freq_map)) {                              \
				return -EINVAL;                                                    \
			}                                                                          \
                                                                                                   \
			CLOCK_SetFroOscFreq(lpc84x_fro_freq_map[idx]);                             \
                                                                                                   \
			if (DT_INST_PROP(inst, fro_low_power_boot)) {                              \
				CLOCK_SetFroOutClkSrc(kCLOCK_FroSrcLpwrBootValue);                 \
			} else {                                                                   \
				CLOCK_SetFroOutClkSrc(kCLOCK_FroSrcFroOsc);                        \
			}                                                                          \
		}                                                                                  \
		/* ---- FRO OUT Initialization ---- */                                             \
		IF_ENABLED(DT_INST_PROP(inst, enable_froout), (					\
					POWER_DisablePD(kPDRUNCFG_PD_FRO);			\
					POWER_DisablePD(kPDRUNCFG_PD_FRO_OUT);			\
					))                              \
                                                                                                   \
		/* ---- System Oscillator Initialization ---- */                                   \
		IF_ENABLED(DT_INST_PROP(inst, enable_sysosc), (			\
			BUILD_ASSERT(DT_INST_NODE_HAS_PROP(inst, sysosc_freq),	\
				"sysosc-freq is required when enable-sysosc is set"); \
			CLOCK_InitSysOsc(DT_INST_PROP(inst, sysosc_freq));	\
		))                                \
                                                                                                   \
		/* ---- External Clock source choice ---- */                                       \
		IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, extclk_src), (		\
			COND_CODE_1(IS_EQ(DT_INST_ENUM_IDX(inst, extclk_src), 0), ( \
				/* sysosc: handled by InitSysOsc if enabled */	\
				config->base->EXTCLKSEL &= ~SYSCON_EXTCLKSEL_SEL_MASK; \
			), (							\
				/* clkin: needs extclk-freq */			\
				BUILD_ASSERT(DT_INST_NODE_HAS_PROP(inst, extclk_freq), \
					"extclk-freq required for clkin");	\
				CLOCK_InitExtClkin(DT_INST_PROP(inst, extclk_freq)); \
			));							\
		))                           \
                                                                                                   \
		/* ---- Main Clock selection ---- */                                               \
		IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, mainclk_src), (		\
			CLOCK_SetMainClkSrc(					\
				lpc84x_mainclk_src_map[DT_INST_ENUM_IDX(inst, mainclk_src)]); \
		))                          \
                                                                                                   \
		/* ---- FRG0 Initialization ---- */                                                \
		IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, frg0_clk_src), ( \
			config->base->FRG[0].FRGCLKSEL = DT_INST_ENUM_IDX(inst, frg0_clk_src); \
			IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, frg0_mult), ( \
				config->base->FRG[0].FRGDIV = 0xFF; \
				config->base->FRG[0].FRGMULT = DT_INST_PROP(inst, frg0_mult); \
			)) \
		))                          \
                                                                                                   \
		/* ---- FRG1 Initialization ---- */                                                \
		IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, frg1_clk_src), ( \
			config->base->FRG[1].FRGCLKSEL = DT_INST_ENUM_IDX(inst, frg1_clk_src); \
			IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, frg1_mult), ( \
				config->base->FRG[1].FRGDIV = 0xFF; \
				config->base->FRG[1].FRGMULT = DT_INST_PROP(inst, frg1_mult); \
			)) \
		))                          \
                                                                                                   \
		CLOCK_SetCoreSysClkDiv(DT_INST_PROP(inst, ahb_clk_divider));                       \
                                                                                                   \
		CLOCK_SetFLASHAccessCyclesForFreq(CLOCK_GetMainClkFreq());                         \
		SystemCoreClockUpdate();                                                           \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	static const struct lpc84x_clock_config cfg_##inst = {                                     \
		.base = (SYSCON_Type *)DT_INST_REG_ADDR(inst),                                     \
	};                                                                                         \
                                                                                                   \
	static struct lpc84x_clock_data data_##inst;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, lpc84x_clock_init_##inst, NULL, &data_##inst, &cfg_##inst,     \
			      PRE_KERNEL_1, CONFIG_CLOCK_CONTROL_INIT_PRIORITY,                    \
			      &lpc84x_clock_api);

DT_INST_FOREACH_STATUS_OKAY(LPC84X_CLOCK_CONTROL_INIT)
