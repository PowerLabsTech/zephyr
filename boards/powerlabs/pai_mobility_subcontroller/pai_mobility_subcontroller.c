#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>

#ifdef CONFIG_ENABLE_PAI_MOBILITY_SUBCONTROLLER_RGB_LED
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pai_mobility_subcontroller);

static int startup_enable_rgb_led(void)
{
    const struct gpio_dt_spec enable_led = GPIO_DT_SPEC_GET(DT_NODELABEL(enable_rgb_led), gpios);
	int ret;
    // Your startup logic goes here
    LOG_DBG("Custom startup code running automatically!\n");
    if (!gpio_is_ready_dt(&enable_led))
	{
		return 0;
	}

	ret = gpio_pin_configure_dt(&enable_led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0)
	{
		return 0;
	}
	ret = gpio_pin_set_dt(&enable_led, 1);
	if (ret < 0)
	{
		return 0;
	}
    return 0; // Return 0 on successful initialization
}

/* Register the function to run at startup */
SYS_INIT(startup_enable_rgb_led, APPLICATION, CONFIG_ENABLE_PAI_MOBILITY_SUBCONTROLLER_RGB_LED_PRIORITY);
#endif
