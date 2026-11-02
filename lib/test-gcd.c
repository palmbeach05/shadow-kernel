#include <linux/errno.h>
#include <linux/gcd.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init test_gcd_init(void)
{
	static const struct {
		unsigned long a, b, expected;
	} tests[] = {
		{ 0, 0, 0 },
		{ 0, 42, 42 },
		{ 42, 0, 42 },
		{ 48, 18, 6 },
		{ 18, 48, 6 },
		{ 17, 13, 1 },
		{ 42, 42, 42 },
		{ ~0UL, 0, ~0UL },
		{ 0, ~0UL, ~0UL },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		unsigned long result = gcd(tests[i].a, tests[i].b);

		if (WARN(result != tests[i].expected,
			 "gcd(%lu, %lu): expected %lu, got %lu\n",
			 tests[i].a, tests[i].b, tests[i].expected, result))
			return -EINVAL;
	}
	return 0;
}

static void __exit test_gcd_exit(void)
{
}

module_init(test_gcd_init);
module_exit(test_gcd_exit);
MODULE_LICENSE("GPL");
