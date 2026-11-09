// SPDX-License-Identifier: GPL-2.0-only
/* External GPL test module against the matching full-test kernel. The exported
 * alg_test prototype is crypto/internal.h in Linux 7.3.1. Re-run the complete
 * kernel tests after RNG use and mixed API traffic; do not mark algorithms tested
 * or change the test manager's policy or fuzz iteration count.
 */
#include <linux/module.h>
#include <linux/crypto.h>

extern int alg_test(const char *driver, const char *alg, u32 type, u32 mask);

static const struct {
	const char *driver, *name;
	u32 type;
} algorithms[] = {
	{ "md5-rk3128", "md5", CRYPTO_ALG_TYPE_AHASH },
	{ "sha1-rk3128", "sha1", CRYPTO_ALG_TYPE_AHASH },
	{ "sha256-rk3128", "sha256", CRYPTO_ALG_TYPE_AHASH },
	{ "ecb-aes-rk3128", "ecb(aes)", CRYPTO_ALG_TYPE_SKCIPHER },
	{ "cbc-aes-rk3128", "cbc(aes)", CRYPTO_ALG_TYPE_SKCIPHER },
	{ "ctr-aes-rk3128", "ctr(aes)", CRYPTO_ALG_TYPE_SKCIPHER },
	{ "ecb-des-rk3128", "ecb(des)", CRYPTO_ALG_TYPE_SKCIPHER },
	{ "cbc-des-rk3128", "cbc(des)", CRYPTO_ALG_TYPE_SKCIPHER },
	{ "ecb-des3-ede-rk3128", "ecb(des3_ede)", CRYPTO_ALG_TYPE_SKCIPHER },
	{ "cbc-des3-ede-rk3128", "cbc(des3_ede)", CRYPTO_ALG_TYPE_SKCIPHER },
};

static int __init transition_test_init(void)
{
	unsigned int i;
	int err;

	if (!IS_ENABLED(CONFIG_CRYPTO_SELFTESTS_FULL))
		return -EPERM;
	for (i = 0; i < ARRAY_SIZE(algorithms); i++) {
		pr_emerg("crypto764: full self-test START %s\n", algorithms[i].driver);
		err = alg_test(algorithms[i].driver, algorithms[i].name,
			       algorithms[i].type, CRYPTO_ALG_TYPE_MASK);
		if (err) {
			pr_err("crypto764: post-RNG self-test %s FAILED %d\n",
			       algorithms[i].driver, err);
			return err;
		}
		pr_info("crypto764: post-RNG self-test %s PASS\n", algorithms[i].driver);
	}
	return 0;
}

static void __exit transition_test_exit(void) {}
module_init(transition_test_init);
module_exit(transition_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Finite RK3128 post-RNG full kernel self-tests");
