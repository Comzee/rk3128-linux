// SPDX-License-Identifier: GPL-2.0-only
/* Bounded hash/cipher interleave through explicit production driver APIs.
 * The default 16 KiB reaches every measured software cutoff. No PM, reset,
 * clock or register access: compare complete digests/ciphertexts/round trips.
 */
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

static unsigned int bytes = 16384;
module_param(bytes, uint, 0400);
static bool sweep;
module_param(sweep, bool, 0400);
static bool cipher_first;
module_param(cipher_first, bool, 0400);
static int only_row = -1;
module_param(only_row, int, 0400);
static unsigned int cipher_bytes;
module_param(cipher_bytes, uint, 0400);
static unsigned int rounds = 8;
module_param(rounds, uint, 0400);
static const struct {
	const char *hardware, *software;
	unsigned int keylen;
} ciphers[] = {
	{ "ecb-aes-rk3128", "ecb-aes-lib", 24 },
	{ "ecb-aes-rk3128", "ecb-aes-lib", 32 },
	{ "ecb-aes-rk3128", "ecb-aes-lib", 16 },
	{ "ecb-des-rk3128", "ecb(des-generic)", 8 },
	{ "ecb-des3-ede-rk3128", "ecb(des3_ede-generic)", 24 },
};
static struct crypto_ahash *hash;
static struct crypto_shash *reference;
static struct ahash_request *hreq;
static struct shash_desc *desc;
static u8 *data, *actual, *expected;
static u8 key[32];

static int hash_check(void)
{
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist src;
	u8 got[33], want[32];
	int err;

	memset(got, 0xa5, sizeof(got));
	sg_init_one(&src, data, bytes);
	ahash_request_set_callback(hreq, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &wait);
	ahash_request_set_crypt(hreq, &src, got, bytes);
	err = crypto_wait_req(crypto_ahash_digest(hreq), &wait);
	if (!err)
		err = crypto_shash_digest(desc, data, bytes, want);
	if (!err && (memcmp(got, want, 32) || got[32] != 0xa5))
		err = -EBADMSG;
	return err;
}

static int cipher_check(unsigned int row)
{
	DECLARE_CRYPTO_WAIT(wait);
	struct crypto_skcipher *hw, *sw;
	struct skcipher_request *hr = NULL, *sr = NULL;
	struct scatterlist src, dst, oracle;
	int err;
	unsigned int n = cipher_bytes ? cipher_bytes : bytes;

	hw = crypto_alloc_skcipher(ciphers[row].hardware, 0, 0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	sw = crypto_alloc_skcipher(ciphers[row].software, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(sw)) {
		err = PTR_ERR(sw);
		goto free_hw;
	}
	hr = skcipher_request_alloc(hw, GFP_KERNEL);
	sr = skcipher_request_alloc(sw, GFP_KERNEL);
	if (!hr || !sr) {
		err = -ENOMEM;
		goto out;
	}
	err = crypto_skcipher_setkey(hw, key, ciphers[row].keylen);
	if (!err)
		err = crypto_skcipher_setkey(sw, key, ciphers[row].keylen);
	if (err)
		goto out;
	memset(actual, 0xa5, n + 1);
	memset(expected, 0xa5, n + 1);
	sg_init_one(&src, data, n);
	sg_init_one(&dst, actual, n);
	sg_init_one(&oracle, expected, n);
	skcipher_request_set_callback(hr, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &wait);
	skcipher_request_set_callback(sr, 0, NULL, NULL);
	skcipher_request_set_crypt(hr, &src, &dst, n, NULL);
	skcipher_request_set_crypt(sr, &src, &oracle, n, NULL);
	pr_emerg("crypto764: interleave row=%u encrypt key=%u n=%u\n",
		row, ciphers[row].keylen, n);
	err = crypto_wait_req(crypto_skcipher_encrypt(hr), &wait);
	if (!err)
		err = crypto_skcipher_encrypt(sr);
	if (!err && (memcmp(actual, expected, n) || actual[n] != 0xa5))
		err = -EBADMSG;
	if (!err) {
		skcipher_request_set_crypt(hr, &dst, &dst, n, NULL);
		pr_emerg("crypto764: interleave row=%u decrypt key=%u n=%u\n",
			row, ciphers[row].keylen, n);
		err = crypto_wait_req(crypto_skcipher_decrypt(hr), &wait);
		if (!err && (memcmp(actual, data, n) || actual[n] != 0xa5))
			err = -EBADMSG;
	}
out:
	skcipher_request_free(hr);
	skcipher_request_free(sr);
	crypto_free_skcipher(sw);
free_hw:
	crypto_free_skcipher(hw);
	return err;
}

static int __init interleave_init(void)
{
	unsigned int round, row, i;
	int err = -ENOMEM;

	if (sweep && (only_row >= 0 || bytes < 16384))
		return -EINVAL;
	if (!bytes || bytes > 65536 || bytes % 16 || !rounds || rounds > 32 ||
	    only_row < -1 || only_row >= (int)ARRAY_SIZE(ciphers) ||
	    cipher_bytes > bytes || cipher_bytes % 16)
		return -EINVAL;
	pr_emerg("crypto764: interleave START bytes=%u rounds=%u; allocate SHA256 hardware\n",
		bytes, rounds);
	hash = crypto_alloc_ahash("sha256-rk3128", 0, 0);
	if (IS_ERR(hash))
		return PTR_ERR(hash);
	pr_emerg("crypto764: interleave allocate SHA256 software\n");
	reference = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(reference)) {
		err = PTR_ERR(reference);
		goto free_hash;
	}
	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(reference), GFP_KERNEL);
	hreq = ahash_request_alloc(hash, GFP_KERNEL);
	data = kmalloc(bytes, GFP_KERNEL);
	actual = kmalloc(bytes + 1, GFP_KERNEL);
	expected = kmalloc(bytes + 1, GFP_KERNEL);
	if (!desc || !hreq || !data || !actual || !expected)
		goto out;
	desc->tfm = reference;
	if (sweep) {
		static const unsigned int rows[] = { 2, 1, 0, 0, 0, 0 };
		static const unsigned int sizes[] = { 16384, 16384, 64, 1024, 4096, 16384 };

		for (round = 0; round < ARRAY_SIZE(rows); round++) {
			row = rows[round];
			cipher_bytes = sizes[round];
			for (i = 0; i < bytes; i++)
				data[i] = i * 17 + round * 29;
			for (i = 0; i < sizeof(key); i++)
				key[i] = i * 23 + round * 7;
			pr_emerg("crypto764: cold sweep case=%u key=%u bytes=%u; no hash\n",
				round, ciphers[row].keylen, cipher_bytes);
			err = cipher_check(row);
			if (err)
				goto out;
		}
		pr_emerg("crypto764: cold sweep PASS cases=%zu; no hash\n", ARRAY_SIZE(rows));
		goto out;
	}
	for (round = 0; round < rounds; round++) {
		for (row = 0; row < ARRAY_SIZE(ciphers); row++) {
			if (only_row >= 0 && row != only_row)
				continue;
			for (i = 0; i < bytes; i++)
				data[i] = i * 17 + round * 29 + row * 43;
			for (i = 0; i < sizeof(key); i++)
				key[i] = i * 23 + round * 7 + row * 3;
			pr_emerg("crypto764: interleave round=%u row=%u key=%u %s\n",
				round, row, ciphers[row].keylen,
				cipher_first ? "cipher-first" : "hash-before");
			err = cipher_first ? 0 : hash_check();
			if (!err) {
				pr_emerg("crypto764: interleave round=%u row=%u cipher\n", round, row);
				err = cipher_check(row);
			}
			if (!err) {
				pr_emerg("crypto764: interleave round=%u row=%u hash-after\n", round, row);
				err = hash_check();
			}
			if (err) {
				pr_err("crypto764: interleave FAIL round=%u row=%u key=%u err=%d\n",
				       round, row, ciphers[row].keylen, err);
				goto out;
			}
		}
	}
	pr_emerg("crypto764: interleave PASS rounds=%u rows=%u bytes=%u; SHA256/AES192/256/128/DES/EDE/encrypt/decrypt\n",
		rounds, only_row < 0 ? (unsigned int)ARRAY_SIZE(ciphers) : 1, bytes);
out:
	memzero_explicit(key, sizeof(key));
	kfree_sensitive(data);
	kfree_sensitive(actual);
	kfree_sensitive(expected);
	kfree_sensitive(desc);
	ahash_request_free(hreq);
	crypto_free_shash(reference);
free_hash:
	crypto_free_ahash(hash);
	return err;
}

static void __exit interleave_exit(void) {}
module_init(interleave_init);
module_exit(interleave_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded software-checked RK3128 hash/cipher interleave");
