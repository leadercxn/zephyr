/*
 * Copyright (c) 2017 Intel Corporation
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT espressif_esp32_i2c

/* Include esp-idf headers first to avoid redefining BIT() macro */
#include <soc/i2c_reg.h>
#include <esp32/rom/gpio.h>
#include <soc/gpio_sig_map.h>
#include <hal/i2c_ll.h>
#include <hal/i2c_hal.h>
#include <hal/gpio_hal.h>

#include <soc.h>
#include <errno.h>
#include <drivers/gpio.h>
#include <drivers/i2c.h>
#include <drivers/interrupt_controller/intc_esp32.h>
#include <drivers/clock_control.h>
#include <sys/util.h>
#include <string.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(i2c_esp32, CONFIG_I2C_LOG_LEVEL);

#include "i2c-priv.h"

#define I2C_FILTER_CYC_NUM_DEF 7	/* Number of apb cycles filtered by default */
#define I2C_CLR_BUS_SCL_NUM 9	/* Number of SCL clocks to restore SDA signal */
#define I2C_CLR_BUS_HALF_PERIOD_US 5	/* Period of SCL clock to restore SDA signal */
#define I2C_TRANSFER_TIMEOUT_MSEC 500	/* Transfer timeout period */

/* Freq limitation when using different clock sources */
#define I2C_CLK_LIMIT_REF_TICK (1 * 1000 * 1000 / 20)	/* REF_TICK, no more than REF_TICK/20*/
#define I2C_CLK_LIMIT_APB (80 * 1000 * 1000 / 20)	/* Limited by APB, no more than APB/20 */
#define I2C_CLK_LIMIT_RTC (20 * 1000 * 1000 / 20)	/* Limited by RTC, no more than RTC/20 */
#define I2C_CLK_LIMIT_XTAL (40 * 1000 * 1000 / 20)	/* Limited by RTC, no more than XTAL/20 */

enum i2c_status_t {
	I2C_STATUS_READ,	/* read status for current master command */
	I2C_STATUS_WRITE,	/* write status for current master command */
	I2C_STATUS_IDLE,	/* idle status for current master command */
	I2C_STATUS_ACK_ERROR,	/* ack error status for current master command */
	I2C_STATUS_DONE,	/* I2C command done */
	I2C_STATUS_TIMEOUT,	/* I2C bus status error, and operation timeout */
};

struct i2c_esp32_pin {
	const char *gpio_name;
	int sig_out;
	int sig_in;
	gpio_pin_t pin;
};

struct i2c_esp32_data {
	i2c_hal_context_t hal;
	struct k_sem cmd_sem;
	struct k_sem transfer_sem;
	volatile enum i2c_status_t status;
	uint32_t dev_config;
	int cmd_idx;
	int irq_line;

	const struct device *scl_gpio;
	const struct device *sda_gpio;
};

typedef void (*irq_connect_cb)(void);

struct i2c_esp32_config {
	int index;

	const struct device *clock_dev;

	const struct i2c_esp32_pin scl;
	const struct i2c_esp32_pin sda;

	const clock_control_subsys_t clock_subsys;

	const struct {
		bool tx_lsb_first;
		bool rx_lsb_first;
	} mode;

	int irq_source;

	const uint32_t default_config;
	const uint32_t bitrate;
};

/* I2C clock characteristic, The order is the same as i2c_sclk_t. */
static uint32_t i2c_clk_alloc[I2C_SCLK_MAX] = {
	0,
#if SOC_I2C_SUPPORT_APB
	I2C_CLK_LIMIT_APB,	/* I2C APB clock characteristic */
#endif
#if SOC_I2C_SUPPORT_XTAL
	I2C_CLK_LIMIT_XTAL,	/* I2C XTAL characteristic */
#endif
#if SOC_I2C_SUPPORT_RTC
	I2C_CLK_LIMIT_RTC,	/* I2C 20M RTC characteristic */
#endif
#if SOC_I2C_SUPPORT_REF_TICK
	I2C_CLK_LIMIT_REF_TICK,	/* I2C REF_TICK characteristic */
#endif
};

static i2c_sclk_t i2c_get_clk_src(uint32_t clk_freq)
{
	for (i2c_sclk_t clk = I2C_SCLK_DEFAULT + 1; clk < I2C_SCLK_MAX; clk++) {
		if (clk_freq <= i2c_clk_alloc[clk]) {
			return clk;
		}
	}
	return I2C_SCLK_MAX;	/* flag invalid */
}

static int i2c_esp32_config_pin(const struct device *dev)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

	if (config->index >= SOC_I2C_NUM) {
		LOG_ERR("Invalid I2C peripheral number");
		return -EINVAL;
	}

	gpio_pin_set(data->sda_gpio, config->sda.pin, 1);
	gpio_pin_configure(data->sda_gpio, config->sda.pin,
			GPIO_PULL_UP | GPIO_OPEN_DRAIN | GPIO_OUTPUT | GPIO_INPUT);
	esp_rom_gpio_matrix_out(config->sda.pin, config->sda.sig_out, 0, 0);
	esp_rom_gpio_matrix_in(config->sda.pin, config->sda.sig_in, 0);

	gpio_pin_set(data->scl_gpio, config->scl.pin, 1);
	gpio_pin_configure(data->scl_gpio, config->scl.pin,
			GPIO_PULL_UP | GPIO_OPEN_DRAIN | GPIO_OUTPUT | GPIO_INPUT);
	esp_rom_gpio_matrix_out(config->scl.pin, config->scl.sig_out, 0, 0);
	esp_rom_gpio_matrix_in(config->scl.pin, config->scl.sig_in, 0);

	return 0;
}

/* Some slave device will die by accident and keep the SDA in low level,
 * in this case, master should send several clock to make the slave release the bus.
 * Slave mode of ESP32 might also get in wrong state that held the SDA low,
 * in this case, master device could send a stop signal to make esp32 slave release the bus.
 **/
static void IRAM_ATTR i2c_master_clear_bus(const struct device *dev)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

#ifndef SOC_I2C_SUPPORT_HW_CLR_BUS
	const struct i2c_esp32_config *config = dev->config;
	const int scl_half_period = I2C_CLR_BUS_HALF_PERIOD_US; /* use standard 100kHz data rate */
	int i = 0;
	gpio_pin_t scl_io = config->scl.pin;
	gpio_pin_t sda_io = config->sda.pin;

	gpio_pin_configure(data->scl_gpio, scl_io, GPIO_OUTPUT | GPIO_OPEN_DRAIN);
	gpio_pin_configure(data->sda_gpio, sda_io, GPIO_OUTPUT | GPIO_OPEN_DRAIN | GPIO_INPUT);
	/* If a SLAVE device was in a read operation when the bus was interrupted, */
	/* the SLAVE device is controlling SDA. If the slave is sending a stream of ZERO bytes, */
	/* it will only release SDA during the  ACK bit period. So, this reset code needs */
	/* to synchronize the bit stream with either the ACK bit, or a 1 bit to correctly */
	/* generate a STOP condition. */
	gpio_pin_set(data->scl_gpio, scl_io, 0);
	gpio_pin_set(data->sda_gpio, sda_io, 1);
	esp_rom_delay_us(scl_half_period);
	while (!gpio_pin_get(data->sda_gpio, sda_io) && (i++ < I2C_CLR_BUS_SCL_NUM)) {
		gpio_pin_set(data->scl_gpio, scl_io, 1);
		esp_rom_delay_us(scl_half_period);
		gpio_pin_set(data->scl_gpio, scl_io, 0);
		esp_rom_delay_us(scl_half_period);
	}
	gpio_pin_set(data->sda_gpio, sda_io, 0); /* setup for STOP */
	gpio_pin_set(data->scl_gpio, scl_io, 1);
	esp_rom_delay_us(scl_half_period);
	gpio_pin_set(data->sda_gpio, sda_io, 1); /* STOP, SDA low -> high while SCL is HIGH */
	i2c_esp32_config_pin(dev);
#else
	i2c_hal_master_clr_bus(&data->hal);
#endif
	i2c_hal_update_config(&data->hal);
}

static void IRAM_ATTR i2c_hw_fsm_reset(const struct device *dev)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

#ifndef SOC_I2C_SUPPORT_HW_FSM_RST
	const struct i2c_esp32_config *config = dev->config;
	int scl_low_period, scl_high_period;
	int scl_start_hold, scl_rstart_setup;
	int scl_stop_hold, scl_stop_setup;
	int sda_hold, sda_sample;
	int timeout;
	uint8_t filter_cfg;

	i2c_hal_get_scl_timing(&data->hal, &scl_high_period, &scl_low_period);
	i2c_hal_get_start_timing(&data->hal, &scl_rstart_setup, &scl_start_hold);
	i2c_hal_get_stop_timing(&data->hal, &scl_stop_setup, &scl_stop_hold);
	i2c_hal_get_sda_timing(&data->hal, &sda_sample, &sda_hold);
	i2c_hal_get_tout(&data->hal, &timeout);
	i2c_hal_get_filter(&data->hal, &filter_cfg);

	/* to reset the I2C hw module, we need re-enable the hw */
	clock_control_off(config->clock_dev, config->clock_subsys);
	i2c_master_clear_bus(dev);
	clock_control_on(config->clock_dev, config->clock_subsys);

	i2c_hal_master_init(&data->hal, config->index);
	i2c_hal_disable_intr_mask(&data->hal, I2C_LL_INTR_MASK);
	i2c_hal_clr_intsts_mask(&data->hal, I2C_LL_INTR_MASK);
	i2c_hal_set_scl_timing(&data->hal, scl_high_period, scl_low_period);
	i2c_hal_set_start_timing(&data->hal, scl_rstart_setup, scl_start_hold);
	i2c_hal_set_stop_timing(&data->hal, scl_stop_setup, scl_stop_hold);
	i2c_hal_set_sda_timing(&data->hal, sda_sample, sda_hold);
	i2c_hal_set_tout(&data->hal, timeout);
	i2c_hal_set_filter(&data->hal, filter_cfg);
#else
	i2c_hal_master_fsm_rst(&data->hal);
	i2c_master_clear_bus(dev);
#endif
	i2c_hal_update_config(&data->hal);
}

static int i2c_esp32_recover(const struct device *dev)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

	k_sem_take(&data->transfer_sem, K_FOREVER);
	i2c_hw_fsm_reset(dev);
	k_sem_give(&data->transfer_sem);

	return 0;
}

static int i2c_esp32_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

	if (!(dev_config & I2C_MODE_MASTER)) {
		LOG_ERR("Only I2C Master mode supported.");
		return -ENOTSUP;
	}

	data->dev_config = dev_config;

	i2c_trans_mode_t tx_mode = I2C_DATA_MODE_MSB_FIRST;
	i2c_trans_mode_t rx_mode = I2C_DATA_MODE_MSB_FIRST;

	if (config->mode.tx_lsb_first) {
		tx_mode = I2C_DATA_MODE_LSB_FIRST;
	}

	if (config->mode.rx_lsb_first) {
		rx_mode = I2C_DATA_MODE_LSB_FIRST;
	}

	i2c_hal_master_init(&data->hal, config->index);
	i2c_hal_set_data_mode(&data->hal, tx_mode, rx_mode);
	i2c_hal_set_filter(&data->hal, I2C_FILTER_CYC_NUM_DEF);
	i2c_hal_update_config(&data->hal);

	if (config->bitrate == 0) {
		LOG_ERR("Error configuring I2C speed.");
		return -ENOTSUP;
	}

	i2c_hal_set_bus_timing(&data->hal, config->bitrate, i2c_get_clk_src(config->bitrate));
	i2c_hal_update_config(&data->hal);

	irq_enable(data->irq_line);

	return 0;
}

static void IRAM_ATTR i2c_esp32_reset_fifo(const struct device *dev)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

	/* reset fifo buffers */
	i2c_hal_txfifo_rst(&data->hal);
	i2c_hal_rxfifo_rst(&data->hal);
}

static int IRAM_ATTR i2c_esp32_transmit(const struct device *dev)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

	/* start the transfer */
	i2c_hal_update_config(&data->hal);
	i2c_hal_trans_start(&data->hal);

	int ret = k_sem_take(&data->cmd_sem, K_MSEC(I2C_TRANSFER_TIMEOUT_MSEC));

	if (ret != 0) {
		/* If the I2C slave is powered off or the SDA/SCL is */
		/* connected to ground, for example, I2C hw FSM would get */
		/* stuck in wrong state, we have to reset the I2C module in this case. */
		i2c_hw_fsm_reset(dev);
		return -ETIMEDOUT;
	}

	if (data->status == I2C_STATUS_TIMEOUT) {
		i2c_hw_fsm_reset(dev);
		ret = -ETIMEDOUT;
	} else if (data->status == I2C_STATUS_ACK_ERROR) {
		ret = -EFAULT;
	} else {
		ret = 0;
	}

	return ret;
}

static void IRAM_ATTR i2c_esp32_write_addr(const struct device *dev, uint16_t addr)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;
	uint8_t addr_len = 1;
	uint8_t addr_byte = addr & 0xFF;

	i2c_hw_cmd_t cmd = {
		.op_code = I2C_LL_CMD_RESTART
	};

	/* write re-start command */
	i2c_hal_write_cmd_reg(&data->hal, cmd, data->cmd_idx++);

	/* write address value in tx buffer */
	i2c_hal_write_txfifo(&data->hal, &addr_byte, 1);
	if (data->dev_config & I2C_ADDR_10_BITS) {
		addr_byte = (addr >> 8) & 0xFF;
		i2c_hal_write_txfifo(&data->hal, &addr_byte, 1);
		addr_len++;
	}

	cmd = (i2c_hw_cmd_t) {
		.op_code = I2C_LL_CMD_WRITE,
		.ack_en = true,
		.byte_num = addr_len,
	};

	i2c_hal_write_cmd_reg(&data->hal, cmd, data->cmd_idx++);
}

static int IRAM_ATTR i2c_esp32_read_msg(const struct device *dev,
	struct i2c_msg *msg, uint16_t addr)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;
	uint8_t rd_filled = 0;
	uint8_t *read_pr = NULL;
	int ret = 0;

	/* reset command index and set status as read operation */
	data->cmd_idx = 0;
	data->status = I2C_STATUS_READ;

	i2c_hw_cmd_t cmd = {
		.op_code = I2C_LL_CMD_READ
	};

	i2c_hw_cmd_t hw_end_cmd = {
		.op_code = I2C_LL_CMD_END,
	};

	/* Set the R/W bit to R */
	addr |= BIT(0);

	if (msg->flags & I2C_MSG_RESTART) {
		/* write restart command and address */
		i2c_esp32_write_addr(dev, addr);
	}

	while (msg->len) {
		rd_filled = (msg->len > SOC_I2C_FIFO_LEN) ? SOC_I2C_FIFO_LEN : (msg->len - 1);
		read_pr = msg->buf;
		msg->len -= rd_filled;

		if (rd_filled) {
			cmd = (i2c_hw_cmd_t) {
				.op_code = I2C_LL_CMD_READ,
				.ack_en = false,
				.ack_val = 0,
				.byte_num = rd_filled
			};

			i2c_hal_write_cmd_reg(&data->hal, cmd, data->cmd_idx++);
		}

		/* I2C master won't acknowledge the last byte read from the
		 * slave device. Divide the read command in two segments as
		 * recommended by the ESP32 Technical Reference Manual.
		 */
		if (msg->len == 1) {
			cmd = (i2c_hw_cmd_t)  {
				.op_code = I2C_LL_CMD_READ,
				.byte_num = 1,
				.ack_val = 1,
			};
			msg->len = 0;
			rd_filled++;
			i2c_hal_write_cmd_reg(&data->hal, cmd, data->cmd_idx++);
		}

		if (msg->len == 0) {
			cmd = (i2c_hw_cmd_t)  {
				.op_code = I2C_LL_CMD_STOP,
				.byte_num = 0,
				.ack_val = 0,
				.ack_en = 0
			};
			i2c_hal_write_cmd_reg(&data->hal, cmd, data->cmd_idx++);
		}

		i2c_hal_write_cmd_reg(&data->hal, hw_end_cmd, data->cmd_idx++);
		i2c_hal_enable_master_rx_it(&data->hal);

		ret = i2c_esp32_transmit(dev);
		if (ret < 0) {
			LOG_ERR("I2C transfer error: %d", ret);
			return ret;
		}

		i2c_hal_read_rxfifo(&data->hal, msg->buf, rd_filled);
		msg->buf += rd_filled;

		/* reset fifo read pointer */
		data->cmd_idx = 0;
	}

	return 0;
}

static int IRAM_ATTR i2c_esp32_write_msg(const struct device *dev,
		struct i2c_msg *msg, uint16_t addr)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;
	uint8_t wr_filled = 0;
	uint8_t *write_pr = NULL;
	int ret = 0;

	/* reset command index and set status as write operation */
	data->cmd_idx = 0;
	data->status = I2C_STATUS_WRITE;

	i2c_hw_cmd_t cmd = {
		.op_code = I2C_LL_CMD_WRITE,
		.ack_en = true
	};

	i2c_hw_cmd_t hw_end_cmd = {
		.op_code = I2C_LL_CMD_END,
	};

	if (msg->flags & I2C_MSG_RESTART) {
		/* write restart command and address */
		i2c_esp32_write_addr(dev, addr);
	}

	for (;;) {
		wr_filled = (msg->len > SOC_I2C_FIFO_LEN) ? SOC_I2C_FIFO_LEN : msg->len;
		write_pr = msg->buf;
		msg->buf += wr_filled;
		msg->len -= wr_filled;
		cmd.byte_num = wr_filled;

		if (wr_filled > 0) {
			i2c_hal_write_txfifo(&data->hal, write_pr, wr_filled);
			i2c_hal_write_cmd_reg(&data->hal, cmd, data->cmd_idx++);
		}

		if (msg->len == 0 && (msg->flags & I2C_MSG_STOP)) {
			cmd = (i2c_hw_cmd_t) {
				.op_code = I2C_LL_CMD_STOP,
				.ack_en = false,
				.byte_num = 0
			};
			i2c_hal_write_cmd_reg(&data->hal, cmd, data->cmd_idx++);
		} else {
			i2c_hal_write_cmd_reg(&data->hal, hw_end_cmd, data->cmd_idx++);
		}

		i2c_hal_enable_master_tx_it(&data->hal);

		ret = i2c_esp32_transmit(dev);
		if (ret < 0) {
			LOG_ERR("I2C transfer error: %d", ret);
			return ret;
		}

		/* reset fifo write pointer */
		data->cmd_idx = 0;

		if (msg->len == 0) {
			break;
		}
	}

	return 0;
}

static int IRAM_ATTR i2c_esp32_transfer(const struct device *dev, struct i2c_msg *msgs,
			      uint8_t num_msgs, uint16_t addr)
{
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;
	struct i2c_msg *current, *next;
	int ret = 0;

	if (!num_msgs) {
		return 0;
	}

	/* Check for validity of all messages before transfer */
	current = msgs;

	/* Add restart flag on first message to send start event */
	current->flags |= I2C_MSG_RESTART;

	for (int k = 1; k <= num_msgs; k++) {
		if (k < num_msgs) {
			next = current + 1;

			/* messages of different direction require restart event */
			if ((current->flags & I2C_MSG_RW_MASK) != (next->flags & I2C_MSG_RW_MASK)) {
				if (!(next->flags & I2C_MSG_RESTART)) {
					ret = -EINVAL;
					break;
				}
			}

			/* check if there is any stop event in the middle of the transaction */
			if (current->flags & I2C_MSG_STOP) {
				ret = -EINVAL;
				break;
			}
		} else {
			/* make sure the last message contains stop event */
			current->flags |= I2C_MSG_STOP;
		}

		current++;
	}

	if (ret) {
		return ret;
	}

	k_sem_take(&data->transfer_sem, K_FOREVER);

	/* Mask out unused address bits, and make room for R/W bit */
	addr &= BIT_MASK(data->dev_config & I2C_ADDR_10_BITS ? 10 : 7);
	addr <<= 1;

	for (; num_msgs > 0; num_msgs--, msgs++) {

		if (data->status == I2C_STATUS_TIMEOUT || i2c_hal_is_bus_busy(&data->hal)) {
			i2c_hw_fsm_reset(dev);
		}

		/* reset all fifo buffer before start */
		i2c_esp32_reset_fifo(dev);

		/* These two interrupts some times can not be cleared when the FSM gets stuck. */
		/* So we disable them when these two interrupt occurs and re-enable them here. */
		i2c_hal_disable_intr_mask(&data->hal, I2C_LL_INTR_MASK);
		i2c_hal_clr_intsts_mask(&data->hal, I2C_LL_INTR_MASK);

		if ((msgs->flags & I2C_MSG_RW_MASK) == I2C_MSG_READ) {
			ret = i2c_esp32_read_msg(dev, msgs, addr);
		} else {
			ret = i2c_esp32_write_msg(dev, msgs, addr);
		}

		if (ret < 0) {
			break;
		}
	}

	k_sem_give(&data->transfer_sem);

	return ret;
}

static void IRAM_ATTR i2c_esp32_isr(void *arg)
{
	struct device *dev = (struct device *)arg;
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;
	i2c_intr_event_t evt_type = I2C_INTR_EVENT_ERR;

	if (data->status == I2C_STATUS_WRITE) {
		i2c_hal_master_handle_tx_event(&data->hal, &evt_type);
	} else if (data->status == I2C_STATUS_READ) {
		i2c_hal_master_handle_rx_event(&data->hal, &evt_type);
	}

	if (evt_type == I2C_INTR_EVENT_NACK) {
		data->status = I2C_STATUS_ACK_ERROR;
	} else if (evt_type == I2C_INTR_EVENT_TOUT) {
		data->status = I2C_STATUS_TIMEOUT;
	} else if (evt_type == I2C_INTR_EVENT_ARBIT_LOST) {
		data->status = I2C_STATUS_TIMEOUT;
	} else if (evt_type == I2C_INTR_EVENT_TRANS_DONE) {
		data->status = I2C_STATUS_DONE;
	}

	k_sem_give(&data->cmd_sem);
}

static int i2c_esp32_init(const struct device *dev);

static const struct i2c_driver_api i2c_esp32_driver_api = {
	.configure = i2c_esp32_configure,
	.transfer = i2c_esp32_transfer,
	.recover_bus = i2c_esp32_recover
};

static int IRAM_ATTR i2c_esp32_init(const struct device *dev)
{
	const struct i2c_esp32_config *config = dev->config;
	struct i2c_esp32_data *data = (struct i2c_esp32_data *const)(dev)->data;

	if (config->scl.gpio_name == NULL || config->sda.gpio_name == NULL) {
		LOG_ERR("Failed to get GPIO device");
		return -EINVAL;
	}

	data->scl_gpio = device_get_binding(config->scl.gpio_name);
	if (!data->scl_gpio) {
		LOG_ERR("Failed to get SCL GPIO device");
		return -EINVAL;
	}

	data->sda_gpio = device_get_binding(config->sda.gpio_name);
	if (!data->sda_gpio) {
		LOG_ERR("Failed to get SDA GPIO device");
		return -EINVAL;
	}

	int ret = i2c_esp32_config_pin(dev);

	if (ret < 0) {
		LOG_ERR("Failed to configure I2C pins");
		return -EINVAL;
	}

	clock_control_on(config->clock_dev, config->clock_subsys);

	data->irq_line = esp_intr_alloc(config->irq_source, 0, i2c_esp32_isr, (void *)dev, NULL);

	return i2c_esp32_configure(dev, config->default_config);
}

#define GPIO0_NAME COND_CODE_1(DT_NODE_HAS_STATUS(DT_NODELABEL(gpio0), okay), \
			     (DT_LABEL(DT_INST(0, espressif_esp32_gpio))), (NULL))
#define GPIO1_NAME COND_CODE_1(DT_NODE_HAS_STATUS(DT_NODELABEL(gpio1), okay), \
			     (DT_LABEL(DT_INST(1, espressif_esp32_gpio))), (NULL))

#define DT_I2C_ESP32_GPIO_NAME(idx, pin) ( \
	DT_INST_PROP(idx, pin) < 32 ? GPIO0_NAME : GPIO1_NAME)

#define I2C_ESP32_FREQUENCY(bitrate)				       \
	 (bitrate == I2C_BITRATE_STANDARD ? KHZ(100)	       \
	: bitrate == I2C_BITRATE_FAST     ? KHZ(400)	       \
	: bitrate == I2C_BITRATE_FAST_PLUS  ? MHZ(1) : 0)
#define I2C_FREQUENCY(idx)						       \
	I2C_ESP32_FREQUENCY(DT_INST_PROP(idx, clock_frequency))

#define ESP32_I2C_INIT(idx)		\
	static struct i2c_esp32_data i2c_esp32_data_##idx = {		  \
		.hal = {	\
			.dev = (i2c_dev_t *) DT_REG_ADDR(DT_NODELABEL(i2c##idx)),	\
		},	\
		.cmd_sem = Z_SEM_INITIALIZER(                            \
			i2c_esp32_data_##idx.cmd_sem, 0, 1),                 \
		.transfer_sem = Z_SEM_INITIALIZER(                        \
			i2c_esp32_data_##idx.transfer_sem, 1, 1)		      \
	};							\
								\
	static const struct i2c_esp32_config i2c_esp32_config_##idx = {	       \
	.index = idx, \
	.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(idx)), \
	.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(idx, offset), \
	.scl = {	\
		.gpio_name = DT_I2C_ESP32_GPIO_NAME(idx, scl_pin),	\
		.sig_out = I2CEXT##idx##_SCL_OUT_IDX,	\
		.sig_in = I2CEXT##idx##_SCL_IN_IDX,	\
		.pin = DT_INST_PROP(idx, scl_pin), \
	},	\
	.sda = {	\
		.gpio_name = DT_I2C_ESP32_GPIO_NAME(idx, sda_pin),	\
		.sig_out = I2CEXT##idx##_SDA_OUT_IDX,	\
		.sig_in = I2CEXT##idx##_SDA_IN_IDX,	\
		.pin = DT_INST_PROP(idx, sda_pin), \
	},	\
	.mode = { \
		.tx_lsb_first = DT_INST_PROP(idx, tx_lsb), \
		.rx_lsb_first = DT_INST_PROP(idx, rx_lsb), \
	}, \
	.irq_source = ETS_I2C_EXT##idx##_INTR_SOURCE,	\
	.bitrate = I2C_FREQUENCY(idx),	\
	.default_config = I2C_MODE_MASTER,				\
	};								       \
	I2C_DEVICE_DT_DEFINE(DT_NODELABEL(i2c##idx),					       \
		      i2c_esp32_init,					       \
		      NULL,				       \
		      &i2c_esp32_data_##idx,				       \
		      &i2c_esp32_config_##idx,				       \
		      POST_KERNEL,					       \
		      CONFIG_I2C_INIT_PRIORITY,	       \
		      &i2c_esp32_driver_api); \

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c0), okay)
ESP32_I2C_INIT(0);
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay)
ESP32_I2C_INIT(1);
#endif
