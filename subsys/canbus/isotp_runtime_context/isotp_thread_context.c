#include <zephyr/canbus/isotp_thread_context.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(isotp_thread_context, CONFIG_ISOTP_RUNTIME_CONTEXT_LOG_LEVEL);

static int start_isotp_recv(struct isotp_runtime_context *isotp_runtime_ctx)
{
	// Try starting the ISO-TP receive work handler
	// If it fails, add to the systemq for a retry later
	int err;
	k_mutex_lock(&isotp_runtime_ctx->recv_mutex, K_FOREVER);

	if (isotp_runtime_ctx->is_recv_bound) {
		LOG_DBG("context already bounded for %x",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		err = 0;
		goto start_isotp_recv_unlock_mutex;
	}

	err = isotp_bind(&isotp_runtime_ctx->recv_ctx_0_5, isotp_runtime_ctx->can_dev,
			 &isotp_runtime_ctx->rx_addr, &isotp_runtime_ctx->tx_addr,
			 &isotp_runtime_ctx->fc_opts_0_5, K_NO_WAIT);
	if (err != ISOTP_N_OK) {
		LOG_ERR("Failed to bind to rx ID 0x%x [%d]\n",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id,
			err);
		goto start_isotp_recv_unlock_mutex;
	}
	LOG_DBG("ISO-TP receive bound to rx ID 0x%x",
		isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
			? isotp_runtime_ctx->rx_addr.ext_id
			: isotp_runtime_ctx->rx_addr.std_id);
	isotp_runtime_ctx->is_recv_bound = true;
	k_condvar_signal(&isotp_runtime_ctx->recv_condvar);

start_isotp_recv_unlock_mutex:
	k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);

	return err;
}

static int stop_isotp_recv(struct isotp_runtime_context *isotp_runtime_ctx)
{

	int err = k_mutex_lock(&isotp_runtime_ctx->recv_mutex, K_FOREVER);
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
	k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);
stop_isotp_recv_work_handler_return:
	return err;
}

// There is no need for a lock key because the isr can only change the send_in_progress flag to
// false, and the work handler can only send it to true. The isr will only be called when the
// send_in_progress is true and the work handler will never go through the write path of the
// send_in_progress flag when it is true. The only time the work handler will go through the write
// path of the send_in_progress flag is when it is false, and the isr will never be called when it
// is false. Therefore, there is no need for a lock key.
// Similar logic applies for the timer isr.

static void isotp_send_complete_cb(int error_nr, void *arg)
{
	struct isotp_runtime_context *isotp_runtime_ctx = (struct isotp_runtime_context *)arg;
	if (error_nr != ISOTP_N_OK) {
		LOG_ERR("TX failed with error %d [%s] for len %d", error_nr, strerror(-error_nr),
			isotp_runtime_ctx->current_send.len);
	} else {
		LOG_INF("TX completed successfully for len %d",
			isotp_runtime_ctx->current_send.len);
	}
	isotp_runtime_ctx->last_error_state_nr = error_nr;
	isotp_runtime_ctx->send_in_progress = false;

	int err = k_work_submit(&isotp_runtime_ctx->work);
	__ASSERT(err >= 0, "Failed to queue subcontroller send work");
	ARG_UNUSED(err);
}

static void isotp_send_retry_timer_handler(struct k_timer *timer)
{
	struct isotp_runtime_context *isotp_runtime_ctx =
		CONTAINER_OF(timer, struct isotp_runtime_context, send_retry_timer);
	LOG_DBG("Retry timer expired for isotp runtime context of rx.id 0x%X",
		isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
			? isotp_runtime_ctx->rx_addr.ext_id
			: isotp_runtime_ctx->rx_addr.std_id);
	isotp_runtime_ctx->last_error_state_nr = ISOTP_CONTEXT_RUNTIME_RETRY_READY;

	int err = k_work_submit(&isotp_runtime_ctx->work);
	__ASSERT(err >= 0, "Failed to queue subcontroller send work");
	ARG_UNUSED(err);
}
static void isotp_runtime_context_work_handler(struct k_work *work)
{
	struct isotp_runtime_context *isotp_runtime_ctx =
		CONTAINER_OF(work, struct isotp_runtime_context, work);
	int err;
	bool start_receiving = false;
	int delay_ms;
	k_mutex_lock(&isotp_runtime_ctx->recv_mutex, K_FOREVER);
	if (isotp_runtime_ctx->send_in_progress) {
		LOG_DBG("Send in progress, waiting for completion callback for isotp runtime "
			"context of rx.id 0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		goto isotp_runtime_context_work_handler_unlock;
	}

	if (k_timer_remaining_get(&isotp_runtime_ctx->send_retry_timer) > 0) {
		LOG_DBG("Retry timer still running for isotp runtime context of rx.id 0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		start_receiving = false;
	} else if (isotp_runtime_ctx->last_error_state_nr == ISOTP_CONTEXT_RUNTIME_RETRY_READY) {
		start_receiving = true;

		LOG_WRN("Resending last message for isotp runtime of rx.id  ID 0x%X after timeout",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
	} else if (isotp_runtime_ctx->last_error_state_nr != ISOTP_N_OK) {

		delay_ms = ((CONFIG_ISOTP_BS_TIMEOUT * 2 + (CONFIG_ISOTP_BS_TIMEOUT >> 2)) +
			    (sys_rand32_get() % (int)(CONFIG_ISOTP_BS_TIMEOUT * 2)));
		k_timer_start(&isotp_runtime_ctx->send_retry_timer, K_MSEC(delay_ms), K_FOREVER);
		start_receiving = false;
		LOG_WRN("Isotp error for isotp runtime of rx.id  sending timer %d for "
			"retryID 0x%X due to error "
			"[%d]",
			delay_ms,
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id,
			isotp_runtime_ctx->last_error_state_nr);
		start_receiving = false;

	} else if (k_msgq_get(&isotp_runtime_ctx->send_msgq, &isotp_runtime_ctx->current_send,
			      K_NO_WAIT) == 0) {
		start_receiving = true;
	}

	if (!start_receiving) {
		LOG_DBG("Starting receive on isotp runtime context of rx.id 0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		err = start_isotp_recv(isotp_runtime_ctx);
		if (err != 0) {
			goto isotp_runtime_context_work_handler_work_submit;
		} else {
			goto isotp_runtime_context_work_handler_unlock;
		}
	}

	err = stop_isotp_recv(isotp_runtime_ctx);
	if (err != 0) {
		LOG_ERR("Failed to stop ISO-TP receive work handler for isotp runtime context of "
			"rx.id 0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		isotp_runtime_ctx->last_error_state_nr = err;
		goto isotp_runtime_context_work_handler_work_submit;
	}

	err = isotp_send(&isotp_runtime_ctx->send_ctx, isotp_runtime_ctx->can_dev,
			 isotp_runtime_ctx->current_send.data, isotp_runtime_ctx->current_send.len,
			 &isotp_runtime_ctx->tx_addr, &isotp_runtime_ctx->rx_addr,
			 isotp_send_complete_cb, isotp_runtime_ctx);
	if (err != ISOTP_N_OK) {
		LOG_ERR("Error while sending data to ID 0x%x [%d]\n",
			isotp_runtime_ctx->tx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->tx_addr.ext_id
				: isotp_runtime_ctx->tx_addr.std_id,
			err);
		isotp_runtime_ctx->last_error_state_nr = err;
		goto isotp_runtime_context_work_handler_work_submit;
	} else {
		isotp_runtime_ctx->send_in_progress = true;
		goto isotp_runtime_context_work_handler_unlock;
	}
isotp_runtime_context_work_handler_work_submit:
	err = k_work_submit(work);
	__ASSERT(err >= 0, "Failed to queue subcontroller send work");
isotp_runtime_context_work_handler_unlock:
	k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);
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

	err = k_mutex_init(&isotp_runtime_ctx->recv_mutex);
	__ASSERT(err == 0, "Failed to initialize mutex for isotp_state recv_mutex");

	err = k_mutex_lock(&isotp_runtime_ctx->recv_mutex, K_NO_WAIT);
	if (err != 0) {
		LOG_ERR("Failed to lock mutex for isotp_state recv_mutex");
		goto isotp_runtime_context_init_return;
	}

	isotp_runtime_ctx->can_dev = can_dev;
	isotp_runtime_ctx->rx_addr = *rx_addr;
	isotp_runtime_ctx->tx_addr = *tx_addr;
	isotp_runtime_ctx->fc_opts_0_5 = *fc_opts_0_5;

	isotp_runtime_ctx->is_recv_bound = false;
	isotp_runtime_ctx->current_send.len = 0;
	isotp_runtime_ctx->last_error_state_nr = ISOTP_N_OK;
	isotp_runtime_ctx->enable_recv = enable_recv;
	isotp_runtime_ctx->send_in_progress = false;

	err = k_condvar_init(&isotp_runtime_ctx->recv_condvar);
	__ASSERT(err == 0, "Failed to initialize condvar for isotp_state recv_condvar");
	(void)err;
	k_work_init(&isotp_runtime_ctx->work, isotp_runtime_context_work_handler);
	k_timer_init(&isotp_runtime_ctx->send_retry_timer, isotp_send_retry_timer_handler, NULL);

	k_msgq_init(&isotp_runtime_ctx->send_msgq, (char *)isotp_runtime_ctx->send_msgq_buffer,
		    sizeof(struct isotp_runtime_context_queued_send),
		    CONFIG_ISOTP_RUNTIME_CONTEXT_QUEUE_SIZE);

	err = k_work_submit(&isotp_runtime_ctx->work);
	if (err < 0) {
		LOG_ERR("Failed to submit work for ISO-TP receive context for subcontroller ID "
			"0x%02X",
			isotp_runtime_ctx->rx_addr.ext_id);
		goto isotp_runtime_context_init_unlock;
	}
	err = 0;
	isotp_runtime_ctx->initialized = true;
isotp_runtime_context_init_unlock:
	k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);
isotp_runtime_context_init_return:
	return err;
}

int isotp_runtime_context_send(struct isotp_runtime_context *isotp_runtime_ctx,
			       const uint8_t data[], size_t data_len, k_timeout_t timeout)
{
	struct isotp_runtime_context_queued_send queued_send;
	int err;

	if (!isotp_runtime_ctx || !data || data_len == 0 ||
	    data_len > CONFIG_ISOTP_RUNTIME_CONTEXT_MAX_SEND_LEN) {
		LOG_ERR("Invalid parameters for initializing ISO-TP runtime context 0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		err = -EINVAL;
		goto send_to_subcontroller_return;
	}

	if (!isotp_runtime_ctx->initialized) {
		LOG_ERR("Send runtime not initialized for isotp runtime ID 0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		err = -EAGAIN;
		goto send_to_subcontroller_return;
	}
	memcpy(queued_send.data, data, data_len);
	queued_send.len = data_len;

	err = k_msgq_put(&isotp_runtime_ctx->send_msgq, &queued_send, timeout);
	if (err != 0) {
		LOG_ERR("Failed to queue message for subcontroller ID 0x%02X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		goto send_to_subcontroller_return;
	}

	err = k_work_submit(&isotp_runtime_ctx->work);
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
		LOG_ERR("Failed to lock mutex for isotp_runtime_ctx->recv_mutex for isotp runtime "
			"ID 0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		goto isotp_runtime_context_recv_return;
	}
	/* block this thread until another thread signals cond. While
	 * blocked, the mutex is released, then re-acquired before this
	 * thread is woken up and the call returns.
	 */
	while (!isotp_runtime_ctx->is_recv_bound) {
		timeout = sys_timepoint_timeout(end_time);
		LOG_DBG("ISO-TP receive not bound, waiting for it to be bound for isotp runtime ID "
			"0x%X",
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		err = k_condvar_wait(&isotp_runtime_ctx->recv_condvar,
				     &isotp_runtime_ctx->recv_mutex, timeout);
		if (err != 0) {
			LOG_ERR("Error while waiting for ISO-TP receive to be bound: %d for isotp "
				"runtime ID 0x%X",
				err,
				isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
					? isotp_runtime_ctx->rx_addr.ext_id
					: isotp_runtime_ctx->rx_addr.std_id);
			break;
		}
	}
	k_mutex_unlock(&isotp_runtime_ctx->recv_mutex);
	if (err != 0) {
		goto isotp_runtime_context_recv_return;
	}

	timeout = sys_timepoint_timeout(end_time);

	err = isotp_recv(&isotp_runtime_ctx->recv_ctx_0_5, data, buf_len, timeout);
	if (err < 0) {
		LOG_ERR("Receiving error for paimoc  [%d] for isotp runtime ID 0x%X", err,
			isotp_runtime_ctx->rx_addr.flags & ISOTP_MSG_IDE
				? isotp_runtime_ctx->rx_addr.ext_id
				: isotp_runtime_ctx->rx_addr.std_id);
		goto isotp_runtime_context_recv_return;
	}
isotp_runtime_context_recv_return:
	return err;
}
