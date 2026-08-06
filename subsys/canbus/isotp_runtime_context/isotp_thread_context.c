#include <zephyr/canbus/isotp_thread_context.h>

#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(isotp_thread_context, CONFIG_ISOTP_RUNTIME_CONTEXT_LOG_LEVEL);

static void start_isotp_recv_work_handler(struct k_work *work)
{
	// Try starting the ISO-TP receive work handler
	// If it fails, add to the systemq for a retry later
	struct isotp_runtime_context *isotp_runtime_ctx =
		CONTAINER_OF(work, struct isotp_runtime_context, start_recv_work);
	int err;
	bool readd_to_queue = false;
	err = k_mutex_lock(&isotp_runtime_ctx->recv_mutex, K_NO_WAIT);
	if (err != 0) {
		readd_to_queue = true;
		LOG_ERR("Failed to lock mutex for isotp_state");
		goto start_isotp_recv_work_handler_start_work;
	}

	if (isotp_runtime_ctx->is_recv_bound) {
		LOG_DBG("ISO-TP receive already bound");
		goto start_isotp_recv_work_handler_unlock_mutex;
	}

	err = isotp_bind(&isotp_runtime_ctx->recv_ctx_0_5, isotp_runtime_ctx->can_dev,
			 &isotp_runtime_ctx->rx_addr, &isotp_runtime_ctx->tx_addr,
			 &isotp_runtime_ctx->fc_opts_0_5, K_NO_WAIT);
	if (err != ISOTP_N_OK) {
		readd_to_queue = true;
		LOG_ERR("Failed to bind to rx ID 0x%x [%d]\n", isotp_runtime_ctx->rx_addr.ext_id,
			err);
		goto start_isotp_recv_work_handler_unlock_mutex;
	}
	isotp_runtime_ctx->is_recv_bound = true;
	k_condvar_signal(&isotp_runtime_ctx->recv_condvar);

start_isotp_recv_work_handler_unlock_mutex:
	err = k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);
	__ASSERT(err == 0, "Failed to unlock mutex for isotp_state recv_mutex");

start_isotp_recv_work_handler_start_work:
	if (readd_to_queue) {
		err = k_work_submit(&isotp_runtime_ctx->start_recv_work);
		__ASSERT(err >= 0, "Failed to queue ISO-TP receive work");
	}

	return;
}

static void subcontroller_send_work_handler(struct k_work *work);

static int stop_isotp_recv(struct isotp_runtime_context *isotp_runtime_ctx)
{

	int err = k_mutex_lock(&isotp_runtime_ctx->recv_mutex, K_NO_WAIT);
	if (err != 0) {
		LOG_ERR("Failed to lock mutex for isotp_state recv_mutex");
		goto stop_isotp_recv_work_handler_return;
	}
	if (!isotp_runtime_ctx->is_recv_bound) {
		LOG_DBG("ISO-TP receive not bound");
		goto stop_isotp_recv_work_handler_unlock_mutex;
	}

	isotp_unbind(&isotp_runtime_ctx->recv_ctx_0_5);
	isotp_runtime_ctx->is_recv_bound = false;
stop_isotp_recv_work_handler_unlock_mutex:
	err = k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);
	__ASSERT(err == 0, "Failed to unlock mutex for isotp_state recv_mutex");
stop_isotp_recv_work_handler_return:
	return err;
}

void isotp_runtime_context_start_recv(struct isotp_runtime_context *isotp_runtime_ctx)
{
	isotp_runtime_ctx->last_error_nr = ISOTP_N_OK;
	isotp_runtime_ctx->current_send.len = 0;
	int err = k_work_submit(&isotp_runtime_ctx->start_recv_work);
	__ASSERT(err >= 0, "Failed to queue ISO-TP receive work");
	(void)err;
}

static void isotp_send_complete_cb(int error_nr, void *arg)
{
	struct isotp_runtime_context *isotp_runtime_ctx = (struct isotp_runtime_context *)arg;
	if (error_nr != ISOTP_N_OK) {
		LOG_ERR("TX failed with error [%d]\n Resending", error_nr);
	} else {
		LOG_INF("TX completed successfully");
	}
	isotp_runtime_ctx->last_error_nr = error_nr;

	if (error_nr != ISOTP_N_OK || k_msgq_num_used_get(&isotp_runtime_ctx->send_msgq) > 0) {
		int err = k_work_submit(&isotp_runtime_ctx->send_work);
		__ASSERT(err >= 0, "Failed to queue subcontroller send work");
		ARG_UNUSED(err);
	} else {
		if (isotp_runtime_ctx->enable_recv) {
			isotp_runtime_context_start_recv(isotp_runtime_ctx);
		}
	}
	LOG_DBG("TX complete work handler");
}

static void subcontroller_send_work_handler(struct k_work *work)
{
	struct isotp_runtime_context *isotp_runtime_ctx =
		CONTAINER_OF(work, struct isotp_runtime_context, send_work);
	int err;
	bool data_available_to_send = false;

	if (isotp_runtime_ctx->current_send.len != 0 &&
	    isotp_runtime_ctx->last_error_nr != ISOTP_N_OK) {
		data_available_to_send = true;
		// LOG_WRN("Resending last message for subcontroller ID 0x%02X due to error [%d]",
		//         CONTAINER_OF(isotp_runtime_ctx, struct subcontroller_state,
		//         isotp_runtime_ctx)->id, isotp_runtime_ctx->last_error_nr);
	} else if (k_msgq_get(&isotp_runtime_ctx->send_msgq, &isotp_runtime_ctx->current_send,
			      K_NO_WAIT) == 0) {
		data_available_to_send = true;
	}

	if (!data_available_to_send) {
		LOG_DBG("No more messages to send for isotp context %p", isotp_runtime_ctx);
		goto subcontroller_send_work_handler_return;
	}

	err = stop_isotp_recv(isotp_runtime_ctx);
	if (err != 0) {
		LOG_ERR("Failed to stop ISO-TP receive work handler");
		isotp_runtime_ctx->last_error_nr = err;
		goto subcontroller_send_work_handler_work_submit;
	}

	err = isotp_send(&isotp_runtime_ctx->send_ctx, isotp_runtime_ctx->can_dev,
			 isotp_runtime_ctx->current_send.data, isotp_runtime_ctx->current_send.len,
			 &isotp_runtime_ctx->tx_addr, &isotp_runtime_ctx->rx_addr,
			 isotp_send_complete_cb, isotp_runtime_ctx);
	if (err != ISOTP_N_OK) {
		LOG_ERR("Error while sending data to ID 0x%x [%d]\n",
			isotp_runtime_ctx->tx_addr.ext_id, err);
		isotp_runtime_ctx->last_error_nr = err;
		goto subcontroller_send_work_handler_work_submit;
	} else {
		goto subcontroller_send_work_handler_return;
	}
subcontroller_send_work_handler_work_submit:
	err = k_work_submit(work);
	__ASSERT(err >= 0, "Failed to queue subcontroller send work");
subcontroller_send_work_handler_return:
	return;
}

int isotp_runtime_context_init(struct isotp_runtime_context *isotp_runtime_ctx,
			   const struct device *can_dev, struct isotp_msg_id *rx_addr,
			   struct isotp_msg_id *tx_addr, struct isotp_fc_opts *fc_opts_0_5,
			   bool enable_recv)
{
	int err;

	if (!isotp_runtime_ctx || !rx_addr || !tx_addr || !fc_opts_0_5) {
		LOG_ERR("Invalid parameters for initializing ISO-TP runtime context");
		return -EINVAL;
	}

	if (isotp_runtime_ctx->initialized) {
		LOG_WRN("ISO-TP runtime context already initialized for subcontroller ID 0x%02X",
			rx_addr->ext_id);
		return -EALREADY;
	}

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device is not ready for ISO-TP runtime context initialization");
		return -ENODEV;
	}

	isotp_runtime_ctx->can_dev = can_dev;
	isotp_runtime_ctx->rx_addr = *rx_addr;
	isotp_runtime_ctx->tx_addr = *tx_addr;
	isotp_runtime_ctx->fc_opts_0_5 = *fc_opts_0_5;
	isotp_runtime_ctx->is_recv_bound = false;
	err = k_mutex_init(&isotp_runtime_ctx->recv_mutex);
	__ASSERT(err == 0, "Failed to initialize mutex for isotp_state recv_mutex");
	err = k_condvar_init(&isotp_runtime_ctx->recv_condvar);
	__ASSERT(err == 0, "Failed to initialize condvar for isotp_state recv_condvar");
	(void)err;
	k_work_init(&isotp_runtime_ctx->start_recv_work, start_isotp_recv_work_handler);

	isotp_runtime_ctx->current_send.len = 0;
	isotp_runtime_ctx->last_error_nr = ISOTP_N_OK;
	k_msgq_init(&isotp_runtime_ctx->send_msgq, (char *)isotp_runtime_ctx->send_msgq_buffer,
		    sizeof(struct isotp_runtime_context_queued_send),
		    SUBCONTROLLER_SEND_QUEUE_DEPTH);
	k_work_init(&isotp_runtime_ctx->send_work, subcontroller_send_work_handler);
	isotp_runtime_ctx->enable_recv = enable_recv;
	if (isotp_runtime_ctx->enable_recv) {
		isotp_runtime_context_start_recv(isotp_runtime_ctx);
	}

	isotp_runtime_ctx->initialized = true;

	return 0;
}

/** @brief Sends data to the subcontroller using ISO-TP protocol.
 * @param state Pointer to the subcontroller state structure.
 * @param data Pointer to the data buffer to be sent.
 * @param data_len Length of the data to be sent.
 * @return 0 on success, or a negative error code on failure.
 *
 */
int isotp_runtime_context_send(struct isotp_runtime_context *isotp_runtime_ctx,
			       const uint8_t data[], size_t data_len)
{
	struct isotp_runtime_context_queued_send queued_send;
	int err;

	if (!isotp_runtime_ctx->initialized) {
		LOG_ERR("Send runtime not initialized for subcontroller ID 0x%02X",
			isotp_runtime_ctx->rx_addr.ext_id);
		err = -EAGAIN;
		goto send_to_subcontroller_return;
	}

	if (data_len > SUBCONTROLLER_MAX_SEND_LEN) {
		LOG_ERR("Data length exceeds maximum allowed length of %d bytes",
			SUBCONTROLLER_MAX_SEND_LEN);
		err = -EINVAL;
		goto send_to_subcontroller_return;
	}

	memcpy(queued_send.data, data, data_len);
	queued_send.len = data_len;

	err = k_msgq_put(&isotp_runtime_ctx->send_msgq, &queued_send, K_NO_WAIT);
	if (err != 0) {
		LOG_ERR("Failed to queue message for subcontroller ID 0x%02X",
			isotp_runtime_ctx->rx_addr.ext_id);
		goto send_to_subcontroller_return;
	}

	err = k_work_submit(&isotp_runtime_ctx->send_work);
	__ASSERT(err >= 0, "Failed to queue subcontroller send work");
	err = err >= 0 ? 0 : err;
send_to_subcontroller_return:
	return err;
}

int isotp_runtime_context_recv(struct isotp_runtime_context *isotp_runtime_ctx, uint8_t *data,
			       size_t buf_len, k_timeout_t timeout)
{
	int err;
	k_timepoint_t end_time = sys_timepoint_calc(timeout);

	err = k_mutex_lock(&isotp_runtime_ctx->recv_mutex, timeout);
	if (err != 0) {
		LOG_ERR("Failed to lock mutex for isotp_runtime_ctx->recv_mutex");
		goto isotp_runtime_context_recv_return;
	}
	/* block this thread until another thread signals cond. While
	 * blocked, the mutex is released, then re-acquired before this
	 * thread is woken up and the call returns.
	 */
	timeout = sys_timepoint_timeout(end_time);
	while (!isotp_runtime_ctx->is_recv_bound) {
		LOG_DBG("ISO-TP receive not bound, waiting for it to be bound");
		k_condvar_wait(&isotp_runtime_ctx->recv_condvar, &isotp_runtime_ctx->recv_mutex,
			       timeout);
	}

	err = k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);
	__ASSERT(err == 0, "Failed to unlock mutex for isotp_runtime_ctx->recv_mutex");
	timeout = sys_timepoint_timeout(end_time);

	err = isotp_recv(&isotp_runtime_ctx->recv_ctx_0_5, data, buf_len, timeout);
	if (err < 0) {
		LOG_ERR("Receiving error for paimoc  [%d]\n", err);
		goto isotp_runtime_context_recv_return;
	}
isotp_runtime_context_recv_return:
	return err;
}
