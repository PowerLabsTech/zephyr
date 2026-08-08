#include <zephyr/drivers/can/can_uart_to_can.h>
#include <zephyr/sys/clock.h>

#include <stdio.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/toolchain.h>
#include <zephyr/types.h>
#include "zephyr/sys/__assert.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(can_uart_to_can, CONFIG_CAN_LOG_LEVEL);

#define DT_DRV_COMPAT                   powerlabs_uart_to_can
#define UART_TO_CAN_ASYNC_RX_TIMEOUT_US 2000

static void process_data_uart_data(const struct device *uart_to_can_dev);

static inline uint8_t ring_buf_get_char(struct ring_buf *buf)
{
	uint8_t data;
	assert(ring_buf_get(buf, &data, 1) == 1);
	return data;
}

static inline bool ring_buf_get_char_to_uint(struct ring_buf *buf, uint8_t num_of_bytes, int base,
					     unsigned long *out_val)
{
	char data[num_of_bytes + 1];
	char *temp_data;
	unsigned long temp_val;
	for (uint32_t i = 0; i < num_of_bytes; i++) {
		data[i] = ring_buf_get_char(buf);
	}
	data[num_of_bytes] = '\0';
	temp_val = strtoul(data, &temp_data, base);

	if ((uintptr_t)temp_data != (uintptr_t)data) {
		*out_val = temp_val;
		return true;
	}
	return false;
}

static inline bool ring_buf_get_hex_to_uint8_t(struct ring_buf *buf, uint8_t *out_val)
{
	char data[2 + 1];
	char *temp_data;
	unsigned long temp_val;
	for (uint32_t i = 0; i < 2; i++) {
		data[i] = ring_buf_get_char(buf);
	}
	data[2] = '\0';
	temp_val = strtoul(data, &temp_data, MESSAGE_BASE);

	if ((uintptr_t)temp_data != (uintptr_t)data) {
		*out_val = temp_val;
		return true;
	}
	return false;
}

// static int copy_data_to_ring_buf(struct ring_buf *ring_buf, const uint8_t
// *const data,
//                           size_t data_len) {
//   uint8_t *temp_data;
//   int err;
//   uint32_t bytes_written2 = 0;

//   uint32_t bytes_written = ring_buf_put_claim(ring_buf, &temp_data,
//   data_len); memcpy(temp_data, data, bytes_written); if
//   (ring_buf_put_finish(ring_buf, bytes_written) != 0) {
//     err = -1;
//     goto copy_data_to_ring_buf_return;
//   }

//   if (bytes_written < data_len && ring_buf_space_get(ring_buf) > 0) {
//     bytes_written2 =
//         ring_buf_put_claim(ring_buf, &temp_data, data_len - bytes_written);
//     memcpy(temp_data, &data[bytes_written], bytes_written2);
//     if (ring_buf_put_finish(ring_buf, bytes_written2) != 0) {
//       err = -1;
//       goto copy_data_to_ring_buf_return;
//     }
//   }
//   err = bytes_written + bytes_written2;

// copy_data_to_ring_buf_return:
//   return err;
// }

// static int copy_data_from_ring_buf(struct ring_buf *ring_buf, uint8_t *const
// data,
//                             size_t data_len) {
//   uint8_t *temp_data;
//   int err;
//   uint32_t bytes_read2 = 0;

//   uint32_t bytes_read = ring_buf_get_claim(ring_buf, &temp_data, data_len);
//   memcpy(data, temp_data, bytes_read);
//   if (ring_buf_get_finish(ring_buf, bytes_read) != 0) {
//     err = -1;
//     goto copy_data_from_ring_buf_return;
//   }

//   if (bytes_read < data_len && ring_buf_size_get(ring_buf) > 0) {
//     bytes_read2 =
//         ring_buf_get_claim(ring_buf, &temp_data, data_len - bytes_read);
//     memcpy(&data[bytes_read], temp_data, bytes_read2);
//     if (ring_buf_get_finish(ring_buf, bytes_read2) != 0) {
//       err = -1;
//       goto copy_data_from_ring_buf_return;
//     }
//   }
//   err = bytes_read + bytes_read2;

// copy_data_from_ring_buf_return:
//   return err;
// }

void clear_buf_till_r_with_atleast_one_byte(struct ring_buf *buf)
{
	uint32_t buf_size = ring_buf_size_get(buf);
	for (uint32_t i = 0; i < buf_size; i++) {
		if (ring_buf_get_char(buf) == '\r') {
			break;
		}
	}
}

bool ring_buf_has_wrapped(struct ring_buf *buf)
{
	uint32_t data_size = ring_buf_size_get(buf);
	uint8_t *data;
	uint32_t claim_size = ring_buf_get_claim(buf, &data, data_size);
	assert(!ring_buf_get_finish(buf, 0));
	return claim_size < data_size;
}

int search_r_consider_wrap(struct ring_buf *buf)
{

	uint8_t *data;
	uint8_t *data_remain;
	bool buf_has_wrapped = ring_buf_has_wrapped(buf);
	uint32_t get_size = ring_buf_size_get(buf);
	uint32_t get_claim_size = ring_buf_get_claim(buf, &data, get_size);
	int ret_val = INT_MIN;
	for (uint32_t i = 0; i < get_claim_size; i++) {
		if (data[i] == '\r') {
			LOG_DBG("Found \\r at index %d", i);
			ret_val = i;
			break;
		}
	}
	if (buf_has_wrapped) {
		uint32_t get_claim_size2 =
			ring_buf_get_claim(buf, &data_remain, get_size - get_claim_size);
		for (uint32_t i = 0; i < get_claim_size2; i++) {
			if (data_remain[i] == '\r') {
				LOG_DBG("Found \\r at index %d", -1 * i);
				ret_val = -1 * i;
				break;
			}
		}
	}
	if (ring_buf_get_finish(buf, 0) != 0) {
		LOG_ERR("Failed to free ring buffer");
	}
	return ret_val;
}

static void process_can_frame(const struct device *uart_to_can_dev, struct can_frame *frame,
			      int filter_id)
{
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;

	if (data->rx_cb[filter_id].callback != NULL) {
		data->rx_cb[filter_id].callback(uart_to_can_dev, frame,
						data->rx_cb[filter_id].user_data);
	}
}

static int parse_can_message(struct ring_buf *const buf, const uint32_t can_id, bool is_extended_id,
			     bool is_rtr, struct can_frame *frame)
{
	int err = 0;
	unsigned long temp_long;
	uint8_t dlc;
	uint8_t can_data[8];
	if (ring_buf_size_get(buf) < 1) {
		err = -1;
		LOG_ERR("Not enough data in buffer");
		goto parse_can_message_11bit_return;
	}
	if (ring_buf_get_char_to_uint(buf, 1, MESSAGE_BASE, &temp_long)) {
		dlc = temp_long;
	} else {
		err = -1;
		LOG_ERR("Invalid dlc");
		goto parse_can_message_11bit_return;
	}
	if (ring_buf_size_get(buf) < 2 * dlc) {
		err = -1;
		LOG_ERR("Not enough data in buffer");
		goto parse_can_message_11bit_return;
	}
	for (uint32_t i = 0; i < dlc; i++) {
		if (!ring_buf_get_hex_to_uint8_t(buf, &can_data[i])) {
			err = -1;
			LOG_ERR("Invalid data");
			goto parse_can_message_11bit_return;
		}
	}

	frame->id = can_id;
	frame->dlc = dlc;
	frame->flags = 0;
	frame->reserved = 0;
	memcpy(frame->data, can_data, dlc);

	if (is_extended_id) {
		frame->flags |= CAN_FRAME_IDE;
	}

	if (is_rtr) {
		frame->flags |= CAN_FRAME_RTR;
	}
parse_can_message_11bit_return:
	return err;
}

static int parse_can_message_11bit(const struct device *uart_to_can_dev, struct ring_buf *const buf,
				   struct can_frame *frame)
{
	int err = 0;
	unsigned long temp_long;
	int filter_id;
	uint16_t can_id = 0;
	if (ring_buf_size_get(buf) < (CAN_ID_11_BIT_BYTE_LENGHT + 2 + 1)) {
		err = -1;
		LOG_ERR("Not enough data in buffer");
		goto parse_can_message_11bit_return;
	}
	if (ring_buf_get_char_to_uint(buf, CAN_ID_11_BIT_BYTE_LENGHT, MESSAGE_BASE, &temp_long)) {
		can_id = temp_long;
	} else {
		err = -1;
		LOG_ERR("Invalid CAN ID");
		goto parse_can_message_11bit_return;
	}

	if (ring_buf_get_char_to_uint(buf, 2, MESSAGE_BASE, &temp_long)) {
		filter_id = temp_long;
	} else {
		err = -1;
		LOG_ERR("Invalid filter id");
		goto parse_can_message_11bit_return;
	}

	err = parse_can_message(buf, can_id, false, false, frame);

	if (err == 0) {
		process_can_frame(uart_to_can_dev, frame, filter_id);
	}

parse_can_message_11bit_return:
	return err;
}

static int parse_can_message_29bit(const struct device *uart_to_can_dev, struct ring_buf *const buf,
				   struct can_frame *frame)
{
	int err = 0;
	unsigned long temp_long;
	int filter_id;
	uint32_t can_id = 0;
	if (ring_buf_size_get(buf) < (CAN_ID_29_BIT_BYTE_LENGHT + 2 + 1)) {
		err = -1;
		LOG_ERR("Not enough data in buffer");
		goto parse_can_message_29bit_return;
	}
	if (ring_buf_get_char_to_uint(buf, CAN_ID_29_BIT_BYTE_LENGHT, MESSAGE_BASE, &temp_long)) {
		can_id = temp_long;
	} else {
		err = -1;
		LOG_ERR("Invalid CAN ID");
		goto parse_can_message_29bit_return;
	}
	if (ring_buf_get_char_to_uint(buf, 2, MESSAGE_BASE, &temp_long)) {
		filter_id = temp_long;
	} else {
		err = -1;
		LOG_ERR("Invalid filter id");
		goto parse_can_message_29bit_return;
	}

	err = parse_can_message(buf, can_id, true, false, frame);

	if (err == 0) {
		process_can_frame(uart_to_can_dev, frame, filter_id);
	}
parse_can_message_29bit_return:
	return err;
}

struct uart_message can_frame_to_uart_message(const struct can_frame *frame)
{
	struct uart_message message;
	size_t offset = 0;
	if (frame->flags & CAN_FRAME_IDE) {
		message.buffer[offset++] = 'T';
		assert(snprintf(&message.buffer[offset], 9, "%08x", frame->id) == 8);
		offset += 8;
	} else {
		message.buffer[offset++] = 't';
		assert(snprintf(&message.buffer[offset], 4, "%03x", frame->id) == 3);
		offset += 3;
	}
	assert(snprintf(&message.buffer[offset], 2, "%01x", frame->dlc) == 1);
	offset += 1;

	for (size_t i = 0; i < frame->dlc; i++) {
		assert(snprintf(&message.buffer[offset], 3, "%02x", frame->data[i]) == 2);
		offset += 2;
	}
	message.buffer[offset++] = '\r';
	message.buffer_size = offset;
	assert(offset < MAX_UART_CAN_FRAME);
	return message;
}

static void uart_to_can_serial_rx_fifo_drain(const struct device *uart_to_can_dev)
{

	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;

	uint8_t buf[8];
	int n;

	do {
		n = uart_fifo_read(uart_dev, buf, sizeof(buf));
	} while (n == sizeof(buf));

	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	ring_buf_reset(&data->rx_ring_buffer);
}

static int uart_to_can_rx_enable(const struct device *uart_to_can_dev)
{
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;

	if (IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
		int err;

		if (data->async_rx_enabled) {
			return 0;
		}

		data->next_async_rx_buf_idx = 1;
		err = uart_rx_enable(uart_dev, data->uart_async_rx_buf[0],
				     UART_TO_CAN_ASYNC_RX_BUF_SIZE,
				     UART_TO_CAN_ASYNC_RX_TIMEOUT_US);
		if (err == 0) {
			data->async_rx_enabled = true;
		}

		return err;
	}

	uart_irq_rx_enable(uart_dev);
	return 0;
}

static int uart_to_can_rx_disable(const struct device *uart_to_can_dev)
{
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;

	if (IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
		int err;

		if (!data->async_rx_enabled) {
			return 0;
		}

		err = uart_rx_disable(uart_dev);
		if (err == 0) {
			data->async_rx_enabled = false;
		}

		return err;
	}

	uart_irq_rx_disable(uart_dev);
	return 0;
}

static void uart_to_can_async_tx_start_next(const struct device *uart_to_can_dev)
{
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	struct uart_message *next_msg;
	int err;

	err = k_msgq_get(&data->can_tx_mail_box, &next_msg, K_NO_WAIT);
	if (err != 0) {
		return;
	}

	err = uart_tx(uart_dev, next_msg->buffer, next_msg->buffer_size, 1000);
	if (err == 0) {
		data->current_msg = next_msg;
		return;
	}

	(void)k_msgq_put(&data->can_tx_mail_box, &next_msg, K_NO_WAIT);
	LOG_ERR("uart_tx failed with %d", err);
}

static void uart_to_can_handle_rx_bytes(const struct device *uart_to_can_dev, const uint8_t *bytes,
					size_t len)
{
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	uint32_t written;

	if (len == 0) {
		return;
	}

	written = ring_buf_put(&data->rx_ring_buffer, bytes, len);
	if (written < len) {
		LOG_WRN("RX ring buffer overflow (%u/%u)", written, len);
	}

	process_data_uart_data(uart_to_can_dev);
}

static int send_uart_internal_no_blocking(const struct device *uart_to_can_dev,
					  struct uart_message *msg_ptr)
{
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;

	int err;
	if (IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
		if (data->current_msg == NULL) {
			err = uart_tx(uart_dev, msg_ptr->buffer, msg_ptr->buffer_size, 10000);
			if (err == 0) {
				data->current_msg = msg_ptr;
				goto send_uart_internal_return;
			}
			if (err != -EBUSY) {
				goto send_uart_internal_return;
			}
		}

		err = k_msgq_put(&data->can_tx_mail_box, &msg_ptr, K_NO_WAIT);
		if (err != 0) {
			goto send_uart_internal_return;
		}
	} else {
		err = k_msgq_put(&data->can_tx_mail_box, &msg_ptr, K_NO_WAIT);
		if (err != 0) {
			goto send_uart_internal_return;
		}
		uart_irq_tx_enable(uart_dev);
	}
send_uart_internal_return:
	return err;
}

static int send_uart(const struct device *uart_to_can_dev, const uint8_t *buffer,
		     const size_t buf_size, k_timeout_t timeout)
{
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	struct uart_message *msg_ptr;
	int err;
	err = k_mem_slab_alloc(&data->can_tx_slab, (void **)&msg_ptr, timeout);
	if (err != 0) {
		goto send_uart_return;
	}

	msg_ptr->buffer_size = buf_size;
	memcpy(msg_ptr->buffer, buffer, buf_size);

	err = send_uart_internal_no_blocking(uart_to_can_dev, msg_ptr);

	if (err != 0) {
		k_mem_slab_free(&data->can_tx_slab, msg_ptr);
		goto send_uart_return;
	}
	err = buf_size;
send_uart_return:
	return err;
}

static int uart_to_can_send_serial_synchronous(const struct device *uart_to_can_dev,
					       const uint8_t *msg, size_t msg_len,
					       k_timeout_t timeout)
{
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	int err = data->current_msg_idx;
	ARG_UNUSED(uart_dev);

	err = uart_to_can_rx_enable(uart_to_can_dev);
	if (err != 0) {
		return err;
	}

	// int64_t start_time = k_uptime_get();
	k_timepoint_t end = sys_timepoint_calc(timeout);
	k_sem_reset(&data->sem);
	err = send_uart(uart_to_can_dev, msg, msg_len, timeout);
	if (err != (int)msg_len) {
		err = -ENODEV;
		goto uart_to_can_send_serial_synchronous_return;
	}
	// timeout = K_MSEC(k_uptime_get() - start_time);
	timeout = sys_timepoint_timeout(end);
	err = k_sem_take(&data->sem, timeout);
	if (err != 0) {
		err = -ETIMEDOUT;
		goto uart_to_can_send_serial_synchronous_return;
	}
	err = data->response;
uart_to_can_send_serial_synchronous_return:
	return err;
}

static void uart_cb_async_handler(const struct device *uart_dev, struct uart_event *evt,
				  void *app_data)
{
	const struct device *uart_to_can_dev = (const struct device *)app_data;
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;

	if (uart_to_can_dev == NULL || config->uart_dev != uart_dev) {
		LOG_ERR("Uart to can device is not properly initialized");
		return;
	}

	switch (evt->type) {
	case UART_TX_DONE:
		if (data->current_msg != NULL) {
			k_mem_slab_free(&data->can_tx_slab, (void *)data->current_msg);
			data->current_msg = NULL;
		}
		uart_to_can_async_tx_start_next(uart_to_can_dev);
		break;
	case UART_TX_ABORTED:
		if (data->current_msg != NULL) {
			k_mem_slab_free(&data->can_tx_slab, (void *)data->current_msg);
			data->current_msg = NULL;
		}
		uart_to_can_async_tx_start_next(uart_to_can_dev);
		break;
	case UART_RX_RDY:
		uart_to_can_handle_rx_bytes(uart_to_can_dev, &evt->data.rx.buf[evt->data.rx.offset],
					    evt->data.rx.len);
		break;
	case UART_RX_BUF_REQUEST: {
		uint8_t idx = data->next_async_rx_buf_idx;
		uart_rx_buf_rsp(uart_dev, data->uart_async_rx_buf[idx],
				UART_TO_CAN_ASYNC_RX_BUF_SIZE);
		data->next_async_rx_buf_idx = (idx + 1U) % 2U;
		break;
	}
	case UART_RX_DISABLED:
		data->async_rx_enabled = false;
		break;
	default:
		break;
	}
}

static void process_data_uart_data(const struct device *uart_to_can_dev)
{
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	struct tx_callback_ctx callback_msg;

	int err = -1;
	// char response = 0;
	uint8_t response = 0;
	unsigned long temp_uint = ring_buf_size_get(&data->rx_ring_buffer);
	//   struct uart_message uart_message;
	struct can_frame frame;
	if (temp_uint > 3 && search_r_consider_wrap(&data->rx_ring_buffer) != INT_MIN) {
		uint8_t command = ring_buf_get_char(&data->rx_ring_buffer);

		switch (command) {
		case 'r':
			LOG_DBG("Send data to standard 11 bit CAN");
			err = parse_can_message_11bit(uart_to_can_dev, &data->rx_ring_buffer,
						      &frame);
			break;
		case 'R':
			LOG_DBG("Send data to standard 29 bit CAN");
			err = parse_can_message_29bit(uart_to_can_dev, &data->rx_ring_buffer,
						      &frame);
			break;

		case 't':
		case 'T':
			if (ring_buf_get_char_to_uint(&data->rx_ring_buffer, 2, 16, &temp_uint)) {
				response = (uint8_t)(temp_uint & 0xFF);
			}
			__ASSERT(err != 0xFF, "Something went really wrong with can send");
			if (response == 0xFF) {
				LOG_ERR("Something went really wrong with can send");
			}
			if (k_msgq_get(&data->tx_callback_fifo, &callback_msg, K_NO_WAIT) != 0) {
				LOG_ERR("No call back registered to send data to can callback");
				break;
			}
			callback_msg.callback(uart_to_can_dev, response, callback_msg.user_data);
			break;
		case 'O':
		case 'C':
		case 'L':
		case 'f':
		case 'm':
		case 'M':
			if (ring_buf_get_char_to_uint(&data->rx_ring_buffer, 2, 16, &temp_uint)) {
				response = (uint8_t)(temp_uint & 0xFF);
				data->response = response;
				k_sem_give(&data->sem);
			}
			break;
		default:
			LOG_ERR("Unrecongnised command");
			err = -ENOENT;
			break;
		}
		clear_buf_till_r_with_atleast_one_byte(&data->rx_ring_buffer);
	}
}

static void cb_handler_rx(const struct device *uart_to_can_dev)
{
	ARG_UNUSED(uart_to_can_dev);
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;

	int n = 0;

	uint8_t *temp_data;
	while (1) {
		int bytes_len = ring_buf_put_claim(&data->rx_ring_buffer, &temp_data,
						   ring_buf_space_get(&data->rx_ring_buffer));
		if (bytes_len < 1) {
			n = 0;
		} else {
			n = uart_fifo_read(uart_dev, temp_data, bytes_len);
		}
		if (ring_buf_put_finish(&data->rx_ring_buffer, n) != 0) {
			LOG_ERR("Error ring_buf_put_finish");
		}
		if (bytes_len < 1 || n < bytes_len) {
			break;
		}
	}
	process_data_uart_data(uart_to_can_dev);
}

static void cb_handler_tx(const struct device *uart_to_can_dev)
{
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;

	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	int err;
	if (data->current_msg == NULL ||
	    (data->current_msg_idx >= (int)data->current_msg->buffer_size)) {
		data->current_msg_idx = 0;
		if (data->current_msg != NULL) {
			k_mem_slab_free(&data->can_tx_slab, (void *)data->current_msg);
			data->current_msg = NULL;
		}
		err = k_msgq_get(&data->can_tx_mail_box, &data->current_msg, K_NO_WAIT);
		if (err != 0) {
			uart_irq_tx_disable(uart_dev);
			goto cb_handler_tx_return;
		}
	}

	err = uart_fifo_fill(uart_dev, &data->current_msg->buffer[data->current_msg_idx],
			     data->current_msg->buffer_size - data->current_msg_idx);
	if (err <= 0) {
		goto cb_handler_tx_return;
	}
	data->current_msg_idx += err;

cb_handler_tx_return:
	return;
}

static void uart_cb_handler(const struct device *uart_dev, void *app_data)
{
	struct device *uart_to_can_dev = (struct device *)app_data;
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;

	if (uart_to_can_dev == NULL || config->uart_dev != uart_dev) {
		LOG_ERR("Uart to can device is not properly initialized");
		return;
	}

	if (uart_irq_update(uart_dev) && uart_irq_is_pending(uart_dev)) {

		if (uart_irq_rx_ready(uart_dev)) {
			cb_handler_rx(uart_to_can_dev);
		}

		if (uart_irq_tx_ready(uart_dev)) {
			cb_handler_tx(uart_to_can_dev);
		}
	}
}

static int uart_to_can_reset(const struct device *uart_to_can_dev)
{
	// TODO(Matthew): Make better
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	const char *msg = "L\r";
	int err;
	int msg_len = strlen(msg);

	struct uart_message *temp_msg_ptr;

	err = k_mutex_lock(&data->inst_mutex, K_MSEC(10000));
	if (err != 0) {
		LOG_ERR("Failed to lock the mutex in uart_to_can_reset");
		goto uart_to_can_reset_return;
	}
	(void)uart_to_can_rx_disable(uart_to_can_dev);
	if (IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
		(void)uart_tx_abort(uart_dev);
		if (data->current_msg != NULL) {
			k_mem_slab_free(&data->can_tx_slab, (void *)data->current_msg);
			data->current_msg = NULL;
		}
	} else {
		uart_irq_tx_disable(uart_dev);
	}
	uart_to_can_serial_rx_fifo_drain(uart_to_can_dev);
	while (k_msgq_get(&data->can_tx_mail_box, &temp_msg_ptr, K_NO_WAIT) == 0) {
		k_mem_slab_free(&data->can_tx_slab, temp_msg_ptr);
	}
	err = uart_to_can_send_serial_synchronous(uart_to_can_dev, msg, msg_len, K_MSEC(100));

	(void)uart_to_can_rx_disable(uart_to_can_dev);
	if (!IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
		uart_irq_tx_disable(uart_dev);
	}

	k_mutex_unlock(&data->inst_mutex);

uart_to_can_reset_return:
	return err;
}

static int uart_to_can_start(const struct device *uart_to_can_dev)
{
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	int err;
	if (data->common.started) {
		err = -EALREADY;
		goto uart_to_can_start_return;
	}
	const char *msg = "O\r";
	int msg_len = strlen(msg);

	struct uart_message *temp_msg_ptr;

	err = k_mutex_lock(&data->inst_mutex, K_MSEC(10000));
	if (err != 0) {
		LOG_ERR("Failed to lock the mutex in uart_to_can_start");
		goto uart_to_can_start_return;
	}

	(void)uart_to_can_rx_disable(uart_to_can_dev);
	if (IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
		(void)uart_tx_abort(uart_dev);
		if (data->current_msg != NULL) {
			k_mem_slab_free(&data->can_tx_slab, (void *)data->current_msg);
			data->current_msg = NULL;
		}
	} else {
		uart_irq_tx_disable(uart_dev);
	}
	uart_to_can_serial_rx_fifo_drain(uart_to_can_dev);
	while (k_msgq_get(&data->can_tx_mail_box, &temp_msg_ptr, K_NO_WAIT) == 0) {
		k_mem_slab_free(&data->can_tx_slab, temp_msg_ptr);
	}

	err = uart_to_can_send_serial_synchronous(uart_to_can_dev, msg, msg_len, K_MSEC(100));
	if (err == 0) {
		data->common.started = true;
	}
	k_mutex_unlock(&data->inst_mutex);

uart_to_can_start_return:
	return err;
}

static int uart_to_can_add_rx_filter(const struct device *uart_to_can_dev,
				     can_rx_callback_t callback, void *user_data,
				     const struct can_filter *filter)
{
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	int err;
	char buffer[1 + 16 + 1];
	if (filter->flags & CAN_EXT_ID_MASK) {
		buffer[0] = 'M';
	} else {
		buffer[0] = 'm';
	}
	assert(snprintf(&buffer[1], 9, "%08x", filter->id) == 8);
	assert(snprintf(&buffer[9], 9, "%08x", filter->mask) == 8);
	buffer[17] = COMMAND_RESPONSE_OKAY[0];

	// const char *msg = "M0000001000000001\r";

	err = k_mutex_lock(&data->inst_mutex, K_MSEC(10000));
	if (err != 0) {
		LOG_ERR("Failed to lock the mutex in uart_to_can_add_rx_filter");
		goto uart_to_can_add_rx_filter_return;
	}
	err = uart_to_can_send_serial_synchronous(uart_to_can_dev, buffer, 18, K_MSEC(100));

	if (err >= 0) {
		data->rx_cb[err].callback = callback;
		data->rx_cb[err].user_data = user_data;
	}
	k_mutex_unlock(&data->inst_mutex);

uart_to_can_add_rx_filter_return:
	return err;
}

static void uart_to_can_remove_rx_filter(const struct device *uart_to_can_dev, int filter_id)
{
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	int err;
	if (filter_id >= (int)(sizeof(data->rx_cb) / sizeof(data->rx_cb[0])) ||
	    data->rx_cb[filter_id].callback == NULL) {
		goto uart_to_can_remove_rx_filter_return;
	}

	char buffer[1 + 2 + 1];
	buffer[0] = 'f';
	assert(snprintf(&buffer[1], 3, "%02x", filter_id) == 2);
	buffer[3] = COMMAND_RESPONSE_OKAY[0];

	err = k_mutex_lock(&data->inst_mutex, K_MSEC(10000));
	if (err != 0) {
		LOG_ERR("Failed to lock the mutex in uart_to_can_remove_rx_filter");
		goto uart_to_can_remove_rx_filter_return;
	}
	err = uart_to_can_send_serial_synchronous(uart_to_can_dev, buffer, 4, K_MSEC(100));

	if (err < 0) {
		goto uart_to_can_remove_rx_filter_unlock_mutex;
	}
	if (data->rx_cb[filter_id].callback != NULL) {
		data->rx_cb[filter_id].callback = NULL;
		data->rx_cb[filter_id].user_data = NULL;
	}
uart_to_can_remove_rx_filter_unlock_mutex:
	k_mutex_unlock(&data->inst_mutex);

uart_to_can_remove_rx_filter_return:
	return;
}

static int uart_to_can_send(const struct device *uart_to_can_dev, const struct can_frame *frame,
			    k_timeout_t timeout, can_tx_callback_t callback, void *user_data)
{
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	struct tx_callback_ctx callback_msg;
	int err;
	if (!data->common.started) {
		err = -ENODEV;
		goto uart_to_can_send_return;
	}

	callback_msg.callback = callback;
	callback_msg.user_data = user_data;

	struct uart_message *msg_ptr;

	k_timepoint_t end_time = sys_timepoint_calc(timeout);

	while ((k_msgq_num_free_get(&data->tx_callback_fifo) < 1)

	       && !sys_timepoint_expired(end_time)) {
		k_msleep(10);
	}

	err = k_mutex_lock(&data->inst_mutex, K_MSEC(10000));
	if (err != 0) {
		LOG_ERR("Failed to lock the mutex in uart_to_can_send");
		goto uart_to_can_send_return;
	}

	if (k_msgq_num_free_get(&data->tx_callback_fifo) < 1) {
		err = -ENODEV;
		goto uart_to_can_send_unlock_mutex;
	}

	timeout = sys_timepoint_timeout(end_time);
	err = k_mem_slab_alloc(&data->can_tx_slab, (void **)&msg_ptr, timeout);

	if (err != 0) {
		goto uart_to_can_send_unlock_mutex;
	}

	*msg_ptr = can_frame_to_uart_message(frame);

	unsigned int key = irq_lock();
	err = send_uart_internal_no_blocking(uart_to_can_dev, msg_ptr);
	if (err == 0) {
		err = k_msgq_put(&data->tx_callback_fifo, &callback_msg, K_NO_WAIT);
		__ASSERT(err == 0, "Failed to put callback in queue");
	}
	irq_unlock(key);

	if (err != 0) {
		k_mem_slab_free(&data->can_tx_slab, msg_ptr);
		goto uart_to_can_send_unlock_mutex;
	}
uart_to_can_send_unlock_mutex:
	k_mutex_unlock(&data->inst_mutex);

uart_to_can_send_return:
	return err;
}

static int uart_to_can_stop(const struct device *uart_to_can_dev)
{
	// TODO(Matthew):
	const struct uart_to_can_config *config =
		(const struct uart_to_can_config *)uart_to_can_dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = (struct uart_to_can_data *)uart_to_can_dev->data;
	int err;

	struct uart_message *temp_msg_ptr;
	if (!data->common.started) {
		err = -ENODEV;
		goto uart_to_can_stop_return;
	}

	struct tx_callback_ctx callback_msg;
	err = k_mutex_lock(&data->inst_mutex, K_MSEC(10000));
	if (err != 0) {
		LOG_ERR("Failed to lock the mutex in uart_to_can_stop");
		goto uart_to_can_stop_return;
	}

	const char *msg = "C\r";
	int msg_len = strlen(msg);
	err = uart_to_can_send_serial_synchronous(uart_to_can_dev, msg, msg_len, K_MSEC(100));

	if (err == 0) {
		(void)uart_to_can_rx_disable(uart_to_can_dev);
		if (IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
			(void)uart_tx_abort(uart_dev);
			if (data->current_msg != NULL) {
				k_mem_slab_free(&data->can_tx_slab, (void *)data->current_msg);
				data->current_msg = NULL;
			}
		} else {
			uart_irq_tx_disable(uart_dev);
		}

		while (k_msgq_get(&data->tx_callback_fifo, &callback_msg, K_NO_WAIT) == 0) {
			callback_msg.callback(uart_to_can_dev, -ENETDOWN, callback_msg.user_data);
		}

		uart_to_can_serial_rx_fifo_drain(uart_to_can_dev);
		while (k_msgq_get(&data->can_tx_mail_box, &temp_msg_ptr, K_NO_WAIT) == 0) {
			k_mem_slab_free(&data->can_tx_slab, temp_msg_ptr);
			// TODO
		}
		data->common.started = false;
	}
	k_mutex_unlock(&data->inst_mutex);
uart_to_can_stop_return:
	return 0;
}
static int uart_to_can_init(const struct device *dev)
{
	const struct uart_to_can_config *config = dev->config;
	const struct device *uart_dev = config->uart_dev;
	struct uart_to_can_data *data = dev->data;
	int err;

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device %s not ready", uart_dev->name);
		return -ENODEV;
	}
	ring_buf_init(&data->rx_ring_buffer, sizeof(data->uart_buf_recv), data->uart_buf_recv);
	// ring_buf_init(&data->tx_ring_buffer, 1024, data->uart_buf_send);
	// data->is_synchronous_mode = false;

	for (int filter_id = 0; filter_id < 3; filter_id++) {
		data->rx_cb[filter_id].callback = NULL;
		data->rx_cb[filter_id].user_data = NULL;
	}

	k_msgq_init(&data->tx_callback_fifo, data->tx_callback_fifo_buffer,
		    sizeof(struct tx_callback_ctx), UART_TO_CAN_NO_MAIL_BOX);

	k_msgq_init(&data->can_tx_mail_box, data->can_tx_mail_box_buffer,
		    sizeof(struct uart_message *), UART_TO_CAN_NO_MAIL_BOX);
	k_sem_init(&data->sem, 0, 1);

	k_mem_slab_init(&data->can_tx_slab, &data->can_tx_slab_buffer, sizeof(struct uart_message),
			UART_TO_CAN_NO_MAIL_BOX);
	k_mutex_init(&data->inst_mutex);
	data->common.started = false;
	data->current_msg = NULL;
	data->current_msg_idx = 0;
	data->next_async_rx_buf_idx = 0;
	data->async_rx_enabled = false;

	if (IS_ENABLED(CONFIG_CAN_UART_TO_CAN_ASYNC_API)) {
		err = uart_callback_set(uart_dev, uart_cb_async_handler, (void *)dev);
	} else {
		err = uart_irq_callback_user_data_set(uart_dev, uart_cb_handler, (void *)dev);
	}

	if (err != 0) {
		LOG_ERR("Failed to set UART callback (err %d)", err);
		return err;
	}

	err = uart_to_can_reset(dev);
	err = uart_to_can_reset(dev);
	if (err != 0) {
		LOG_ERR("Error resetting CAN controller (err %d)", err);
		return -ENXIO;
	}

	LOG_INF("UART to CAN driver initialized");

	return 0;
}

static int uart_to_can_set_mode(const struct device *dev, can_mode_t mode)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(mode);
	LOG_WRN("CAN mode not supported");
	return 0;
}

static int uart_to_can_set_timing(const struct device *dev, const struct can_timing *timing)
{

	ARG_UNUSED(dev);
	ARG_UNUSED(timing);
	LOG_WRN("CAN timing not supported");
	return 0;
}

static int uart_to_can_get_capabilities(const struct device *dev, uint32_t *caps)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(caps);
	LOG_WRN("CAN capabilities not supported");
	return 0;
}

static int can_uart_to_can_get_state(const struct device *dev, enum can_state *state,
				     struct can_bus_err_cnt *err_cnt)
{
	/* Just return active */
	ARG_UNUSED(dev);
	ARG_UNUSED(state);
	ARG_UNUSED(err_cnt);
	LOG_WRN("CAN state not supported");
	return 0;
}

static void uart_to_can_set_state_change_callback(const struct device *dev,
						  can_state_change_callback_t callback,
						  void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(callback);
	ARG_UNUSED(user_data);
	LOG_WRN("CAN state change callback not supported");
	return;
}

static int uart_to_can_get_core_clock(const struct device *dev, uint32_t *clk)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clk);
	LOG_WRN("CAN core clock not supported");
	return 0;
}

static int uart_to_can_get_max_filters(const struct device *dev, bool ext_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ext_id);
	LOG_WRN("CAN max filters not supported");
	return 0;
}

static DEVICE_API(can, uart_to_can_driver_api) = {
	.start = uart_to_can_start,
	.stop = uart_to_can_stop,
	.get_capabilities = uart_to_can_get_capabilities,
	.set_mode = uart_to_can_set_mode,
	.set_timing = uart_to_can_set_timing,
	.send = uart_to_can_send,
	.add_rx_filter = uart_to_can_add_rx_filter,
	.remove_rx_filter = uart_to_can_remove_rx_filter,
	.get_state = can_uart_to_can_get_state,
	// #ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
	// 	.recover = fake_can_recover,
	// #endif /* CONFIG_CAN_MANUAL_RECOVERY_MODE */
	.set_state_change_callback = uart_to_can_set_state_change_callback,
	.get_core_clock = uart_to_can_get_core_clock,
	.get_max_filters = uart_to_can_get_max_filters,
	/* Recommended configuration ranges from CiA 601-2 */
	// 	.timing_min = {
	// 		.sjw = 1,
	// 		.prop_seg = 0,
	// 		.phase_seg1 = 2,
	// 		.phase_seg2 = 2,
	// 		.prescaler = 1
	// 	},
	// 	.timing_max = {
	// 		.sjw = 128,
	// 		.prop_seg = 0,
	// 		.phase_seg1 = 256,
	// 		.phase_seg2 = 128,
	// 		.prescaler = 32
	// 	},
	// #ifdef CONFIG_CAN_FD_MODE
	// 	.set_timing_data = fake_can_set_timing_data,
	// 	/* Recommended configuration ranges from CiA 601-2 */
	// 	.timing_data_min = {
	// 		.sjw = 1,
	// 		.prop_seg = 0,
	// 		.phase_seg1 = 1,
	// 		.phase_seg2 = 1,
	// 		.prescaler = 1
	// 	},
	// 	.timing_data_max = {
	// 		.sjw = 16,
	// 		.prop_seg = 0,
	// 		.phase_seg1 = 32,
	// 		.phase_seg2 = 16,
	// 		.prescaler = 32
	// 	},
	// #endif /* CONFIG_CAN_FD_MODE */
};

#define UART_TO_CAN_DEVICE_DT_DEFINE(inst)                                                         \
	static const struct uart_to_can_config uart_to_can_config##inst = {                        \
		.uart_dev = DEVICE_DT_GET(DT_INST_PROP(inst, uart_device)),                        \
	};                                                                                         \
                                                                                                   \
	static struct uart_to_can_data uart_to_can_data##inst = {                                  \
		.bitrate = DT_INST_PROP_OR(inst, bitrate, CONFIG_CAN_DEFAULT_BITRATE),             \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_DEFINE(DT_DRV_INST(inst), uart_to_can_init, NULL, &uart_to_can_data##inst,       \
			 &uart_to_can_config##inst, POST_KERNEL, CONFIG_CAN_INIT_PRIORITY,         \
			 &uart_to_can_driver_api);

DT_INST_FOREACH_STATUS_OKAY(UART_TO_CAN_DEVICE_DT_DEFINE);
