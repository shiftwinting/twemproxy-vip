/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <nc_core.h>
#include <nc_server.h>

struct stats_desc {
    char *name; /* stats name */
    char *desc; /* stats description */
};

#define DEFINE_ACTION(_name, _type, _desc) { .type = _type, .name = string(#_name) },
static struct stats_metric stats_pool_codec[] = {
    STATS_POOL_CODEC( DEFINE_ACTION )
};

static struct stats_metric stats_server_codec[] = {
    STATS_SERVER_CODEC( DEFINE_ACTION )
};
#undef DEFINE_ACTION

#define DEFINE_ACTION(_name, _type, _desc) { .name = #_name, .desc = _desc },
static struct stats_desc stats_pool_desc[] = {
    STATS_POOL_CODEC( DEFINE_ACTION )
};

static struct stats_desc stats_server_desc[] = {
    STATS_SERVER_CODEC( DEFINE_ACTION )
};
#undef DEFINE_ACTION

void
stats_describe(void)
{
    uint32_t i;

    log_stderr("pool stats:");
    for (i = 0; i < NELEMS(stats_pool_desc); i++) {
        log_stderr("  %-20s\"%s\"", stats_pool_desc[i].name,
                   stats_pool_desc[i].desc);
    }

    log_stderr("");

    log_stderr("server stats:");
    for (i = 0; i < NELEMS(stats_server_desc); i++) {
        log_stderr("  %-20s\"%s\"", stats_server_desc[i].name,
                   stats_server_desc[i].desc);
    }
}

static void
stats_metric_init(struct stats_metric *stm)
{
    switch (stm->type) {
    case STATS_COUNTER:
        stm->value.counter = 0LL;
        break;

    case STATS_GAUGE:
        stm->value.counter = 0LL;
        break;

    case STATS_TIMESTAMP:
        stm->value.timestamp = 0LL;
        break;

    default:
        NOT_REACHED();
    }
}

static void
stats_metric_reset(struct array *stats_metric)
{
    uint32_t i, nmetric;

    nmetric = array_n(stats_metric);
    ASSERT(nmetric == STATS_POOL_NFIELD || nmetric == STATS_SERVER_NFIELD);

    for (i = 0; i < nmetric; i++) {
        struct stats_metric *stm = array_get(stats_metric, i);

        stats_metric_init(stm);
    }
}

static rstatus_t
stats_pool_metric_init(struct array *stats_metric)
{
    rstatus_t status;
    uint32_t i, nfield = STATS_POOL_NFIELD;

    status = array_init(stats_metric, nfield, sizeof(struct stats_metric));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < nfield; i++) {
        struct stats_metric *stm = array_push(stats_metric);

        /* initialize from pool codec first */
        *stm = stats_pool_codec[i];

        /* initialize individual metric */
        stats_metric_init(stm);
    }

    return NC_OK;
}

static rstatus_t
stats_server_metric_init(struct stats_server *sts)
{
    rstatus_t status;
    uint32_t i, nfield = STATS_SERVER_NFIELD;

    status = array_init(&sts->metric, nfield, sizeof(struct stats_metric));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < nfield; i++) {
        struct stats_metric *stm = array_push(&sts->metric);

        /* initialize from server codec first */
        *stm = stats_server_codec[i];

        /* initialize individual metric */
        stats_metric_init(stm);
    }

    return NC_OK;
}

static void
stats_metric_deinit(struct array *metric)
{
    uint32_t i, nmetric;

    nmetric = array_n(metric);
    for (i = 0; i < nmetric; i++) {
        array_pop(metric);
    }
    array_deinit(metric);
}

static rstatus_t
stats_server_init(struct stats_server *sts, struct server *s)
{
    rstatus_t status;

    sts->name = s->name;
    array_null(&sts->metric);

    status = stats_server_metric_init(sts);
    if (status != NC_OK) {
        return status;
    }

    log_debug(LOG_VVVERB, "init stats server '%.*s' with %"PRIu32" metric",
              sts->name.len, sts->name.data, array_n(&sts->metric));

    return NC_OK;

}

static rstatus_t
stats_server_map(struct array *stats_server, struct array *server)
{
    rstatus_t status;
    uint32_t i, nserver;

    nserver = array_n(server);
    ASSERT(nserver != 0);

    status = array_init(stats_server, nserver, sizeof(struct stats_server));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < nserver; i++) {
        struct server *s = array_get(server, i);
        struct stats_server *sts = array_push(stats_server);

        status = stats_server_init(sts, s);
        if (status != NC_OK) {
            return status;
        }
    }

    log_debug(LOG_VVVERB, "map %"PRIu32" stats servers", nserver);

    return NC_OK;
}

static void
stats_server_unmap(struct array *stats_server)
{
    uint32_t i, nserver;

    nserver = array_n(stats_server);

    for (i = 0; i < nserver; i++) {
        struct stats_server *sts = array_pop(stats_server);
        stats_metric_deinit(&sts->metric);
    }
    array_deinit(stats_server);

    log_debug(LOG_VVVERB, "unmap %"PRIu32" stats servers", nserver);
}

static rstatus_t
stats_pool_init(struct stats_pool *stp, struct server_pool *sp)
{
    rstatus_t status;

    stp->name = sp->name;
    array_null(&stp->metric);
    array_null(&stp->server);

    status = stats_pool_metric_init(&stp->metric);
    if (status != NC_OK) {
        return status;
    }

    status = stats_server_map(&stp->server, &sp->server);
    if (status != NC_OK) {
        stats_metric_deinit(&stp->metric);
        return status;
    }

    log_debug(LOG_VVVERB, "init stats pool '%.*s' with %"PRIu32" metric and "
              "%"PRIu32" server", stp->name.len, stp->name.data,
              array_n(&stp->metric), array_n(&stp->metric));

    return NC_OK;
}

static void
stats_pool_reset(struct array *stats_pool)
{
    uint32_t i, npool;

    npool = array_n(stats_pool);

    for (i = 0; i < npool; i++) {
        struct stats_pool *stp = array_get(stats_pool, i);
        uint32_t j, nserver;

        stats_metric_reset(&stp->metric);

        nserver = array_n(&stp->server);
        for (j = 0; j < nserver; j++) {
            struct stats_server *sts = array_get(&stp->server, j);
            stats_metric_reset(&sts->metric);
        }
    }
}

static rstatus_t
stats_pool_map(struct array *stats_pool, struct array *server_pool)
{
    rstatus_t status;
    uint32_t i, npool;

    npool = array_n(server_pool);
    ASSERT(npool != 0);

    status = array_init(stats_pool, npool, sizeof(struct stats_pool));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < npool; i++) {
        struct server_pool *sp = array_get(server_pool, i);
        struct stats_pool *stp = array_push(stats_pool);

        status = stats_pool_init(stp, sp);
        if (status != NC_OK) {
            return status;
        }
    }

    log_debug(LOG_VVVERB, "map %"PRIu32" stats pools", npool);

    return NC_OK;
}

static void
stats_pool_unmap(struct array *stats_pool)
{
    uint32_t i, npool;

    npool = array_n(stats_pool);

    for (i = 0; i < npool; i++) {
        struct stats_pool *stp = array_pop(stats_pool);
        stats_metric_deinit(&stp->metric);
        stats_server_unmap(&stp->server);
    }
    array_deinit(stats_pool);

    log_debug(LOG_VVVERB, "unmap %"PRIu32" stats pool", npool);
}

static rstatus_t
stats_create_buf(struct stats *st)
{
    uint32_t int64_max_digits = 20; /* INT64_MAX = 9223372036854775807 */
    uint32_t key_value_extra = 8;   /* "key": "value", */
    uint32_t pool_extra = 8;        /* '"pool_name": { ' + ' }' */
    uint32_t server_extra = 8;      /* '"server_name": { ' + ' }' */
    size_t size = 0;
    uint32_t i;

    ASSERT(st->buf.data == NULL && st->buf.size == 0);

    /* header */
    size += 1;

    size += st->service_str.len;
    size += st->service.len;
    size += key_value_extra;

    size += st->source_str.len;
    size += st->source.len;
    size += key_value_extra;

    size += st->version_str.len;
    size += st->version.len;
    size += key_value_extra;

    size += st->uptime_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    size += st->timestamp_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    size += st->ntotal_conn_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    size += st->ncurr_conn_str.len;
    size += int64_max_digits;
    size += key_value_extra;

#if 1 //shenzheng 2015-7-9 proxy administer
	size += st->ncurr_conn_str_a.len;
    size += int64_max_digits;
    size += key_value_extra;
#endif //shenzheng 2015-7-9 proxy administer

#if 1 //shenzheng 2015-3-23 common
#ifdef NC_DEBUG_LOG
	size += st->ntotal_msg_str.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_msg_str.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->ntotal_mbuf_str.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_mbuf_str.len;
    size += int64_max_digits;
    size += key_value_extra;
#endif
#endif //shenzheng 2015-3-23 common

#if 1 //shenzheng 2015-7-9 proxy administer
#ifdef NC_DEBUG_LOG
	size += st->ntotal_msg_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_msg_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->ntotal_mbuf_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_mbuf_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;
#endif
#endif //shenzheng 2015-7-9 proxy administer

    /* server pools */
    for (i = 0; i < array_n(&st->sum); i++) {
        struct stats_pool *stp = array_get(&st->sum, i);
        uint32_t j;

        size += stp->name.len;
        size += pool_extra;

        for (j = 0; j < array_n(&stp->metric); j++) {
            struct stats_metric *stm = array_get(&stp->metric, j);

            size += stm->name.len;
            size += int64_max_digits;
            size += key_value_extra;
        }

        /* servers per pool */
        for (j = 0; j < array_n(&stp->server); j++) {
            struct stats_server *sts = array_get(&stp->server, j);
            uint32_t k;

            size += sts->name.len;
            size += server_extra;

            for (k = 0; k < array_n(&sts->metric); k++) {
                struct stats_metric *stm = array_get(&sts->metric, k);

                size += stm->name.len;
                size += int64_max_digits;
                size += key_value_extra;
            }
        }
    }

    /* footer */
    size += 2;

    size = NC_ALIGN(size, NC_ALIGNMENT);

    st->buf.data = nc_alloc(size);
    if (st->buf.data == NULL) {
        log_error("create stats buffer of size %zu failed: %s", size,
                   strerror(errno));
        return NC_ENOMEM;
    }
    st->buf.size = size;

    log_debug(LOG_DEBUG, "stats buffer size %zu", size);

    return NC_OK;
}

static void
stats_destroy_buf(struct stats *st)
{
    if (st->buf.size != 0) {
        ASSERT(st->buf.data != NULL);
        nc_free(st->buf.data);
        st->buf.size = 0;
    }
}

static rstatus_t
stats_add_string(struct stats *st, struct string *key, struct string *val)
{
    struct stats_buffer *buf;
    uint8_t *pos;
    size_t room;
    int n;

    buf = &st->buf;
    pos = buf->data + buf->len;
    room = buf->size - buf->len - 1;

    n = nc_snprintf(pos, room, "\"%.*s\":\"%.*s\", ", key->len, key->data,
                    val->len, val->data);
    if (n < 0 || n >= (int)room) {
        return NC_ERROR;
    }

    buf->len += (size_t)n;

    return NC_OK;
}

static rstatus_t
stats_add_num(struct stats *st, struct string *key, int64_t val)
{
    struct stats_buffer *buf;
    uint8_t *pos;
    size_t room;
    int n;

    buf = &st->buf;
    pos = buf->data + buf->len;
    room = buf->size - buf->len - 1;

    n = nc_snprintf(pos, room, "\"%.*s\":%"PRId64", ", key->len, key->data,
                    val);
    if (n < 0 || n >= (int)room) {
        return NC_ERROR;
    }

    buf->len += (size_t)n;

    return NC_OK;
}

static rstatus_t
stats_add_header(struct stats *st)
{
    rstatus_t status;
    struct stats_buffer *buf;
    int64_t cur_ts, uptime;

    buf = &st->buf;
    buf->data[0] = '{';
    buf->len = 1;

    cur_ts = (int64_t)time(NULL);
    uptime = cur_ts - st->start_ts;

    status = stats_add_string(st, &st->service_str, &st->service);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_string(st, &st->source_str, &st->source);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_string(st, &st->version_str, &st->version);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_num(st, &st->uptime_str, uptime);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_num(st, &st->timestamp_str, cur_ts);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_num(st, &st->ntotal_conn_str, conn_ntotal_conn());
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_num(st, &st->ncurr_conn_str, conn_ncurr_conn());
    if (status != NC_OK) {
        return status;
    }

#if 1 //shenzheng 2015-7-9 proxy administer
	status = stats_add_num(st, &st->ncurr_conn_str_a, conn_ncurr_conn_proxy_adm());
    if (status != NC_OK) {
        return status;
    }
#endif //shenzheng 2015-7-9 proxy administer

#if 1 //shenzheng 2015-3-23 common
#ifdef NC_DEBUG_LOG
	status = stats_add_num(st, &st->ntotal_msg_str, msg_ntotal_msg());
    if (status != NC_OK) {
        return status;
    }

	status = stats_add_num(st, &st->nfree_msg_str, msg_nfree_msg());
    if (status != NC_OK) {
        return status;
    }

	status = stats_add_num(st, &st->ntotal_mbuf_str, mbuf_ntotal_mbuf());
    if (status != NC_OK) {
        return status;
    }

	status = stats_add_num(st, &st->nfree_mbuf_str, mbuf_nfree_mbuf());
    if (status != NC_OK) {
        return status;
    }
#endif
#endif //shenzheng 2015-3-23 common

#if 1 //shenzheng 2015-7-9 proxy administer
#ifdef NC_DEBUG_LOG
	status = stats_add_num(st, &st->ntotal_msg_str_proxy_adm, msg_ntotal_msg_proxy_adm());
    if (status != NC_OK) {
        return status;
    }

	status = stats_add_num(st, &st->nfree_msg_str_proxy_adm, msg_nfree_msg_proxy_adm());
    if (status != NC_OK) {
        return status;
    }

	status = stats_add_num(st, &st->ntotal_mbuf_str_proxy_adm, mbuf_ntotal_mbuf_proxy_adm());
    if (status != NC_OK) {
        return status;
    }

	status = stats_add_num(st, &st->nfree_mbuf_str_proxy_adm, mbuf_nfree_mbuf_proxy_adm());
    if (status != NC_OK) {
        return status;
    }
#endif
#endif //shenzheng 2015-7-9 proxy administer

    return NC_OK;
}

static rstatus_t
stats_add_footer(struct stats *st)
{
    struct stats_buffer *buf;
    uint8_t *pos;

    buf = &st->buf;

    if (buf->len == buf->size) {
        return NC_ERROR;
    }

    /* overwrite the last byte and add a new byte */
    pos = buf->data + buf->len - 1;
    pos[0] = '}';
    pos[1] = '\n';
    buf->len += 1;

    return NC_OK;
}

static rstatus_t
stats_begin_nesting(struct stats *st, struct string *key)
{
    struct stats_buffer *buf;
    uint8_t *pos;
    size_t room;
    int n;

    buf = &st->buf;
    pos = buf->data + buf->len;
    room = buf->size - buf->len - 1;

    n = nc_snprintf(pos, room, "\"%.*s\": {", key->len, key->data);
    if (n < 0 || n >= (int)room) {
        return NC_ERROR;
    }

    buf->len += (size_t)n;

    return NC_OK;
}

static rstatus_t
stats_end_nesting(struct stats *st)
{
    struct stats_buffer *buf;
    uint8_t *pos;

    buf = &st->buf;
    pos = buf->data + buf->len;

    pos -= 2; /* go back by 2 bytes */

    switch (pos[0]) {
    case ',':
        /* overwrite last two bytes; len remains unchanged */
        ASSERT(pos[1] == ' ');
        pos[0] = '}';
        pos[1] = ',';
        break;

    case '}':
        if (buf->len == buf->size) {
            return NC_ERROR;
        }
        /* overwrite the last byte and add a new byte */
        ASSERT(pos[1] == ',');
        pos[1] = '}';
        pos[2] = ',';
        buf->len += 1;
        break;

    default:
        NOT_REACHED();
    }

    return NC_OK;
}

static rstatus_t
stats_copy_metric(struct stats *st, struct array *metric)
{
    rstatus_t status;
    uint32_t i;

    for (i = 0; i < array_n(metric); i++) {
        struct stats_metric *stm = array_get(metric, i);

        status = stats_add_num(st, &stm->name, stm->value.counter);
        if (status != NC_OK) {
            return status;
        }
    }

    return NC_OK;
}

static void
stats_aggregate_metric(struct array *dst, struct array *src)
{
    uint32_t i;

    for (i = 0; i < array_n(src); i++) {
        struct stats_metric *stm1, *stm2;

        stm1 = array_get(src, i);
        stm2 = array_get(dst, i);

        ASSERT(stm1->type == stm2->type);

        switch (stm1->type) {
        case STATS_COUNTER:
            stm2->value.counter += stm1->value.counter;
            break;

        case STATS_GAUGE:
            stm2->value.counter += stm1->value.counter;
#if 1 //shenzheng 2015-5-29 config-reload
			if(stm2->value.counter <= 0)
			{
				stm2->value.counter = 0;
			}
#endif //shenzheng 2015-5-29 config-reload
            break;

        case STATS_TIMESTAMP:
            if (stm1->value.timestamp) {
                stm2->value.timestamp = stm1->value.timestamp;
            }
            break;

        default:
            NOT_REACHED();
        }
    }
}

static void
stats_aggregate(struct stats *st)
{
    uint32_t i;

    if (st->aggregate == 0) {
        log_debug(LOG_PVERB, "skip aggregate of shadow %p to sum %p as "
                  "generator is slow", st->shadow.elem, st->sum.elem);
        return;
    }

    log_debug(LOG_PVERB, "aggregate stats shadow %p to sum %p", st->shadow.elem,
              st->sum.elem);

    for (i = 0; i < array_n(&st->shadow); i++) {
        struct stats_pool *stp1, *stp2;
        uint32_t j;

        stp1 = array_get(&st->shadow, i);
        stp2 = array_get(&st->sum, i);
        stats_aggregate_metric(&stp2->metric, &stp1->metric);

        for (j = 0; j < array_n(&stp1->server); j++) {
            struct stats_server *sts1, *sts2;

            sts1 = array_get(&stp1->server, j);
            sts2 = array_get(&stp2->server, j);
            stats_aggregate_metric(&sts2->metric, &sts1->metric);
        }
    }

    st->aggregate = 0;
}

static rstatus_t
stats_make_rsp(struct stats *st)
{
    rstatus_t status;
    uint32_t i;

    status = stats_add_header(st);
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < array_n(&st->sum); i++) {
        struct stats_pool *stp = array_get(&st->sum, i);
        uint32_t j;

        status = stats_begin_nesting(st, &stp->name);
        if (status != NC_OK) {
            return status;
        }

        /* copy pool metric from sum(c) to buffer */
        status = stats_copy_metric(st, &stp->metric);
        if (status != NC_OK) {
            return status;
        }

        for (j = 0; j < array_n(&stp->server); j++) {
            struct stats_server *sts = array_get(&stp->server, j);

            status = stats_begin_nesting(st, &sts->name);
            if (status != NC_OK) {
                return status;
            }

            /* copy server metric from sum(c) to buffer */
            status = stats_copy_metric(st, &sts->metric);
            if (status != NC_OK) {
                return status;
            }

            status = stats_end_nesting(st);
            if (status != NC_OK) {
                return status;
            }
        }

        status = stats_end_nesting(st);
        if (status != NC_OK) {
            return status;
        }
    }

    status = stats_add_footer(st);
    if (status != NC_OK) {
        return status;
    }

    return NC_OK;
}

static rstatus_t
stats_send_rsp(struct stats *st)
{
    rstatus_t status;
    ssize_t n;
    int sd;

    status = stats_make_rsp(st);
    if (status != NC_OK) {
        return status;
    }

    sd = accept(st->sd, NULL, NULL);
    if (sd < 0) {
        log_error("accept on m %d failed: %s", st->sd, strerror(errno));
        return NC_ERROR;
    }

    log_debug(LOG_VERB, "send stats on sd %d %d bytes", sd, st->buf.len);

    n = nc_sendn(sd, st->buf.data, st->buf.len);
    
	if (n < 0) {
        log_error("send stats on sd %d failed: %s", sd, strerror(errno));
        close(sd);
        return NC_ERROR;
    }
	close(sd);

    return NC_OK;
}

static void
stats_loop_callback(void *arg1, void *arg2)
{
    struct stats *st = arg1;
    int n = *((int *)arg2);

#if 1 //shenzheng 2015-5-14 config-reload
	bool flag = false;

	if(st->pause)
	{		
		if(st->reload_thread)
		{
			rstatus_t status;
			struct context *ctx;
			struct array *pools_curr;

			ctx = st->ctx;

			ASSERT(ctx != NULL);

			if(ctx->which_pool)
			{
				pools_curr = &ctx->pool_swap;
			}
			else
			{
				pools_curr = &ctx->pool;
			}
			
			status = stats_recreate(ctx, pools_curr);
			if(status != NC_OK)
			{
				log_error("error: stats_recreate error.");
				return;
			}
		
			stats_recreate_buf(st);
			st->reload_thread = 0;
			st->pause = 0;
		}
	
		if(n == 0)
		{
			return;
		}
	
	    int sd;
		sd = accept(st->sd, NULL, NULL);
    	if (sd < 0) {
	        log_error("accept on m %d failed: %s", st->sd, strerror(errno));
	        return;
	    }

	    log_debug(LOG_VERB, "send stats on sd %d %d bytes", sd, st->buf.len);

	    n = nc_sendn(sd, st->buf.data, st->buf.len);
	    
		if (n < 0) {
	        log_error("send stats on sd %d failed: %s", sd, strerror(errno));
	        close(sd);
	        return;
	    }
		close(sd);

		return;
	}

	if(st->reload_thread)
	{
		//stats_recreate_buf(st);
		//flag = true;
		//st->reload_thread = 0;
		//st->pause = 0;
	}
#endif //shenzheng 2015-5-16 config-reload
    /* aggregate stats from shadow (b) -> sum (c) */
    stats_aggregate(st);
	
    if (n == 0) {
        return;
    }

    /* send aggregate stats sum (c) to collector */
    stats_send_rsp(st);
	
#if 0 //shenzheng 2015-7-16 config-reload
	if(st->reload_thread && flag == true)
	{
		st->reload_thread = 0;
	}
#endif //shenzheng 2015-7-16 config-reload

}

static void *
stats_loop(void *arg)
{
    event_loop_stats(stats_loop_callback, arg);
    return NULL;
}

static rstatus_t
stats_listen(struct stats *st)
{
    rstatus_t status;
    struct sockinfo si;

    status = nc_resolve(&st->addr, st->port, &si);
    if (status < 0) {
        return status;
    }

    st->sd = socket(si.family, SOCK_STREAM, 0);
    if (st->sd < 0) {
        log_error("socket failed: %s", strerror(errno));
        return NC_ERROR;
    }

    status = nc_set_reuseaddr(st->sd);
    if (status < 0) {
        log_error("set reuseaddr on m %d failed: %s", st->sd, strerror(errno));
        return NC_ERROR;
    }

    status = bind(st->sd, (struct sockaddr *)&si.addr, si.addrlen);
    if (status < 0) {
        log_error("bind on m %d to addr '%.*s:%u' failed: %s", st->sd,
                  st->addr.len, st->addr.data, st->port, strerror(errno));
        return NC_ERROR;
    }

    status = listen(st->sd, SOMAXCONN);
    if (status < 0) {
        log_error("listen on m %d failed: %s", st->sd, strerror(errno));
        return NC_ERROR;
    }

    log_debug(LOG_NOTICE, "m %d listening on '%.*s:%u'", st->sd,
              st->addr.len, st->addr.data, st->port);

    return NC_OK;
}

static rstatus_t
stats_start_aggregator(struct stats *st)
{
    rstatus_t status;

    if (!stats_enabled) {
        return NC_OK;
    }

    status = stats_listen(st);
    if (status != NC_OK) {
        return status;
    }

    status = pthread_create(&st->tid, NULL, stats_loop, st);
    if (status < 0) {
        log_error("stats aggregator create failed: %s", strerror(status));
        return NC_ERROR;
    }

    return NC_OK;
}

static void
stats_stop_aggregator(struct stats *st)
{
    if (!stats_enabled) {
        return;
    }

    close(st->sd);
}

struct stats *
stats_create(uint16_t stats_port, char *stats_ip, int stats_interval,
             char *source, struct array *server_pool)
{
    rstatus_t status;
    struct stats *st;

    st = nc_alloc(sizeof(*st));
    if (st == NULL) {
        return NULL;
    }

    st->port = stats_port;
    st->interval = stats_interval;
    string_set_raw(&st->addr, stats_ip);

    st->start_ts = (int64_t)time(NULL);

    st->buf.len = 0;
    st->buf.data = NULL;
    st->buf.size = 0;

    array_null(&st->current);
    array_null(&st->shadow);
    array_null(&st->sum);

    st->tid = (pthread_t) -1;
    st->sd = -1;

    string_set_text(&st->service_str, "service");
    string_set_text(&st->service, "nutcracker");

    string_set_text(&st->source_str, "source");
    string_set_raw(&st->source, source);

    string_set_text(&st->version_str, "version");
    string_set_text(&st->version, NC_VERSION_STRING);

    string_set_text(&st->uptime_str, "uptime");
    string_set_text(&st->timestamp_str, "timestamp");

    string_set_text(&st->ntotal_conn_str, "total_connections");
    string_set_text(&st->ncurr_conn_str, "curr_connections");

#if 1 //shenzheng 2015-7-9 proxy administer
	string_set_text(&st->ncurr_conn_str_a, "curr_connections_a");
#endif //shenzheng 2015-7-9 proxy administer

#if 1 //shenzheng 2015-3-23 common
#ifdef NC_DEBUG_LOG
	string_set_text(&st->ntotal_msg_str, "total_msgs");
    string_set_text(&st->nfree_msg_str, "free_msgs");
	string_set_text(&st->ntotal_mbuf_str, "total_mbufs");
    string_set_text(&st->nfree_mbuf_str, "free_mbufs");
#endif
#endif //shenzheng 2015-3-23 common

#if 1 //shenzheng 2015-7-9 proxy administer
#ifdef NC_DEBUG_LOG
	string_set_text(&st->ntotal_msg_str_proxy_adm, "total_msgs_a");
    string_set_text(&st->nfree_msg_str_proxy_adm, "free_msgs_a");
	string_set_text(&st->ntotal_mbuf_str_proxy_adm, "total_mbufs_a");
    string_set_text(&st->nfree_mbuf_str_proxy_adm, "free_mbufs_a");
#endif
#endif //shenzheng 2015-7-9 proxy administer

    st->updated = 0;
    st->aggregate = 0;

    /* map server pool to current (a), shadow (b) and sum (c) */

    status = stats_pool_map(&st->current, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->shadow, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->sum, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_create_buf(st);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_start_aggregator(st);
    if (status != NC_OK) {
        goto error;
    }

#if 1 //shenzheng 2015-5-14 config-reload
	st->reload_thread = 0;
	st->pause = 0;
	st->ctx = NULL;
#endif //shenzheng 2015-7-16 config-reload

    return st;

error:
    stats_destroy(st);
    return NULL;
}

void
stats_destroy(struct stats *st)
{
    stats_stop_aggregator(st);
    stats_pool_unmap(&st->sum);
    stats_pool_unmap(&st->shadow);
    stats_pool_unmap(&st->current);
    stats_destroy_buf(st);
    nc_free(st);
}

void
stats_swap(struct stats *st)
{
    if (!stats_enabled) {
        return;
    }

#if 1 //shenzheng 2015-5-15 config-reload
	if(st->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-15 config-reload

    if (st->aggregate == 1) {
        log_debug(LOG_PVERB, "skip swap of current %p shadow %p as aggregator "
                  "is busy", st->current.elem, st->shadow.elem);
        return;
    }

    if (st->updated == 0) {
        log_debug(LOG_PVERB, "skip swap of current %p shadow %p as there is "
                  "nothing new", st->current.elem, st->shadow.elem);
        return;
    }

    log_debug(LOG_PVERB, "swap stats current %p shadow %p", st->current.elem,
              st->shadow.elem);

    array_swap(&st->current, &st->shadow);

    /*
     * Reset current (a) stats before giving it back to generator to keep
     * stats addition idempotent
     */
    stats_pool_reset(&st->current);
    st->updated = 0;

    st->aggregate = 1;
}

static struct stats_metric *
stats_pool_to_metric(struct context *ctx, struct server_pool *pool,
                     stats_pool_field_t fidx)
{
    struct stats *st;
    struct stats_pool *stp;
    struct stats_metric *stm;
    uint32_t pidx;

    pidx = pool->idx;

    st = ctx->stats;
    stp = array_get(&st->current, pidx);
    stm = array_get(&stp->metric, fidx);

    st->updated = 1;

    log_debug(LOG_VVVERB, "metric '%.*s' in pool %"PRIu32"", stm->name.len,
              stm->name.data, pidx);

    return stm;
}

void
_stats_pool_incr(struct context *ctx, struct server_pool *pool,
                 stats_pool_field_t fidx)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter++;

    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_pool_decr(struct context *ctx, struct server_pool *pool,
                 stats_pool_field_t fidx)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter--;

    log_debug(LOG_VVVERB, "decr field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_pool_incr_by(struct context *ctx, struct server_pool *pool,
                    stats_pool_field_t fidx, int64_t val)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter += val;

    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_pool_decr_by(struct context *ctx, struct server_pool *pool,
                    stats_pool_field_t fidx, int64_t val)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter -= val;
	
    log_debug(LOG_VVVERB, "decr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_pool_set_ts(struct context *ctx, struct server_pool *pool,
                   stats_pool_field_t fidx, int64_t val)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_TIMESTAMP);
    stm->value.timestamp = val;

    log_debug(LOG_VVVERB, "set ts field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.timestamp);
}

static struct stats_metric *
stats_server_to_metric(struct context *ctx, struct server *server,
                       stats_server_field_t fidx)
{
    struct stats *st;
    struct stats_pool *stp;
    struct stats_server *sts;
    struct stats_metric *stm;
    uint32_t pidx, sidx;

    sidx = server->idx;
    pidx = server->owner->idx;

    st = ctx->stats;
    stp = array_get(&st->current, pidx);
    sts = array_get(&stp->server, sidx);
    stm = array_get(&sts->metric, fidx);

    st->updated = 1;

    log_debug(LOG_VVVERB, "metric '%.*s' in pool %"PRIu32" server %"PRIu32"",
              stm->name.len, stm->name.data, pidx, sidx);

    return stm;
}

void
_stats_server_incr(struct context *ctx, struct server *server,
                   stats_server_field_t fidx)
{
    struct stats_metric *stm;

#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter++;

    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_server_decr(struct context *ctx, struct server *server,
                   stats_server_field_t fidx)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter--;

    log_debug(LOG_VVVERB, "decr field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_server_incr_by(struct context *ctx, struct server *server,
                      stats_server_field_t fidx, int64_t val)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter += val;

    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_server_decr_by(struct context *ctx, struct server *server,
                      stats_server_field_t fidx, int64_t val)
{
    struct stats_metric *stm;
		
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter -= val;

    log_debug(LOG_VVVERB, "decr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_server_set_ts(struct context *ctx, struct server *server,
                     stats_server_field_t fidx, int64_t val)
{
    struct stats_metric *stm;
	
#if 1 //shenzheng 2015-5-14 config-reload
	if(ctx->stats->pause)
	{
		return;
	}
#endif //shenzheng 2015-5-14 config-reload

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_TIMESTAMP);
    stm->value.timestamp = val;

    log_debug(LOG_VVVERB, "set ts field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.timestamp);
}

#if 1 //shenzheng 2015-6-11 config-reload
void
_stats_pool_incr_by_anyway(struct context *ctx, struct server_pool *pool,
                    stats_pool_field_t fidx, int64_t val)
{
    struct stats_metric *stm;

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter += val;

    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_server_incr_by_anyway(struct context *ctx, struct server *server,
                      stats_server_field_t fidx, int64_t val)
{
    struct stats_metric *stm;

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter += val;

    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

#endif //shenzheng 2015-6-11 config-reload

#if 1 //shenzheng 2015-5-14 config-reload
struct stats *
stats_recreate_old(struct context *ctx, struct array *server_pool)
{
    rstatus_t status;
    struct stats *st, *st_curr;
	st_curr = ctx->stats;
	//st = ctx->stats_swap;
	ASSERT(st_curr != NULL);

	if(st == NULL)
	{
		st = nc_alloc(sizeof(*st));
		st->port = st_curr->port;
	    st->interval = st_curr->interval;
		st->addr = st_curr->addr;

		st->tid = st_curr->tid;
		st->sd = st_curr->sd;
		//st->owner = ctx;
	}
	else
	{
		stats_pool_unmap(&st->sum);
	    stats_pool_unmap(&st->shadow);
	    stats_pool_unmap(&st->current);
	    stats_destroy_buf(st);

		ASSERT(st->port == st_curr->port);
		ASSERT(st->interval == st_curr->interval);
		ASSERT(st->addr.data == st_curr->addr.data);
		ASSERT(st->addr.len == st_curr->addr.len);
		ASSERT(st->tid == st_curr->tid);
		ASSERT(st->sd == st_curr->sd);
		//ASSERT(st->owner == ctx);
	}

    if (st == NULL) {
        return NULL;
    }

    st->start_ts = (int64_t)time(NULL);

    st->buf.len = 0;
    st->buf.data = NULL;
    st->buf.size = 0;

    array_null(&st->current);
    array_null(&st->shadow);
    array_null(&st->sum);

    string_set_text(&st->service_str, "service");
    string_set_text(&st->service, "nutcracker");

    string_set_text(&st->source_str, "source");
    //string_set_raw(&st->source, st_curr->source);
    st->source = st_curr->source;

    string_set_text(&st->version_str, "version");
    string_set_text(&st->version, NC_VERSION_STRING);

    string_set_text(&st->uptime_str, "uptime");
    string_set_text(&st->timestamp_str, "timestamp");

    string_set_text(&st->ntotal_conn_str, "total_connections");
    string_set_text(&st->ncurr_conn_str, "curr_connections");
	
#if 1 //shenzheng 2015-7-9 proxy administer
	string_set_text(&st->ncurr_conn_str_a, "curr_connections_a");
#endif //shenzheng 2015-7-9 proxy administer

#if 1 //shenzheng 2015-3-23 common
#ifdef NC_DEBUG_LOG
	string_set_text(&st->ntotal_msg_str, "total_msgs");
    string_set_text(&st->nfree_msg_str, "free_msgs");
	string_set_text(&st->ntotal_mbuf_str, "total_mbufs");
    string_set_text(&st->nfree_mbuf_str, "free_mbufs");
#endif
#endif //shenzheng 2015-3-23 common

#if 1 //shenzheng 2015-7-9 proxy administer
#ifdef NC_DEBUG_LOG
	string_set_text(&st->ntotal_msg_str_proxy_adm, "total_msgs_a");
	string_set_text(&st->nfree_msg_str_proxy_adm, "free_msgs_a");
	string_set_text(&st->ntotal_mbuf_str_proxy_adm, "total_mbufs_a");
	string_set_text(&st->nfree_mbuf_str_proxy_adm, "free_mbufs_a");
#endif
#endif //shenzheng 2015-7-9 proxy administer

    st->updated = 0;
    st->aggregate = 0;

    /* map server pool to current (a), shadow (b) and sum (c) */

    status = stats_pool_map(&st->current, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->shadow, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->sum, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_create_buf(st);
    if (status != NC_OK) {
        goto error;
    }

    //status = stats_start_aggregator(st);
    //if (status != NC_OK) {
    //    goto error;
    //}

    return st;

error:
	st->sd = -1;
    stats_destroy(st);
    return NULL;
}

rstatus_t
stats_recreate_buf(struct stats *st)
{
    uint32_t int64_max_digits = 20; /* INT64_MAX = 9223372036854775807 */
    uint32_t key_value_extra = 8;   /* "key": "value", */
    uint32_t pool_extra = 8;        /* '"pool_name": { ' + ' }' */
    uint32_t server_extra = 8;      /* '"server_name": { ' + ' }' */
    size_t size = 0;
    uint32_t i;

    ASSERT(st->buf.data != NULL && st->buf.size != 0);

	stats_destroy_buf(st);

    /* header */
    size += 1;

    size += st->service_str.len;
    size += st->service.len;
    size += key_value_extra;

    size += st->source_str.len;
    size += st->source.len;
    size += key_value_extra;

    size += st->version_str.len;
    size += st->version.len;
    size += key_value_extra;

    size += st->uptime_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    size += st->timestamp_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    size += st->ntotal_conn_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    size += st->ncurr_conn_str.len;
    size += int64_max_digits;
    size += key_value_extra;

#if 1 //shenzheng 2015-7-9 proxy administer
	size += st->ncurr_conn_str_a.len;
    size += int64_max_digits;
    size += key_value_extra;
#endif //shenzheng 2015-7-9 proxy administer

#if 1 //shenzheng 2015-3-23 common
#ifdef NC_DEBUG_LOG
	size += st->ntotal_msg_str.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_msg_str.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->ntotal_mbuf_str.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_mbuf_str.len;
    size += int64_max_digits;
    size += key_value_extra;
#endif
#endif //shenzheng 2015-3-23 common

#if 1 //shenzheng 2015-7-9 proxy administer
#ifdef NC_DEBUG_LOG
	size += st->ntotal_msg_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_msg_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->ntotal_mbuf_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;

	size += st->nfree_mbuf_str_proxy_adm.len;
    size += int64_max_digits;
    size += key_value_extra;
#endif
#endif //shenzheng 2015-7-9 proxy administer

    /* server pools */
    for (i = 0; i < array_n(&st->sum); i++) {
        struct stats_pool *stp = array_get(&st->sum, i);
        uint32_t j;

        size += stp->name.len;
        size += pool_extra;

        for (j = 0; j < array_n(&stp->metric); j++) {
            struct stats_metric *stm = array_get(&stp->metric, j);

            size += stm->name.len;
            size += int64_max_digits;
            size += key_value_extra;
        }

        /* servers per pool */
        for (j = 0; j < array_n(&stp->server); j++) {
            struct stats_server *sts = array_get(&stp->server, j);
            uint32_t k;

            size += sts->name.len;
            size += server_extra;

            for (k = 0; k < array_n(&sts->metric); k++) {
                struct stats_metric *stm = array_get(&sts->metric, k);

                size += stm->name.len;
                size += int64_max_digits;
                size += key_value_extra;
            }
        }
    }

    /* footer */
    size += 2;

    size = NC_ALIGN(size, NC_ALIGNMENT);

    st->buf.data = nc_alloc(size);
    if (st->buf.data == NULL) {
        log_error("create stats buffer of size %zu failed: %s", size,
                   strerror(errno));
        return NC_ENOMEM;
    }
    st->buf.size = size;

    log_debug(LOG_DEBUG, "stats buffer size %zu", size);

	stats_make_rsp(st);

    return NC_OK;
}

rstatus_t
stats_recreate(struct context *ctx, struct array *server_pool)
{
    rstatus_t status;
	uint32_t i, j, nsp, nser;
	int64_t count = 0;
	struct server_pool *sp;
	struct array *servers;
	struct server *ser;
	struct conn *conn_s;
    struct stats *st;
	struct stats_buffer *st_b;

	st = ctx->stats;

    ASSERT(st != NULL);

    st->start_ts = (int64_t)time(NULL);

    st->updated = 0;
    st->aggregate = 0;

	stats_pool_unmap(&st->sum);
    stats_pool_unmap(&st->shadow);
    stats_pool_unmap(&st->current);

    /* map server pool to current (a), shadow (b) and sum (c) */

    status = stats_pool_map(&st->current, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->shadow, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->sum, server_pool);
    if (status != NC_OK) {
        goto error;
    }

	//add exits stats
	for (i = 0, nsp = array_n(server_pool); i < nsp; i++)
	{
		sp = array_get(server_pool, i);
		log_debug(LOG_DEBUG, "server_pool(%s)'s client_connections : %d",
			sp->name.data, sp->nc_conn_q);
		stats_pool_incr_by_anyway(ctx, sp, client_connections, sp->nc_conn_q);
		servers = &(sp->server);
		for(j = 0, nser = array_n(servers); j <nser; j++)
		{
			ser = array_get(servers, j);
			log_debug(LOG_DEBUG, "server(%s)'s connections : %d", 
				ser->name.data, ser->ns_conn_q);
			count = 0;
			TAILQ_FOREACH(conn_s, &ser->s_conn_q, conn_tqe)
			{
				ASSERT(conn_s != NULL);
				if(conn_s->connected)
				{
					count ++;
				}
			}
			stats_server_incr_by_anyway(ctx, ser, server_connections, count);
		}
		
	}

    return NC_OK;

error:
    //stats_destroy(st);
    return status;
}

#endif //shenzheng 2015-5-14 config-reload

