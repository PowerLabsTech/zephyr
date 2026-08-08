#ifndef _ISOTP_THREAD_CONTEXT_H_
#define _ISOTP_THREAD_CONTEXT_H_

#include <zephyr/kernel.h>
#include <zephyr/canbus/isotp.h>



struct isotp_runtime_context_queued_send {
	size_t len;
	uint8_t data[CONFIG_ISOTP_RUNTIME_CONTEXT_MAX_SEND_LEN];
};
struct isotp_runtime_context;

typedef void (*custom_isotp_receive_callback)(struct isotp_runtime_context *isotp_runtime_ctx,
					      const uint8_t *data, size_t len);
struct isotp_runtime_context {
	struct k_msgq send_msgq;
	struct isotp_runtime_context_queued_send send_msgq_buffer[CONFIG_ISOTP_RUNTIME_CONTEXT_QUEUE_SIZE];

	struct k_mutex recv_mutex;
	struct k_work work;
	struct k_condvar recv_condvar;

	struct isotp_send_ctx send_ctx;
	struct isotp_recv_ctx recv_ctx_0_5;

	struct isotp_runtime_context_queued_send current_send;
	int last_error_nr;
	bool send_in_progress;
	bool is_recv_bound;
	bool enable_recv;

	bool initialized;

	const struct device *can_dev;
	struct isotp_msg_id rx_addr;
	struct isotp_msg_id tx_addr;
	struct isotp_fc_opts fc_opts_0_5;


};

/** @brief Initializes the ISO-TP runtime context.
 * @param isotp_runtime_ctx Pointer to the ISO-TP runtime context structure.
 * @param can_dev Pointer to the CAN device structure.
 * @param rx_addr Pointer to the receive address structure.
 * @param tx_addr Pointer to the transmit address structure.
 * @param fc_opts_0_5 Pointer to the flow control options structure.
 * @param enable_recv Boolean flag to enable or disable receiving isotp data.
 * @return 0 on success, or a negative error code on failure.
 *
 * This function initializes the ISO-TP runtime context by setting up the receive and transmit
 * addresses, flow control options, and other necessary structures. It also initializes the mutex
 * and condition variable used for synchronization in the receive work handler. The function
 * prepares the context for sending and receiving data using the ISO-TP protocol.
 */
int isotp_runtime_context_init(struct isotp_runtime_context *isotp_runtime_ctx,
			       const struct device *can_dev, struct isotp_msg_id *rx_addr,
			       struct isotp_msg_id *tx_addr, struct isotp_fc_opts *fc_opts_0_5,
			       bool enable_recv);

/** @brief Sends data to the subcontroller using ISO-TP protocol.
 * @param state Pointer to the subcontroller state structure.
 * @param data Pointer to the data buffer to be sent.
 * @param data_len Length of the data to be sent.
 * @param timeout Timeout for sending data.
 * @return 0 on success, or a negative error code on failure.
 *
 * This function works by queuing the data to be sent in the send message queue and then submitting
 * the send work handler. It ensures that the ISO-TP runtime context is initialized before
 * attempting to send data. If the send operation fails, it logs an error and returns a negative
 * error code. The function returns 0 on success or a negative error code on failure.
 */
int isotp_runtime_context_send(struct isotp_runtime_context *isotp_runtime_ctx,
			       const uint8_t data[], size_t data_len, k_timeout_t timeout);

/** @brief Receives data from the subcontroller using ISO-TP protocol.
 * @param isotp_runtime_ctx Pointer to the ISO-TP runtime context structure.
 * @param data Pointer to the buffer where received data will be stored.
 * @param buf_len Length of the buffer provided for receiving data.
 * @param timeout Timeout for receiving data.
 * @return 0 on success, or a negative error code on failure.
 *
 * This function attempts to receive data from the subcontroller using the ISO-TP protocol. It waits
 * for incoming data and stores it in the provided buffer. If the receive operation fails or times
 * out, it returns a negative error code. The function returns 0 on success or a negative error code
 * on failure.
 */
int isotp_runtime_context_recv(struct isotp_runtime_context *isotp_runtime_ctx, uint8_t *data,
			       size_t buf_len, k_timeout_t timeout);
#endif /* _ISOTP_THREAD_CONTEXT_H_ */
