#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pai_mobility_subcontroller);

static struct gpio_callback tpl5010_wake_cb_data;
static struct k_work tpl5010_wake_work;

#ifdef CONFIG_ENABLE_PAI_MOBILITY_SUBCONTROLLER_RGB_LED
static int startup_enable_rgb_led(void)
{
	const struct gpio_dt_spec enable_led =
		GPIO_DT_SPEC_GET(DT_NODELABEL(enable_rgb_led), gpios);
	int ret;
	LOG_DBG("Enabling RGB LED at startup!\n");
	if (!gpio_is_ready_dt(&enable_led)) {
		ret = -1;
		goto startup_enable_rgb_led_return;
	}

	ret = gpio_pin_configure_dt(&enable_led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		goto startup_enable_rgb_led_return;
	}
	ret = gpio_pin_set_dt(&enable_led, 1);
	if (ret < 0) {
		goto startup_enable_rgb_led_return;
	}
startup_enable_rgb_led_return:
	return ret;
}

/* Register the function to run at startup */
SYS_INIT(startup_enable_rgb_led, APPLICATION,
	 CONFIG_ENABLE_PAI_MOBILITY_SUBCONTROLLER_RGB_LED_SETUP_PRIORITY);
#endif

static inline int send_done_to_tpl5010_watchdog(void)
{
	const struct gpio_dt_spec tpl5010_done_pin =
		GPIO_DT_SPEC_GET(DT_NODELABEL(tpl5010_done_pin), gpios);
	int ret;
	if (!gpio_is_ready_dt(&tpl5010_done_pin)) {
		ret = -1;
		goto send_done_to_tpl5010_watchdog_return;
	}
	ret = gpio_pin_set_dt(&tpl5010_done_pin, 1);
	if (ret < 0) {
		goto send_done_to_tpl5010_watchdog_return;
	}
	k_busy_wait(1); // using a 1000ns delay because the that is the low resolution that
			// k_busy_wait() can provide so as to help with portability. The tpl5010
			// datasheet specifies a minimum pulse width of 100ns, so this should be
			// sufficient.

	ret = gpio_pin_set_dt(&tpl5010_done_pin, 0);
	if (ret < 0) {
		goto send_done_to_tpl5010_watchdog_return;
	}
send_done_to_tpl5010_watchdog_return:
	return ret;
}

static void tpl5010_wake_work_handler(__maybe_unused struct k_work *work)
{
	// Handle the work here
	int ret = send_done_to_tpl5010_watchdog();
	(void)ret; // Suppress unused variable warning
	__ASSERT(ret == 0, "Failed to send done signal to TPL5010 watchdog");
	LOG_DBG("Handling work for TPL5010 wake event");
}
static void tpl5010_wake_recieved(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_submit(&tpl5010_wake_work);
	// Handle interrupt safely and quickly here 
	LOG_DBG("Button pressed at %u", k_uptime_get_32());
}

static int startup_tpl5010_watchdog(void)
{
	const struct gpio_dt_spec tpl5010_done_pin =
		GPIO_DT_SPEC_GET(DT_NODELABEL(tpl5010_done_pin), gpios);
	int ret;
	// Your startup logic goes here
	LOG_DBG("Custom startup code running automatically!\n");
	if (!gpio_is_ready_dt(&tpl5010_done_pin)) {
		ret = -1;
		goto startup_tpl5010_watchdog_return;
	}

	ret = gpio_pin_configure_dt(&tpl5010_done_pin, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		goto startup_tpl5010_watchdog_return;
	}

	const struct gpio_dt_spec tpl5010_wake_pin =
		GPIO_DT_SPEC_GET(DT_NODELABEL(tpl5010_wake_pin), gpios);
	if (!gpio_is_ready_dt(&tpl5010_wake_pin)) {
		ret = -1;
		goto startup_tpl5010_watchdog_return;
	}
	ret = gpio_pin_configure_dt(&tpl5010_wake_pin, GPIO_INPUT);
	if (ret < 0) {
		goto startup_tpl5010_watchdog_return;
	}

	// Configure the interrupt trigger
	ret = gpio_pin_interrupt_configure_dt(&tpl5010_wake_pin, GPIO_INT_EDGE_RISING);
	if (ret < 0) {
		goto startup_tpl5010_watchdog_return;
	}

	// Initialize and add callback
	gpio_init_callback(&tpl5010_wake_cb_data, tpl5010_wake_recieved, BIT(tpl5010_wake_pin.pin));
	ret = gpio_add_callback(tpl5010_wake_pin.port, &tpl5010_wake_cb_data);
	if (ret < 0) {
		goto startup_tpl5010_watchdog_return;
	}

	// Initialize the work item
	k_work_init(&tpl5010_wake_work, tpl5010_wake_work_handler);

	ret = send_done_to_tpl5010_watchdog();
	if (ret < 0) {
		goto startup_tpl5010_watchdog_return;
	}

startup_tpl5010_watchdog_return:
	return ret;
}

/* Register the function to run at startup */
SYS_INIT(startup_tpl5010_watchdog, APPLICATION, CONFIG_PAI_MOBILITY_SETUP_TPL5010_PRIORITY);
