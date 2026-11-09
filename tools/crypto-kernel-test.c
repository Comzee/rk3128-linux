// SPDX-License-Identifier: GPL-2.0-only
/* Finite explicit-driver comparisons. Build as an external module against the
 * diagnostic kernel: obj-m += crypto-kernel-test.o. No AF_ALG policy changes.
 * insmod crypto-kernel-test.ko algorithm=sha256 kind=hash cases=96 worker_pid=PID
 * CTR tail/IV comparisons use reference=ctr-aes-lib; fixed whole-block CPU
 * benchmarks leave reference empty to select the fastest synchronous driver.
 * A successful init returns zero and prints PASS; rmmod removes only this tester.
 */
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <linux/ktime.h>
#include <linux/sched/cputime.h>
#include <linux/sched/task.h>
#include <linux/rcupdate.h>
#include <linux/pid.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

static char *algorithm = "sha256";
module_param(algorithm, charp, 0400);
static char *software_reference = "";
module_param_named(reference, software_reference, charp, 0400);
MODULE_PARM_DESC(reference, "Explicit synchronous oracle; empty selects the fastest CPU driver");
static char *kind = "hash";
module_param(kind, charp, 0400);
static unsigned int bytes = UINT_MAX;
module_param(bytes, uint, 0400);
MODULE_PARM_DESC(bytes, "Fixed input size, or default mixed boundary/random cases");
static unsigned int cases = 96;
module_param(cases, uint, 0400);
#define LIMIT 65536
static u32 state = 0x75812345;
static u32 next(void)
{
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}
static unsigned int length(unsigned int test)
{
	static const unsigned int edges[] = {
		0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 55, 56, 63, 64,
		65, 127, 128, 129, 255, 256, 1023, 1024, 1025, 4095, 4096, 4097,
		16383, 16384, 16385, 32767, 32768, 32769, 65535, 65536,
	};
	if (bytes != UINT_MAX)
		return bytes;
	return test < ARRAY_SIZE(edges) ? edges[test] : next() % (LIMIT + 1);
}
/* Split both within and across pages; input starts at an unaligned byte. */
static void scatter(struct scatterlist sg[3], u8 *data, unsigned int len)
{
	unsigned int a = min(len, 1U), b = min(len - a, 4093U);

	sg_init_table(sg, 3);
	sg_set_buf(&sg[0], data, a);
	sg_set_buf(&sg[1], data + a, b);
	sg_set_buf(&sg[2], data + a + b, len - a - b);
}
/* Adjusted task time uses the scheduler's accumulated execution runtime. Count
 * the requester and the identified crypto engine worker; do not attribute Home,
 * self-tests or unrelated tasks to the algorithm. IRQ contribution follows
 * kernel accounting and is not isolated by this measurement.
 */
static unsigned int worker_pid;
module_param(worker_pid, uint, 0400);
static struct task_struct *worker;
struct cpu_sample { u64 caller, engine; };
static int sample_cpu(struct cpu_sample *sample)
{
	u64 user, system;

	/* Account this running task before sampling: tick accounting otherwise
	 * leaves sum_exec_runtime stale inside a long insmod syscall. No timed
	 * sleep; the scheduling/accounting overhead is charged to both sides.
	 */
	schedule();
	task_cputime_adjusted(current, &user, &system);
	sample->caller = user + system;
	task_cputime_adjusted(worker, &user, &system);
	sample->engine = user + system;
	return 0;
}
static u64 cpu_used(const struct cpu_sample *before, const struct cpu_sample *after)
{
	return after->caller - before->caller + after->engine - before->engine;
}

static int test_hash(void)
{
	struct crypto_ahash *hw;
	struct crypto_shash *sw;
	struct ahash_request *req = NULL;
	struct shash_desc *desc = NULL;
	struct scatterlist sg[3];
	DECLARE_CRYPTO_WAIT(wait);
	u8 *data = NULL, actual[64], expected[64];
	char driver[64];
	unsigned int t, i, n, size;
	u64 hw_ns = 0, sw_ns = 0, bytes = 0, start;
	u64 hw_cpu_ns = 0, sw_cpu_ns = 0;
	struct cpu_sample before, after;
	int err;

	snprintf(driver, sizeof(driver), "%s-rk3128", algorithm);
	hw = crypto_alloc_ahash(driver, 0, 0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	sw = crypto_alloc_shash(algorithm, 0, 0);
	if (IS_ERR(sw)) {
		err = PTR_ERR(sw);
		goto free_hw;
	}
	size = crypto_ahash_digestsize(hw);
	if (size >= sizeof(actual) || size != crypto_shash_digestsize(sw)) {
		err = -EINVAL;
		goto free_sw;
	}
	pr_info("crypto758: hash hardware=%s software=%s cases=%u\n",
		crypto_ahash_driver_name(hw), crypto_shash_driver_name(sw), cases);
	req = ahash_request_alloc(hw, GFP_KERNEL);
	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(sw), GFP_KERNEL);
	data = kmalloc(LIMIT + 2, GFP_KERNEL);
	if (!req || !desc || !data) {
		err = -ENOMEM;
		goto out;
	}
	desc->tfm = sw;
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  crypto_req_done, &wait);
	for (t = 0; t < cases; t++) {
		n = length(t);
		for (i = 0; i < n; i++)
			data[i + 1] = next();
		scatter(sg, data + 1, n);
		memset(actual, 0xa5, sizeof(actual));
		ahash_request_set_crypt(req, sg, actual, n);
		err = sample_cpu(&before);
		if (err)
			goto out;
		start = ktime_get_ns();
		err = crypto_shash_digest(desc, data + 1, n, expected);
		sw_ns += ktime_get_ns() - start;
		if (!err)
			err = sample_cpu(&after);
		if (err)
			goto out;
		sw_cpu_ns += cpu_used(&before, &after);
		if (err)
			goto out;
		err = sample_cpu(&before);
		if (err)
			goto out;
		start = ktime_get_ns();
		err = crypto_wait_req(crypto_ahash_digest(req), &wait);
		hw_ns += ktime_get_ns() - start;
		if (!err)
			err = sample_cpu(&after);
		if (err)
			goto out;
		hw_cpu_ns += cpu_used(&before, &after);
		if (err || memcmp(actual, expected, size) || actual[size] != 0xa5) {
			pr_err("crypto758: hash FAIL case=%u len=%u err=%d got=%*phN want=%*phN\n",
			       t, n, err, size, actual, size, expected);
			err = err ?: -EBADMSG;
			goto out;
		}
		bytes += n;
	}
	pr_info("crypto758: PASS hash %s cases=%u bytes=%llu hardware_ns=%llu software_ns=%llu hardware_cpu_ns=%llu software_cpu_ns=%llu cpu_scope=requester_and_engine_scheduler_accounting\n",
		algorithm, cases, bytes, hw_ns, sw_ns, hw_cpu_ns, sw_cpu_ns);
	err = 0;
out:
	kfree_sensitive(data);
	kfree_sensitive(desc);
	ahash_request_free(req);
free_sw:
	crypto_free_shash(sw);
free_hw:
	crypto_free_ahash(hw);
	return err;
}
static int test_cipher(void)
{
	struct crypto_skcipher *hw, *sw;
	struct skcipher_request *hr = NULL, *sr = NULL;
	struct scatterlist src[3], dst[3], reference;
	DECLARE_CRYPTO_WAIT(hwait);
	DECLARE_CRYPTO_WAIT(swait);
	u8 *in = NULL, *out = NULL, *expected = NULL;
	u8 key[32], hiv[16], siv[16], iv[16];
	char driver[64], mode[8], cipher[16];
	unsigned int t, i, n, keylen, ivsize, block;
	u64 hw_ns = 0, sw_ns = 0, bytes = 0, start;
	u64 hw_cpu_ns = 0, sw_cpu_ns = 0;
	struct cpu_sample before, after;
	int err;

	if (sscanf(algorithm, "%7[^(](%15[^)])", mode, cipher) != 2)
		return -EINVAL;
	/* Kernel driver names spell des3_ede as des3-ede. */
	for (i = 0; cipher[i]; i++)
		if (cipher[i] == '_')
			cipher[i] = '-';
	snprintf(driver, sizeof(driver), "%s-%s-rk3128", mode, cipher);
	hw = crypto_alloc_skcipher(driver, 0, 0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	sw = crypto_alloc_skcipher(*software_reference ? software_reference : algorithm, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(sw)) {
		err = PTR_ERR(sw);
		goto free_hw;
	}
	pr_info("crypto758: cipher hardware=%s software=%s cases=%u\n",
		crypto_skcipher_driver_name(hw), crypto_skcipher_driver_name(sw), cases);
	hr = skcipher_request_alloc(hw, GFP_KERNEL);
	sr = skcipher_request_alloc(sw, GFP_KERNEL);
	in = kmalloc(LIMIT + 2, GFP_KERNEL);
	out = kmalloc(LIMIT + 2, GFP_KERNEL);
	expected = kmalloc(LIMIT + 2, GFP_KERNEL);
	if (!hr || !sr || !in || !out || !expected) {
		err = -ENOMEM;
		goto out;
	}
	ivsize = crypto_skcipher_ivsize(hw);
	block = crypto_skcipher_blocksize(hw);
	if (ivsize > sizeof(iv)) {
		err = -EINVAL;
		goto out;
	}
	skcipher_request_set_callback(hr, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &hwait);
	skcipher_request_set_callback(sr, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &swait);
	for (t = 0; t < cases; t++) {
		n = round_down(length(t), block);
		keylen = strstr(algorithm, "aes") ? 16 + (t % 3) * 8 :
			 strstr(algorithm, "des3") ? 24 : 8;
		for (i = 0; i < sizeof(key); i++)
			key[i] = next();
		for (i = 0; i < sizeof(iv); i++)
			iv[i] = next();
		for (i = 0; i < n; i++)
			in[i + 1] = next();
		err = crypto_skcipher_setkey(hw, key, keylen);
		if (!err)
			err = crypto_skcipher_setkey(sw, key, keylen);
		if (err)
			goto out;
		memcpy(hiv, iv, sizeof(iv));
		memcpy(siv, iv, sizeof(iv));
		memset(out, 0xa5, LIMIT + 2);
		memcpy(expected + 1, in + 1, n);
		scatter(src, in + 1, n);
		scatter(dst, out + 1, n);
		sg_init_one(&reference, expected + 1, n);
		skcipher_request_set_crypt(hr, src, dst, n, hiv);
		skcipher_request_set_crypt(sr, &reference, &reference, n, siv);
		err = sample_cpu(&before);
		if (err)
			goto out;
		start = ktime_get_ns();
		err = crypto_wait_req(crypto_skcipher_encrypt(sr), &swait);
		sw_ns += ktime_get_ns() - start;
		if (!err)
			err = sample_cpu(&after);
		if (err)
			goto out;
		sw_cpu_ns += cpu_used(&before, &after);
		if (err)
			goto out;
		err = sample_cpu(&before);
		if (err)
			goto out;
		start = ktime_get_ns();
		err = crypto_wait_req(crypto_skcipher_encrypt(hr), &hwait);
		hw_ns += ktime_get_ns() - start;
		if (!err)
			err = sample_cpu(&after);
		if (err)
			goto out;
		hw_cpu_ns += cpu_used(&before, &after);
		if (err || memcmp(out + 1, expected + 1, n) ||
		    memcmp(hiv, siv, ivsize) || out[0] != 0xa5 || out[n + 1] != 0xa5)
			goto fail;
		/* Decrypt in place, checking the API's final chaining IV as well. */
		memcpy(hiv, iv, sizeof(iv));
		memcpy(siv, iv, sizeof(iv));
		skcipher_request_set_crypt(hr, dst, dst, n, hiv);
		skcipher_request_set_crypt(sr, &reference, &reference, n, siv);
		err = crypto_wait_req(crypto_skcipher_decrypt(sr), &swait);
		if (!err)
			err = crypto_wait_req(crypto_skcipher_decrypt(hr), &hwait);
		if (err || memcmp(out + 1, in + 1, n) || memcmp(hiv, siv, ivsize))
			goto fail;
		bytes += n;
	}
	pr_info("crypto758: PASS cipher %s cases=%u bytes=%llu hardware_ns=%llu software_ns=%llu hardware_cpu_ns=%llu software_cpu_ns=%llu cpu_scope=requester_and_engine_scheduler_accounting\n",
		algorithm, cases, bytes, hw_ns, sw_ns, hw_cpu_ns, sw_cpu_ns);
	err = 0;
	goto out;
fail:
	pr_err("crypto758: cipher FAIL %s case=%u len=%u keylen=%u err=%d got=%*phN want=%*phN iv=%*phN want_iv=%*phN\n",
	       algorithm, t, n, keylen, err, min(n, 32U), out + 1,
	       min(n, 32U), expected + 1, ivsize, hiv, ivsize, siv);
	err = err ?: -EBADMSG;
out:
	kfree_sensitive(in);
	kfree_sensitive(out);
	kfree_sensitive(expected);
	skcipher_request_free(hr);
	skcipher_request_free(sr);
	crypto_free_skcipher(sw);
free_hw:
	crypto_free_skcipher(hw);
	return err;
}
static int __init test_init(void)
{
	int err;

	if (!cases || cases > 512 || (bytes != UINT_MAX && bytes > LIMIT) || !worker_pid)
		return -EINVAL;
	rcu_read_lock();
	worker = get_pid_task(find_vpid(worker_pid), PIDTYPE_PID);
	rcu_read_unlock();
	if (!worker)
		return -ESRCH;
	if (strncmp(worker->comm, "100fc000.crypto", TASK_COMM_LEN)) {
		err = -EINVAL;
		goto out;
	}
	if (!strcmp(kind, "hash"))
		err = test_hash();
	else if (!strcmp(kind, "cipher"))
		err = test_cipher();
	else
		err = -EINVAL;
out:
	put_task_struct(worker);
	return err;
}
static void __exit test_exit(void) {}
module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded RK3128 explicit-driver crypto comparisons");
