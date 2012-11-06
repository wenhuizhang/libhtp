
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"

#include "htp.h"
#include "htp_hybrid.h"

// XXX Handle all allocation failures

static int convert_method_number(int method_number) {
    // We can cheat here because LibHTP reuses Apache's
    // method identifiers. But we really shouldn't.
    if ((method_number >= 0)&&(method_number <= 26)) {
        return method_number;
    }
    
    // TODO Decouple this functions from Apache's internals.

    return HTP_M_UNKNOWN;
}

static int convert_protocol_number(int protocol_number) {
    // In Apache, 1.1 is stored as 1001. In LibHTP,
    // the same protocol number is stored as 101.
    return (protocol_number / 1000) * 100 + (protocol_number % 1000);
}

static apr_status_t transaction_cleanup(htp_tx_t *tx) {
    htp_tx_destroy(tx);
    return APR_SUCCESS;
}

static int libhtp_post_request(request_rec *r) {
    htp_connp_t *connp = ap_get_module_config(r->connection->conn_config, &libhtp_module);
    if (connp == NULL) return DECLINED;

    // Create a new LibHTP transaction
    htp_tx_t *tx = htp_txh_create(connp);
    if (tx == NULL) return DECLINED;

    // Transaction lifecycle begins
    htp_txh_state_transaction_start(tx);

    // Populate request line
    htp_txh_req_set_method_c(tx, r->method, ALLOC_REUSE);
    htp_txh_req_set_method_number(tx, convert_method_number(r->method_number));
    htp_txh_req_set_uri_c(tx, r->uri, ALLOC_REUSE);
    htp_txh_req_set_query_string_c(tx, r->args, ALLOC_REUSE);
    htp_txh_req_set_protocol_c(tx, r->protocol, ALLOC_REUSE);
    htp_txh_req_set_protocol_number(tx, convert_protocol_number(r->proto_num));

    if (r->assbackwards) {
        htp_txh_req_set_protocol_http_0_9(tx, 1);
    } else {
        htp_txh_req_set_protocol_http_0_9(tx, 0);
    }

    // Request line available
    htp_txh_state_request_line(tx);

    // Populate request headers
    size_t i;
    const apr_array_header_t *arr = apr_table_elts(r->headers_in);
    const apr_table_entry_t *te = (apr_table_entry_t *) arr->elts;
    for (i = 0; i < arr->nelts; i++) {
        htp_txh_req_set_header_c(tx, te[i].key, te[i].val, ALLOC_REUSE);
    }

    // Request headers available
    htp_txh_state_request_headers(tx);

    // Attach LibHTP's transaction to Apache's request
    ap_set_module_config(r->request_config, &libhtp_module, tx);
    apr_pool_cleanup_register(r->pool, transaction_cleanup, tx);

    return DECLINED;
}

static apr_status_t connection_cleanup(htp_connp_t *connp) {
    htp_config_destroy(connp->cfg);
    htp_connp_destroy(connp);
    
    return APR_SUCCESS;
}

static int libhtp_pre_conn(conn_rec *c, void *csd) {
    // Configuration; normally you'd read the configuration from
    // a file, or some other type of storage, but, because this is
    // just an example, we have it hard-coded.
    htp_cfg_t *cfg = htp_config_create();
    if (cfg == NULL) return OK;
    htp_config_set_server_personality(cfg, HTP_SERVER_APACHE_2_2);
    htp_config_register_urlencoded_parser(cfg);
    htp_config_register_multipart_parser(cfg);

    // Connection parser
    htp_connp_t *connp = htp_connp_create(cfg);
    if (connp == NULL) {
        htp_config_destroy(cfg);
        free(connp);
        return OK;
    }

    // Open connection
    htp_connp_open(connp, c->remote_ip, /* XXX remote port */ 0, c->local_ip, /* XXX local port */);

    ap_set_module_config(c->conn_config, &libhtp_module, connp);
    apr_pool_cleanup_register(c->pool, connection_cleanup, connp);

    return OK;
}

static void libhtp_register_hooks(apr_pool_t *p) {
    ap_hook_pre_connection(libhtp_pre_conn, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_read_request(libhtp_post_request, NULL, NULL, APR_HOOK_MIDDLE);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA libhtp_module = {
    STANDARD20_MODULE_STUFF,
    NULL, /* create per-dir    config structures */
    NULL, /* merge  per-dir    config structures */
    NULL, /* create per-server config structures */
    NULL, /* merge  per-server config structures */
    NULL, /* table of config file commands       */
    libhtp_register_hooks /* register hooks                      */
};

