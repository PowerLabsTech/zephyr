#ifndef _UART_TO_CAN_H_
#define _UART_TO_CAN_H_

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>

/** Base in which the message are encoded */
#define MESSAGE_BASE           16
/** Character used to signify success */
#define COMMAND_RESPONSE_OKAY  "\r"
/** Character used to signify failure */
#define COMMAND_RESPONSE_ERROR "\a"

#define CAN_ID_11_BIT_BYTE_LENGHT     3
#define CAN_ID_29_BIT_BYTE_LENGHT     8
#define UART_TO_CAN_ASYNC_RX_BUF_SIZE 32
#define UART_TO_CAN_FILTER_COUNT      19
/** @brief Size of the buffer needed to receive a message over UART.
 * 1 for \S
 * 2 for filter id
 * 1 for dlc
 * 8 * 2for data
 * 1 for \r
 */
#define MAX_UART_CAN_FRAME            (1 + CAN_ID_29_BIT_BYTE_LENGHT + 2 + 1 + CAN_MAX_DLEN * 2 + 1)

struct uart_message {
	size_t buffer_size;
	uint8_t buffer[MAX_UART_CAN_FRAME];
};

/**
 * @brief Configuration for the UART to CAN driver
 *
 */
struct uart_to_can_config {
	/** Pointer to the uart device */
	const struct device *uart_dev;
};

/**
 * @brief CAN reception context
 */
struct can_rx_ctx {
	/** Callback used to notify the user of a received message */
	can_rx_callback_t callback;
	/** User data to be passed to the callback */
	void *user_data;
};

/**
 * @brief Defines application defined call back function for when a CAN message has been sent.
 * @param dev Pointer to the CAN device
 * @param error Error code
 * @param user_data User data
 */
typedef void (*can_tx_default_cb)(const struct device *dev, int error, void *user_data);

/**
 * @brief Send callback message structure
 */
struct tx_callback_ctx {
	/** Callback used to notify the user of a sent message */
	can_tx_default_cb callback;
	/** User data to be passed to the callback */
	void *user_data;
};

#define UART_TO_CAN_NO_MAIL_BOX 3

/**
 * @brief UART to CAN driver data
 */
struct uart_to_can_data {
	/** Bitrate of the CAN bus */
	uint32_t bitrate;
	/** Ring buffer to hold received characters  */
	struct ring_buf rx_ring_buffer;
	/** Storage of received ring buffer */
	uint8_t uart_buf_recv[MAX_UART_CAN_FRAME * CONFIG_CAN_UART_TO_CAN_RX_BUFFER_COUNT];
	/** Array of CAN reception contexts */
	struct can_rx_ctx rx_cb[UART_TO_CAN_FILTER_COUNT];
	/** Tx callback message queue */
	struct k_msgq tx_callback_fifo;
	/** Storage for the Tx callback message queue */
	char tx_callback_fifo_buffer[UART_TO_CAN_NO_MAIL_BOX * sizeof(struct tx_callback_ctx)];

	/** Message queue used to queue up messages to be sent over CAN.*/
	struct k_msgq can_tx_mail_box;
	/** Storage for the Tx callback message queue */
	char can_tx_mail_box_buffer[UART_TO_CAN_NO_MAIL_BOX * sizeof(struct uart_message *)];

	/** Memory slab used to allocate memory for CAN messages.*/
	struct k_mem_slab can_tx_slab;
	/** Storage for the CAN messages */
	char __aligned(4) can_tx_slab_buffer[UART_TO_CAN_NO_MAIL_BOX * sizeof(struct uart_message)];

	/** Current message being processed */
	struct uart_message *current_msg;
	/** Index of the current message */
	int current_msg_idx;
	/** UART async next RX buffer index */
	uint8_t next_async_rx_buf_idx;
	/** UART async RX enabled state */
	bool async_rx_enabled;
	/** UART async double buffer storage */
	uint8_t uart_async_rx_buf[2][UART_TO_CAN_ASYNC_RX_BUF_SIZE];

	/** Semaphore event used to signal when a message response has been received with the
	 * response return value. */
	struct k_sem sem;
	int8_t response;

	struct k_mutex inst_mutex;

	struct can_driver_data common;
	struct k_work rx_work;
	const struct device *dev;
};

// /** Reset the external UART to CAN device.
//  * @param dev Pointer to the UART to CAN device
//  * @return 0 on success, -1 on failure
//  */
// int uart_to_can_reset(const struct device *uart_to_can_dev);

#endif /* _UART_TO_CAN_H_ */
