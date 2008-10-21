
/*
 * Copyright (C) Maxim Dounin
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_uint_t                         max_cached;
    ngx_uint_t                         last_cached;

    ngx_connection_t                 **cached;

    ngx_http_upstream_init_pt          original_init_upstream;
    ngx_http_upstream_init_peer_pt     original_init_peer;

} ngx_http_upstream_keepalive_srv_conf_t;

typedef struct {
    ngx_http_upstream_keepalive_srv_conf_t  *conf;

    void                              *data;

    ngx_event_get_peer_pt              original_get_peer;
    ngx_event_free_peer_pt             original_free_peer;

} ngx_http_upstream_keepalive_peer_data_t;


static ngx_int_t ngx_http_upstream_init_keepalive_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_keepalive_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_free_keepalive_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);

static void ngx_http_upstream_keepalive_dummy_handler(ngx_event_t *ev);
static void ngx_http_upstream_keepalive_close_handler(ngx_event_t *ev);


static void *ngx_http_upstream_keepalive_create_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_keepalive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_http_upstream_keepalive_commands[] = {

    { ngx_string("keepalive"),
      NGX_HTTP_UPS_CONF|NGX_CONF_NOARGS,
      ngx_http_upstream_keepalive,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_keepalive_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    ngx_http_upstream_keepalive_create_conf, /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_keepalive_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_keepalive_module_ctx, /* module context */
    ngx_http_upstream_keepalive_commands,    /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


ngx_int_t
ngx_http_upstream_init_keepalive(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_keepalive_srv_conf_t  *kcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
                   "init keepalive");

    kcf = ngx_http_conf_upstream_srv_conf(us,
                                          ngx_http_upstream_keepalive_module);

    if (kcf->original_init_upstream(cf, us) != NGX_OK) {
        return NGX_ERROR;
    }

    kcf->original_init_peer = us->peer.init;

    us->peer.init = ngx_http_upstream_init_keepalive_peer;

    kcf->cached = ngx_pcalloc(cf->pool,
                              sizeof(ngx_connection_t *) * kcf->max_cached);
    if (kcf->cached == NULL) {
        return NGX_ERROR;
    }


    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_init_keepalive_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_keepalive_peer_data_t  *kp;
    ngx_http_upstream_keepalive_srv_conf_t   *kcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "init keepalive peer");

    kcf = ngx_http_conf_upstream_srv_conf(us,
                                          ngx_http_upstream_keepalive_module);

    kp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_keepalive_peer_data_t));
    if (kp == NULL) {
        return NGX_ERROR;
    }

    if (kcf->original_init_peer(r, us) != NGX_OK) {
        return NGX_ERROR;
    }

    kp->conf = kcf;
    kp->data = r->upstream->peer.data;
    kp->original_get_peer = r->upstream->peer.get;
    kp->original_free_peer = r->upstream->peer.free;

    r->upstream->peer.data = kp;
    r->upstream->peer.get = ngx_http_upstream_get_keepalive_peer;
    r->upstream->peer.free = ngx_http_upstream_free_keepalive_peer;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_get_keepalive_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_keepalive_peer_data_t  *kp = data;

    ngx_connection_t   *c;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get keepalive peer");

    /* XXX single pool of cached connections */

    if (kp->conf->last_cached) {

        c = kp->conf->cached[--kp->conf->last_cached];

        c->idle = 0;
        c->log = pc->log;
        c->read->log = pc->log;
        c->write->log = pc->log;

        pc->connection = c;
        pc->cached = 1;

        return NGX_DONE;
    }

    return kp->original_get_peer(pc, kp->data);
}


static void
ngx_http_upstream_free_keepalive_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_keepalive_peer_data_t  *kp = data;

    ngx_connection_t  *c;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "free keepalive peer");

    if (!(state & NGX_PEER_FAILED)
        && pc->connection != NULL
        && kp->conf->last_cached < kp->conf->max_cached)
    {
        c = pc->connection;

        kp->conf->cached[kp->conf->last_cached++] = c;
        pc->connection = NULL;

        if (c->read->timer_set) {
            ngx_del_timer(c->read);
        }
        if (c->write->timer_set) {
            ngx_del_timer(c->write);
        }

        c->write->handler = ngx_http_upstream_keepalive_dummy_handler;
        c->read->handler = ngx_http_upstream_keepalive_close_handler;

        c->data = kp->conf;
        c->idle = 1;
        c->log = ngx_cycle->log;
        c->read->log = ngx_cycle->log;
        c->write->log = ngx_cycle->log;
    }

    return kp->original_free_peer(pc, kp->data, state);
}


static void
ngx_http_upstream_keepalive_dummy_handler(ngx_event_t *ev)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0,
                   "keepalive dummy handler");
}


static void
ngx_http_upstream_keepalive_close_handler(ngx_event_t *ev)
{
    ngx_http_upstream_keepalive_srv_conf_t  *conf;

    ngx_uint_t         i;
    ngx_connection_t  *c;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0,
                   "keepalive close handler");

    c = ev->data;
    conf = c->data;

    for (i = 0; i < conf->last_cached; i++) {
        if (conf->cached[i] == c) {

            conf->cached[i] = conf->cached[--conf->last_cached];

            ngx_close_connection(c);
            return;
        }
    }

    ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
                  "keepalive close handler: unknown connection %p", c);
    ngx_close_connection(c);
}


static void *
ngx_http_upstream_keepalive_create_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_keepalive_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool,
                       sizeof(ngx_http_upstream_keepalive_srv_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->last_cached = 0;
     *     conf->original_init_upstream = NULL;
     *     conf->original_init_peer = NULL;
     */

    conf->max_cached = 1;

    return conf;
}


static char *
ngx_http_upstream_keepalive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t            *uscf;
    ngx_http_upstream_keepalive_srv_conf_t  *kcf;

    ngx_int_t    n;
    ngx_str_t   *value;
    ngx_uint_t   i;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    kcf = ngx_http_conf_upstream_srv_conf(uscf,
                                          ngx_http_upstream_keepalive_module);

    kcf->original_init_upstream = uscf->peer.init_upstream
                                  ? uscf->peer.init_upstream
                                  : ngx_http_upstream_init_round_robin;

    uscf->peer.init_upstream = ngx_http_upstream_init_keepalive;

    /* read options */

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "cached=", 7) == 0) {
            n = ngx_atoi(&value[i].data[7], value[i].len - 7);

            if (n == NGX_ERROR) {
                goto invalid;
            }

            kcf->max_cached = n;

            continue;
        }

        goto invalid;
    }

    return NGX_CONF_OK;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}
