// SPDX-License-Identifier: GPL-2.0-only
/* Finite ordered-pair and concurrent RK3128 API acceptance. External module:
 * obj-m += crypto-transition-matrix.o. No MMIO, reset, PM or clock controls.
 * Each worker owns its transforms/requests/buffers and software references.
 */
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/sched/task.h>
#include <linux/slab.h>

#define MAX_BYTES 65536
#define GUARD 16
#define NBUF 5
#define NWORK 4
static unsigned int random_cases = 64;
module_param(random_cases, uint, 0400);
static bool hash_only;
module_param(hash_only, bool, 0400);
static bool software_only;
module_param(software_only, bool, 0400);
static unsigned int nvec;

static const struct vector {
	const char *name, *driver, *software;
	unsigned int keylen, cutoff, block, ivsize;
	bool hash, ctr;
} vectors[] = {
	{ "md5", "md5-rk3128", "md5", 0, 16384, 1, 0, true },
	{ "sha1", "sha1-rk3128", "sha1", 0, 16384, 1, 0, true },
	{ "sha256", "sha256-rk3128", "sha256", 0, 4096, 1, 0, true },
	{ "ecb-aes128", "ecb-aes-rk3128", "ecb-aes-lib", 16, 4096, 16 },
	{ "ecb-aes192", "ecb-aes-rk3128", "ecb-aes-lib", 24, 4096, 16 },
	{ "ecb-aes256", "ecb-aes-rk3128", "ecb-aes-lib", 32, 4096, 16 },
	{ "cbc-aes128", "cbc-aes-rk3128", "cbc-aes-lib", 16, 4096, 16, 16 },
	{ "cbc-aes192", "cbc-aes-rk3128", "cbc-aes-lib", 24, 4096, 16, 16 },
	{ "cbc-aes256", "cbc-aes-rk3128", "cbc-aes-lib", 32, 4096, 16, 16 },
	{ "ctr-aes128", "ctr-aes-rk3128", "ctr-aes-lib", 16, 4096, 1, 16, false, true },
	{ "ctr-aes192", "ctr-aes-rk3128", "ctr-aes-lib", 24, 4096, 1, 16, false, true },
	{ "ctr-aes256", "ctr-aes-rk3128", "ctr-aes-lib", 32, 4096, 1, 16, false, true },
	{ "ecb-des", "ecb-des-rk3128", "ecb(des-generic)", 8, 4096, 8 },
	{ "cbc-des", "cbc-des-rk3128", "cbc(ecb(des-generic))", 8, 4096, 8, 8 },
	{ "ecb-ede", "ecb-des3-ede-rk3128", "ecb(des3_ede-generic)", 24, 1024, 8 },
	{ "cbc-ede", "cbc-des3-ede-rk3128", "cbc(ecb(des3_ede-generic))", 24, 1024, 8, 8 },
};

#define NVEC ARRAY_SIZE(vectors)

struct case_ctx {
	struct crypto_ahash *hash;
	struct crypto_shash *shash;
	struct ahash_request *hreq;
	struct shash_desc *desc;
	struct crypto_skcipher *cipher, *software;
	struct skcipher_request *req, *swreq;
};

struct worker {
	struct case_ctx cases[NVEC];
	u8 *buf[NBUF];
	u32 state;
	unsigned int id, checked;
	int err;
	struct task_struct *task;
	struct completion done;
};

static struct worker workers[NWORK];
static struct completion start;

static u32 next(struct worker *w)
{
	w->state ^= w->state << 13;
	w->state ^= w->state >> 17;
	w->state ^= w->state << 5;
	return w->state;
}

static void scatter(struct scatterlist sg[3], u8 *data, unsigned int n)
{
	unsigned int a = min(n, 1U), b = min(n - a, 4093U);

	sg_init_table(sg, 3);
	sg_set_buf(sg, data, a);
	sg_set_buf(sg + 1, data + a, b);
	sg_set_buf(sg + 2, data + a + b, n - a - b);
}

static void release_worker(struct worker *w)
{
	unsigned int i;

	for (i = 0; i < NVEC; i++) {
		struct case_ctx *c = &w->cases[i];

		ahash_request_free(c->hreq);
		kfree_sensitive(c->desc);
		if (c->hash)
			crypto_free_ahash(c->hash);
		if (c->shash)
			crypto_free_shash(c->shash);
		skcipher_request_free(c->req);
		skcipher_request_free(c->swreq);
		if (c->cipher)
			crypto_free_skcipher(c->cipher);
		if (c->software)
			crypto_free_skcipher(c->software);
	}
	for (i = 0; i < NBUF; i++)
		kfree_sensitive(w->buf[i]);
}

static int prepare_worker(struct worker *w)
{
	unsigned int i;
	int err;

	for (i = 0; i < NBUF; i++) {
		w->buf[i] = kmalloc(MAX_BYTES + 2 * GUARD + 1, GFP_KERNEL);
		if (!w->buf[i])
			return -ENOMEM;
	}
	for (i = 0; i < nvec; i++) {
		const struct vector *v = &vectors[i];
		const char *driver = software_only ? v->software : v->driver;
		struct case_ctx *c = &w->cases[i];
		u32 mask = software_only ? CRYPTO_ALG_ASYNC : 0;

		if (v->hash) {
			c->hash = crypto_alloc_ahash(driver, 0, mask);
			if (IS_ERR(c->hash)) {
				err = PTR_ERR(c->hash);
				c->hash = NULL;
				return err;
			}
			c->shash = crypto_alloc_shash(v->software, 0, 0);
			if (IS_ERR(c->shash)) {
				err = PTR_ERR(c->shash);
				c->shash = NULL;
				return err;
			}
			c->hreq = ahash_request_alloc(c->hash, GFP_KERNEL);
			c->desc = kzalloc(sizeof(*c->desc) + crypto_shash_descsize(c->shash),
					  GFP_KERNEL);
			if (!c->hreq || !c->desc)
				return -ENOMEM;
			c->desc->tfm = c->shash;
		} else {
			c->cipher = crypto_alloc_skcipher(driver, 0, mask);
			if (IS_ERR(c->cipher)) {
				err = PTR_ERR(c->cipher);
				c->cipher = NULL;
				return err;
			}
			c->software = crypto_alloc_skcipher(v->software, 0, CRYPTO_ALG_ASYNC);
			if (IS_ERR(c->software)) {
				err = PTR_ERR(c->software);
				c->software = NULL;
				return err;
			}
			c->req = skcipher_request_alloc(c->cipher, GFP_KERNEL);
			c->swreq = skcipher_request_alloc(c->software, GFP_KERNEL);
			if (!c->req || !c->swreq)
				return -ENOMEM;
		}
	}
	return 0;
}

static int operate(struct worker *w, unsigned int index, unsigned int n)
{
	DECLARE_CRYPTO_WAIT(wait);
	const struct vector *v = &vectors[index];
	struct case_ctx *c = &w->cases[index];
	struct scatterlist src[3], dst[3], oracle[3], plain[3], swplain[3];
	u8 key[32], iv[16], hiv[16], siv[16];
	u8 *b[NBUF];
	unsigned int i, j, result = v->hash ? crypto_ahash_digestsize(c->hash) : n;
	int err;

	if (!n || n > MAX_BYTES || result > MAX_BYTES)
		return -EINVAL;
	for (i = 0; i < NBUF; i++) {
		memset(w->buf[i], 0xa5, MAX_BYTES + 2 * GUARD + 1);
		b[i] = w->buf[i] + GUARD + 1; /* deliberately unaligned */
	}
	for (i = 0; i < n; i++)
		b[0][i] = next(w);
	for (i = 0; i < sizeof(key); i++)
		key[i] = next(w);
	for (i = 0; i < sizeof(iv); i++)
		iv[i] = next(w);
	memcpy(hiv, iv, sizeof(iv));
	memcpy(siv, iv, sizeof(iv));
	scatter(src, b[0], n);
	if (v->hash) {
		ahash_request_set_callback(c->hreq, CRYPTO_TFM_REQ_MAY_BACKLOG,
					   crypto_req_done, &wait);
		ahash_request_set_crypt(c->hreq, src, b[1], n);
		err = crypto_wait_req(crypto_ahash_digest(c->hreq), &wait);
		if (!err)
			err = crypto_shash_digest(c->desc, b[0], n, b[2]);
	} else {
		scatter(dst, b[1], n);
		scatter(oracle, b[2], n);
		scatter(plain, b[3], n);
		scatter(swplain, b[4], n);
		err = crypto_skcipher_setkey(c->cipher, key, v->keylen);
		if (!err)
			err = crypto_skcipher_setkey(c->software, key, v->keylen);
		if (err)
			goto out;
		skcipher_request_set_callback(c->req, CRYPTO_TFM_REQ_MAY_BACKLOG,
					      crypto_req_done, &wait);
		skcipher_request_set_callback(c->swreq, 0, NULL, NULL);
		skcipher_request_set_crypt(c->req, src, dst, n, v->ivsize ? hiv : NULL);
		skcipher_request_set_crypt(c->swreq, src, oracle, n, v->ivsize ? siv : NULL);
		err = crypto_wait_req(crypto_skcipher_encrypt(c->req), &wait);
		if (!err)
			err = crypto_skcipher_encrypt(c->swreq);
		if (!err && memcmp(hiv, siv, v->ivsize))
			err = -EBADMSG;
		if (!err && memcmp(b[1], b[2], n))
			err = -EBADMSG;
		if (err)
			goto out;
		memcpy(hiv, iv, sizeof(iv));
		memcpy(siv, iv, sizeof(iv));
		skcipher_request_set_crypt(c->req, dst, plain, n, v->ivsize ? hiv : NULL);
		skcipher_request_set_crypt(c->swreq, oracle, swplain, n, v->ivsize ? siv : NULL);
		err = crypto_wait_req(crypto_skcipher_decrypt(c->req), &wait);
		if (!err)
			err = crypto_skcipher_decrypt(c->swreq);
		if (!err && (memcmp(b[3], b[0], n) || memcmp(b[4], b[0], n) ||
			     memcmp(hiv, siv, v->ivsize)))
			err = -EBADMSG;
	}
	if (!err && memcmp(b[1], b[2], result))
		err = -EBADMSG;
	/* All prefixes and the complete unused suffixes remain untouched. */
	for (i = 0; !err && i < NBUF; i++) {
		unsigned int used = i == 0 ? n : v->hash ? (i < 3 ? result : 0) : n;

		for (j = 0; j <= GUARD; j++)
			if (w->buf[i][j] != 0xa5)
				err = -EOVERFLOW;
		if (memchr_inv(b[i] + used, 0xa5, MAX_BYTES + GUARD - used))
			err = -EOVERFLOW;
	}
out:
	memzero_explicit(key, sizeof(key));
	memzero_explicit(iv, sizeof(iv));
	memzero_explicit(hiv, sizeof(hiv));
	memzero_explicit(siv, sizeof(siv));
	if (!err)
		w->checked++;
	else
		pr_err("crypto764: FAIL worker=%u algorithm=%s bytes=%u err=%d\n",
		       w->id, v->name, n, err);
	return err;
}

static unsigned int size_for(struct worker *w, unsigned int v, unsigned int profile)
{
	unsigned int n = profile ? vectors[v].cutoff + 17 : vectors[v].cutoff - 17;

	if (profile == 2)
		n = 1 + next(w) % MAX_BYTES;
	return round_up(n, vectors[v].block);
}

static int random_worker(void *arg)
{
	struct worker *w = arg;
	unsigned int i, v;

	wait_for_completion(&start);
	for (i = 0; i < random_cases && !kthread_should_stop(); i++) {
		v = next(w) % nvec;
		pr_emerg("crypto764: random worker=%u case=%u algorithm=%s\n",
			 w->id, i, vectors[v].name);
		w->err = operate(w, v, size_for(w, v, 2));
		if (w->err)
			break;
		cond_resched();
	}
	complete(&w->done);
	return w->err;
}

static int __init matrix_init(void)
{
	unsigned int a, b, p, i, started = 0;
	int err = 0;

	if (!random_cases || random_cases > 512)
		return -EINVAL;
	nvec = hash_only ? 3 : NVEC;
	pr_info("crypto764: matrix software_only=%u vectors=%u random_cases=%u\n",
		software_only, nvec, random_cases);
	init_completion(&start);
	for (i = 0; i < NWORK; i++) {
		workers[i].state = 0x764a3c91 ^ (0x1234567 * (i + 1));
		workers[i].id = i;
		init_completion(&workers[i].done);
		err = prepare_worker(&workers[i]);
		if (err)
			goto out;
	}
	for (p = 0; p < 4; p++) {
		for (a = 0; a < nvec; a++) {
			for (b = 0; b < nvec; b++) {
				pr_emerg("crypto764: pair profile=%u %s -> %s\n",
					 p, vectors[a].name, vectors[b].name);
				err = operate(&workers[0], a, size_for(&workers[0], a, p >> 1));
				if (!err)
					err = operate(&workers[0], b,
						      size_for(&workers[0], b, p & 1));
				if (err)
					goto out;
			}
		}
	}
	pr_info("crypto764: ordered pairs PASS vectors=%u profiles=4 pairs=%u operations=%u\n",
		nvec, 4 * nvec * nvec, workers[0].checked);
	for (i = 0; i < NWORK; i++) {
		workers[i].task = kthread_run(random_worker, &workers[i], "crypto764-%u", i);
		if (IS_ERR(workers[i].task)) {
			err = PTR_ERR(workers[i].task);
			break;
		}
		/* start is still blocked: protect even a quickly exiting task until
		 * kthread_stop joins it, before this test module can be unloaded.
		 */
		get_task_struct(workers[i].task);
		started++;
	}
	complete_all(&start);
	for (i = 0; i < started; i++) {
		wait_for_completion(&workers[i].done);
		kthread_stop(workers[i].task);
		put_task_struct(workers[i].task);
		if (!err)
			err = workers[i].err;
	}
	if (!err)
		pr_info("crypto764: concurrent PASS threads=%u cases_per_thread=%u\n",
			NWORK, random_cases);
out:
	for (i = 0; i < NWORK; i++)
		release_worker(&workers[i]);
	return err;
}

static void __exit matrix_exit(void) {}
module_init(matrix_init);
module_exit(matrix_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Finite RK3128 ordered transitions and concurrent API acceptance");
