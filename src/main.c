#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("\n=== T3-S3 V1.3 boot OK ===\n");
	printk("MCU: ESP32-S3FH4R2\n");
	printk("PSRAM: 2 MB initialized\n");

	uint32_t n = 0;
	while (1) {
		printk("alive %u\n", n++);
		k_sleep(K_SECONDS(2));
	}
	return 0;
}
