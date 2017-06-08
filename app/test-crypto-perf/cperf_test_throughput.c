/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2016-2017 Intel Corporation. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>

#include "cperf_test_throughput.h"
#include "cperf_ops.h"

struct cperf_throughput_results {
	uint64_t ops_enqueued;
	uint64_t ops_dequeued;

	uint64_t ops_enqueued_failed;
	uint64_t ops_dequeued_failed;

	uint64_t ops_failed;

	double ops_per_second;
	double throughput_gbps;
	double cycles_per_byte;
};

struct cperf_throughput_ctx {
	uint8_t dev_id;
	uint16_t qp_id;
	uint8_t lcore_id;

	struct rte_mempool *pkt_mbuf_pool_in;
	struct rte_mempool *pkt_mbuf_pool_out;
	struct rte_mbuf **mbufs_in;
	struct rte_mbuf **mbufs_out;

	struct rte_mempool *crypto_op_pool;

	struct rte_cryptodev_sym_session *sess;

	cperf_populate_ops_t populate_ops;
	cperf_verify_crypto_op_t verify_op_output;

	const struct cperf_options *options;
	const struct cperf_test_vector *test_vector;
	struct cperf_throughput_results results;

};

struct cperf_op_result {
	enum rte_crypto_op_status status;
};

static void
cperf_throughput_test_free(struct cperf_throughput_ctx *ctx, uint32_t mbuf_nb)
{
	uint32_t i;

	if (ctx) {
		if (ctx->sess)
			rte_cryptodev_sym_session_free(ctx->dev_id, ctx->sess);

		if (ctx->mbufs_in) {
			for (i = 0; i < mbuf_nb; i++)
				rte_pktmbuf_free(ctx->mbufs_in[i]);

			rte_free(ctx->mbufs_in);
		}

		if (ctx->mbufs_out) {
			for (i = 0; i < mbuf_nb; i++) {
				if (ctx->mbufs_out[i] != NULL)
					rte_pktmbuf_free(ctx->mbufs_out[i]);
			}

			rte_free(ctx->mbufs_out);
		}

		if (ctx->pkt_mbuf_pool_in)
			rte_mempool_free(ctx->pkt_mbuf_pool_in);

		if (ctx->pkt_mbuf_pool_out)
			rte_mempool_free(ctx->pkt_mbuf_pool_out);

		if (ctx->crypto_op_pool)
			rte_mempool_free(ctx->crypto_op_pool);

		rte_free(ctx);
	}
}

static struct rte_mbuf *
cperf_mbuf_create(struct rte_mempool *mempool,
		uint32_t segments_nb,
		const struct cperf_options *options,
		const struct cperf_test_vector *test_vector)
{
	struct rte_mbuf *mbuf;
	uint32_t segment_sz = options->buffer_sz / segments_nb;
	uint32_t last_sz = options->buffer_sz % segments_nb;
	uint8_t *mbuf_data;
	uint8_t *test_data =
			(options->cipher_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) ?
					test_vector->plaintext.data :
					test_vector->ciphertext.data;

	mbuf = rte_pktmbuf_alloc(mempool);
	if (mbuf == NULL)
		goto error;

	mbuf_data = (uint8_t *)rte_pktmbuf_append(mbuf, segment_sz);
	if (mbuf_data == NULL)
		goto error;

	memcpy(mbuf_data, test_data, segment_sz);
	test_data += segment_sz;
	segments_nb--;

	while (segments_nb) {
		struct rte_mbuf *m;

		m = rte_pktmbuf_alloc(mempool);
		if (m == NULL)
			goto error;

		rte_pktmbuf_chain(mbuf, m);

		mbuf_data = (uint8_t *)rte_pktmbuf_append(mbuf, segment_sz);
		if (mbuf_data == NULL)
			goto error;

		memcpy(mbuf_data, test_data, segment_sz);
		test_data += segment_sz;
		segments_nb--;
	}

	if (last_sz) {
		mbuf_data = (uint8_t *)rte_pktmbuf_append(mbuf, last_sz);
		if (mbuf_data == NULL)
			goto error;

		memcpy(mbuf_data, test_data, last_sz);
	}

	mbuf_data = (uint8_t *)rte_pktmbuf_append(mbuf,
			options->auth_digest_sz);
	if (mbuf_data == NULL)
		goto error;

	if (options->op_type == CPERF_AEAD) {
		uint8_t *aead = (uint8_t *)rte_pktmbuf_prepend(mbuf,
			RTE_ALIGN_CEIL(options->auth_aad_sz, 16));

		if (aead == NULL)
			goto error;

		memcpy(aead, test_vector->aad.data, test_vector->aad.length);
	}

	return mbuf;
error:
	if (mbuf != NULL)
		rte_pktmbuf_free(mbuf);

	return NULL;
}

void *
cperf_throughput_test_constructor(uint8_t dev_id, uint16_t qp_id,
		const struct cperf_options *options,
		const struct cperf_test_vector *test_vector,
		const struct cperf_op_fns *op_fns)
{
	struct cperf_throughput_ctx *ctx = NULL;
	unsigned int mbuf_idx = 0;
	char pool_name[32] = "";

	ctx = rte_malloc(NULL, sizeof(struct cperf_throughput_ctx), 0);
	if (ctx == NULL)
		goto err;

	ctx->dev_id = dev_id;
	ctx->qp_id = qp_id;

	ctx->populate_ops = op_fns->populate_ops;
	ctx->options = options;
	ctx->test_vector = test_vector;

	ctx->sess = op_fns->sess_create(dev_id, options, test_vector);
	if (ctx->sess == NULL)
		goto err;

	snprintf(pool_name, sizeof(pool_name), "cperf_pool_in_cdev_%d",
			dev_id);

	ctx->pkt_mbuf_pool_in = rte_pktmbuf_pool_create(pool_name,
			options->pool_sz * options->segments_nb, 0, 0,
			RTE_PKTMBUF_HEADROOM +
			RTE_CACHE_LINE_ROUNDUP(
				(options->buffer_sz / options->segments_nb) +
				(options->buffer_sz % options->segments_nb) +
					options->auth_digest_sz),
			rte_socket_id());

	if (ctx->pkt_mbuf_pool_in == NULL)
		goto err;

	/* Generate mbufs_in with plaintext populated for test */
	if (ctx->options->pool_sz % ctx->options->burst_sz)
		goto err;

	ctx->mbufs_in = rte_malloc(NULL,
			(sizeof(struct rte_mbuf *) * ctx->options->pool_sz), 0);

	for (mbuf_idx = 0; mbuf_idx < options->pool_sz; mbuf_idx++) {
		ctx->mbufs_in[mbuf_idx] = cperf_mbuf_create(
				ctx->pkt_mbuf_pool_in, options->segments_nb,
				options, test_vector);
		if (ctx->mbufs_in[mbuf_idx] == NULL)
			goto err;
	}

	if (options->out_of_place == 1)	{

		snprintf(pool_name, sizeof(pool_name), "cperf_pool_out_cdev_%d",
				dev_id);

		ctx->pkt_mbuf_pool_out = rte_pktmbuf_pool_create(
				pool_name, options->pool_sz, 0, 0,
				RTE_PKTMBUF_HEADROOM +
				RTE_CACHE_LINE_ROUNDUP(
					options->buffer_sz +
					options->auth_digest_sz),
				rte_socket_id());

		if (ctx->pkt_mbuf_pool_out == NULL)
			goto err;
	}

	ctx->mbufs_out = rte_malloc(NULL,
			(sizeof(struct rte_mbuf *) *
			ctx->options->pool_sz), 0);

	for (mbuf_idx = 0; mbuf_idx < options->pool_sz; mbuf_idx++) {
		if (options->out_of_place == 1)	{
			ctx->mbufs_out[mbuf_idx] = cperf_mbuf_create(
					ctx->pkt_mbuf_pool_out, 1,
					options, test_vector);
			if (ctx->mbufs_out[mbuf_idx] == NULL)
				goto err;
		} else {
			ctx->mbufs_out[mbuf_idx] = NULL;
		}
	}

	snprintf(pool_name, sizeof(pool_name), "cperf_op_pool_cdev_%d",
			dev_id);

	ctx->crypto_op_pool = rte_crypto_op_pool_create(pool_name,
			RTE_CRYPTO_OP_TYPE_SYMMETRIC, options->pool_sz, 0, 0,
			rte_socket_id());
	if (ctx->crypto_op_pool == NULL)
		goto err;

	return ctx;
err:
	cperf_throughput_test_free(ctx, mbuf_idx);

	return NULL;
}

static int
cperf_throughput_test_verifier(struct rte_mbuf *mbuf,
		const struct cperf_options *options,
		const struct cperf_test_vector *vector)
{
	const struct rte_mbuf *m;
	uint32_t len;
	uint16_t nb_segs;
	uint8_t *data;
	uint32_t cipher_offset, auth_offset;
	uint8_t	cipher, auth;
	int res = 0;

	m = mbuf;
	nb_segs = m->nb_segs;
	len = 0;
	while (m && nb_segs != 0) {
		len += m->data_len;
		m = m->next;
		nb_segs--;
	}

	data = rte_malloc(NULL, len, 0);
	if (data == NULL)
		return 1;

	m = mbuf;
	nb_segs = m->nb_segs;
	len = 0;
	while (m && nb_segs != 0) {
		memcpy(data + len, rte_pktmbuf_mtod(m, uint8_t *),
				m->data_len);
		len += m->data_len;
		m = m->next;
		nb_segs--;
	}

	switch (options->op_type) {
	case CPERF_CIPHER_ONLY:
		cipher = 1;
		cipher_offset = 0;
		auth = 0;
		auth_offset = 0;
		break;
	case CPERF_CIPHER_THEN_AUTH:
		cipher = 1;
		cipher_offset = 0;
		auth = 1;
		auth_offset = vector->plaintext.length;
		break;
	case CPERF_AUTH_ONLY:
		cipher = 0;
		cipher_offset = 0;
		auth = 1;
		auth_offset = vector->plaintext.length;
		break;
	case CPERF_AUTH_THEN_CIPHER:
		cipher = 1;
		cipher_offset = 0;
		auth = 1;
		auth_offset = vector->plaintext.length;
		break;
	case CPERF_AEAD:
		cipher = 1;
		cipher_offset = vector->aad.length;
		auth = 1;
		auth_offset = vector->aad.length + vector->plaintext.length;
		break;
	}

	if (cipher == 1) {
		if (options->cipher_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT)
			res += memcmp(data + cipher_offset,
					vector->ciphertext.data,
					vector->ciphertext.length);
		else
			res += memcmp(data + cipher_offset,
					vector->plaintext.data,
					vector->plaintext.length);
	}

	if (auth == 1) {
		if (options->auth_op == RTE_CRYPTO_AUTH_OP_GENERATE)
			res += memcmp(data + auth_offset,
					vector->digest.data,
					vector->digest.length);
	}

	if (res != 0)
		res = 1;

	return res;
}

int
cperf_throughput_test_runner(void *test_ctx)
{
	struct cperf_throughput_ctx *ctx = test_ctx;
	struct cperf_op_result *res, *pres;

	if (ctx->options->verify) {
		res = rte_malloc(NULL, sizeof(struct cperf_op_result) *
				ctx->options->total_ops, 0);
		if (res == NULL)
			return 0;
	}

	uint64_t ops_enqd = 0, ops_enqd_total = 0, ops_enqd_failed = 0;
	uint64_t ops_deqd = 0, ops_deqd_total = 0, ops_deqd_failed = 0;

	uint64_t i, m_idx = 0, tsc_start, tsc_end, tsc_duration;

	uint16_t ops_unused = 0;
	uint64_t idx = 0;

	struct rte_crypto_op *ops[ctx->options->burst_sz];
	struct rte_crypto_op *ops_processed[ctx->options->burst_sz];

	uint32_t lcore = rte_lcore_id();

#ifdef CPERF_LINEARIZATION_ENABLE
	struct rte_cryptodev_info dev_info;
	int linearize = 0;

	/* Check if source mbufs require coalescing */
	if (ctx->options->segments_nb > 1) {
		rte_cryptodev_info_get(ctx->dev_id, &dev_info);
		if ((dev_info.feature_flags &
				RTE_CRYPTODEV_FF_MBUF_SCATTER_GATHER) == 0)
			linearize = 1;
	}
#endif /* CPERF_LINEARIZATION_ENABLE */

	ctx->lcore_id = lcore;

	if (!ctx->options->csv)
		printf("\n# Running throughput test on device: %u, lcore: %u\n",
			ctx->dev_id, lcore);

	/* Warm up the host CPU before starting the test */
	for (i = 0; i < ctx->options->total_ops; i++)
		rte_cryptodev_enqueue_burst(ctx->dev_id, ctx->qp_id, NULL, 0);

	tsc_start = rte_rdtsc_precise();

	while (ops_enqd_total < ctx->options->total_ops) {

		uint16_t burst_size = ((ops_enqd_total + ctx->options->burst_sz)
				<= ctx->options->total_ops) ?
						ctx->options->burst_sz :
						ctx->options->total_ops -
						ops_enqd_total;

		uint16_t ops_needed = burst_size - ops_unused;

		/* Allocate crypto ops from pool */
		if (ops_needed != rte_crypto_op_bulk_alloc(
				ctx->crypto_op_pool,
				RTE_CRYPTO_OP_TYPE_SYMMETRIC,
				ops, ops_needed))
			return -1;

		/* Setup crypto op, attach mbuf etc */
		(ctx->populate_ops)(ops, &ctx->mbufs_in[m_idx],
				&ctx->mbufs_out[m_idx],
				ops_needed, ctx->sess, ctx->options,
				ctx->test_vector);

		if (ctx->options->verify) {
			for (i = 0; i < ops_needed; i++) {
				ops[i]->opaque_data = (void *)&res[idx];
				idx++;
			}
		}

			/**
			 * When ops_needed is smaller than ops_enqd, the
			 * unused ops need to be moved to the front for
			 * next round use.
			 */
			if (unlikely(ops_enqd > ops_needed)) {
				size_t nb_b_to_mov = ops_unused * sizeof(
						struct rte_crypto_op *);

				memmove(&ops[ops_needed], &ops[ops_enqd],
					nb_b_to_mov);
			}

#ifdef CPERF_LINEARIZATION_ENABLE
		if (linearize) {
			/* PMD doesn't support scatter-gather and source buffer
			 * is segmented.
			 * We need to linearize it before enqueuing.
			 */
			for (i = 0; i < burst_size; i++)
				rte_pktmbuf_linearize(ops[i]->sym->m_src);
		}
#endif /* CPERF_LINEARIZATION_ENABLE */

		/* Enqueue burst of ops on crypto device */
		ops_enqd = rte_cryptodev_enqueue_burst(ctx->dev_id, ctx->qp_id,
				ops, burst_size);
		if (ops_enqd < burst_size)
			ops_enqd_failed++;

		/**
		 * Calculate number of ops not enqueued (mainly for hw
		 * accelerators whose ingress queue can fill up).
		 */
		ops_unused = burst_size - ops_enqd;
		ops_enqd_total += ops_enqd;


		/* Dequeue processed burst of ops from crypto device */
		ops_deqd = rte_cryptodev_dequeue_burst(ctx->dev_id, ctx->qp_id,
				ops_processed, ctx->options->burst_sz);

		if (likely(ops_deqd))  {

			if (ctx->options->verify) {
				void *opq;
				for (i = 0; i < ops_deqd; i++) {
					opq = (ops_processed[i]->opaque_data);
					pres = (struct cperf_op_result *)opq;
					pres->status = ops_processed[i]->status;
				}
			}

			/* free crypto ops so they can be reused. We don't free
			 * the mbufs here as we don't want to reuse them as
			 * the crypto operation will change the data and cause
			 * failures.
			 */
			for (i = 0; i < ops_deqd; i++)
				rte_crypto_op_free(ops_processed[i]);

			ops_deqd_total += ops_deqd;
		} else {
			/**
			 * Count dequeue polls which didn't return any
			 * processed operations. This statistic is mainly
			 * relevant to hw accelerators.
			 */
			ops_deqd_failed++;
		}

		m_idx += ops_needed;
		m_idx = m_idx + ctx->options->burst_sz > ctx->options->pool_sz ?
				0 : m_idx;
	}

	/* Dequeue any operations still in the crypto device */

	while (ops_deqd_total < ctx->options->total_ops) {
		/* Sending 0 length burst to flush sw crypto device */
		rte_cryptodev_enqueue_burst(ctx->dev_id, ctx->qp_id, NULL, 0);

		/* dequeue burst */
		ops_deqd = rte_cryptodev_dequeue_burst(ctx->dev_id, ctx->qp_id,
				ops_processed, ctx->options->burst_sz);
		if (ops_deqd == 0)
			ops_deqd_failed++;
		else {
			if (ctx->options->verify) {
				void *opq;
				for (i = 0; i < ops_deqd; i++) {
					opq = (ops_processed[i]->opaque_data);
					pres = (struct cperf_op_result *)opq;
					pres->status = ops_processed[i]->status;
				}
			}

			for (i = 0; i < ops_deqd; i++)
				rte_crypto_op_free(ops_processed[i]);

			ops_deqd_total += ops_deqd;
		}
	}

	tsc_end = rte_rdtsc_precise();
	tsc_duration = (tsc_end - tsc_start);

	if (ctx->options->verify) {
		struct rte_mbuf **mbufs;

		if (ctx->options->out_of_place == 1)
			mbufs = ctx->mbufs_out;
		else
			mbufs = ctx->mbufs_in;

		for (i = 0; i < ctx->options->total_ops; i++) {

			if (res[i].status != RTE_CRYPTO_OP_STATUS_SUCCESS ||
					cperf_throughput_test_verifier(
					mbufs[i], ctx->options,
					ctx->test_vector)) {

				ctx->results.ops_failed++;
			}
		}

		rte_free(res);
	}

	/* Calculate average operations processed per second */
	ctx->results.ops_per_second = ((double)ctx->options->total_ops /
			tsc_duration) * rte_get_tsc_hz();

	/* Calculate average throughput (Gbps) in bits per second */
	ctx->results.throughput_gbps = ((ctx->results.ops_per_second *
			ctx->options->buffer_sz * 8) / 1000000000);


	/* Calculate average cycles per byte */
	ctx->results.cycles_per_byte =  ((double)tsc_duration /
			ctx->options->total_ops) / ctx->options->buffer_sz;

	ctx->results.ops_enqueued = ops_enqd_total;
	ctx->results.ops_dequeued = ops_deqd_total;

	ctx->results.ops_enqueued_failed = ops_enqd_failed;
	ctx->results.ops_dequeued_failed = ops_deqd_failed;

	return 0;
}



void
cperf_throughput_test_destructor(void *arg)
{
	struct cperf_throughput_ctx *ctx = arg;
	struct cperf_throughput_results *results = &ctx->results;
	static int only_once;

	if (ctx == NULL)
		return;

	if (!ctx->options->csv) {
		printf("\n# Device %d on lcore %u\n",
				ctx->dev_id, ctx->lcore_id);
		printf("# Buffer Size(B)\t  Enqueued\t  Dequeued\tFailed Enq"
				"\tFailed Deq\tOps(Millions)\tThroughput(Gbps)"
				"\tCycles Per Byte\n");

		printf("\n%16u\t%10"PRIu64"\t%10"PRIu64"\t%10"PRIu64"\t"
				"%10"PRIu64"\t%16.4f\t%16.4f\t%15.2f\n",
				ctx->options->buffer_sz,
				results->ops_enqueued,
				results->ops_dequeued,
				results->ops_enqueued_failed,
				results->ops_dequeued_failed,
				results->ops_per_second/1000000,
				results->throughput_gbps,
				results->cycles_per_byte);
	} else {
		if (!only_once)
			printf("\n# CPU lcore id, Burst Size(B), "
				"Buffer Size(B),Enqueued,Dequeued,Failed Enq,"
				"Failed Deq,Ops(Millions),Throughput(Gbps),"
				"Cycles Per Byte\n");
		only_once = 1;

		printf("%u;%u;%u;%"PRIu64";%"PRIu64";%"PRIu64";%"PRIu64";"
				"%.f3;%.f3;%.f3\n",
				ctx->lcore_id,
				ctx->options->burst_sz,
				ctx->options->buffer_sz,
				results->ops_enqueued,
				results->ops_dequeued,
				results->ops_enqueued_failed,
				results->ops_dequeued_failed,
				results->ops_per_second/1000000,
				results->throughput_gbps,
				results->cycles_per_byte);
	}

	cperf_throughput_test_free(ctx, ctx->options->pool_sz);
}
