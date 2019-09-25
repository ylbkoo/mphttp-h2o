//
// Created by Chengke Wong on 2019/9/16.
//

#include <h2o/mpclient.h>
#include "h2o/mpclient.h"

#define H2O_MPCLIENT_NO_CERT_VERIFICATION

h2o_mpclient_t* h2o_mpclient_create(char* request_url, h2o_httpclient_ctx_t *_ctx,
                                    h2o_mpclient_t *(*on_reschedule)(h2o_mpclient_t *)) {
  // |h2o_socketpool_create_target| will copy |request_url|
  // we can allocate it on stack
  h2o_url_t url_parsed;
  if (h2o_url_parse(request_url, SIZE_MAX, &url_parsed) != 0) {
    h2o_error_printf("unrecognized type of URL: %s", request_url);
    return NULL;
  }

  h2o_mpclient_t* mp = h2o_mem_alloc(sizeof(h2o_mpclient_t));
  h2o_mem_set_secure(mp, 0, sizeof(h2o_mpclient_t));
  mp->ctx = _ctx;
  mp->connpool = h2o_mem_alloc(sizeof(h2o_httpclient_connection_pool_t));
  mp->url_prefix = request_url;
  mp->on_reschedule = on_reschedule;

  char buf[16];
  static int data_log_count = 0;
  snprintf(buf, 16, "%d.dat", ++data_log_count);
  mp->data_log = fopen(buf, "w");

  h2o_socketpool_t *sockpool = h2o_mem_alloc(sizeof(*sockpool));
  h2o_socketpool_target_t *target = h2o_socketpool_create_target(&url_parsed, NULL);
  h2o_socketpool_init_specific(sockpool, 10, &target, 1, NULL);
  h2o_socketpool_set_timeout(sockpool, 5000 /* in msec */);
  h2o_socketpool_register_loop(sockpool, mp->ctx->loop);
  h2o_httpclient_connection_pool_init(mp->connpool, sockpool);

  /* obtain root */
  char *root, *crt_fullpath;
  if ((root = getenv("H2O_ROOT")) == NULL)
    root = H2O_TO_STR(H2O_ROOT);
#define CA_PATH "/share/h2o/ca-bundle.crt"
  crt_fullpath = h2o_mem_alloc(strlen(root) + strlen(CA_PATH) + 1);
  sprintf(crt_fullpath, "%s%s", root, CA_PATH);
#undef CA_PATH

  SSL_CTX *ssl_ctx = SSL_CTX_new(TLSv1_client_method());
  SSL_CTX_load_verify_locations(ssl_ctx, crt_fullpath, NULL);
#ifdef H2O_MPCLIENT_NO_CERT_VERIFICATION
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
#else
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
#endif
  h2o_socketpool_set_ssl_ctx(sockpool, ssl_ctx);
  SSL_CTX_free(ssl_ctx);
  return mp;
}

void h2o_mpclient_update(h2o_mpclient_t* mp) {
  if (mp->rangeclient.running == NULL) {
    mp->rangeclient.running = mp->rangeclient.pending;
    return;
  }
  if (mp->rangeclient.running->is_closed) {
    h2o_rangeclient_destroy(mp->rangeclient.running);
    mp->rangeclient.running = NULL;
    h2o_mpclient_update(mp);
    return;
  }
  // TODO: put the rangeclient with larger chunk to |rangeclient.running|
}

static int assemble_url(h2o_mpclient_t *mp, char *request_path, h2o_url_t *url_parsed, char *request_url) {
  strncpy(request_url, mp->url_prefix, 128);
  strncat(request_url, request_path, 128);
  if (h2o_url_parse(request_url, SIZE_MAX, url_parsed) != 0) {
    h2o_error_printf("unrecognized type of URL: %s", request_url);
    return -1;
  }
  return 0;
}

static void on_mostly_complete(h2o_rangeclient_t *client) {
  h2o_mpclient_t *mp = (h2o_mpclient_t*) client->data;
  h2o_mpclient_update(mp);
  if (mp->rangeclient.pending != NULL) {
    return;
  }
  h2o_mpclient_t *mp2 = mp->on_reschedule(mp);
  if (mp2 == NULL) {
    return;
  }

  h2o_mpclient_reschedule(mp, mp2);
}

int h2o_mpclient_fetch(h2o_mpclient_t *mp, char *request_path, char *save_to_file, size_t begin, size_t end) {
  // |h2o_rangeclient_create| will copy |url_parsed| and |buf|
  // we can allocate it on stack
  h2o_url_t url_parsed;
  char buf[128];
  if (assemble_url(mp, request_path, &url_parsed, buf) < 0) {
    return -1;
  }

  h2o_mpclient_update(mp);
  if (mp->rangeclient.running == NULL) {
    mp->rangeclient.running = h2o_rangeclient_create(mp->connpool, mp, mp->data_log,
                                                     mp->ctx, &url_parsed,
                                                     save_to_file, begin, end);
    mp->rangeclient.running->cb.on_mostly_complete = on_mostly_complete;
    return 0;
  }
  return -1;
}

static size_t h2o_mpclient_guess_bw(h2o_mpclient_t *mp) {
  if (mp->rangeclient.running) {
    return h2o_rangeclient_get_bw(mp->rangeclient.running);
  }
  // TODO: reuse bandwidth history
  return 1024 * 64; // 64 Kb / s
}

void h2o_mpclient_reschedule(h2o_mpclient_t *mp_idle, h2o_mpclient_t *mp_busy) {
  assert(mp_idle != mp_busy && "scheduling should be between two different clients");
  h2o_rangeclient_t *client_busy = mp_busy->rangeclient.running;
  h2o_rangeclient_t *client_idle = mp_idle->rangeclient.pending;
  assert(client_busy != NULL);
  assert(client_idle == NULL);

  // TODO: move the constant to |ctx|
  if (h2o_rangeclient_get_remaining_time(client_busy) < 100 /* ms */) {
    return;
  }

  size_t bw_idle = h2o_mpclient_guess_bw(mp_idle);
  size_t bw_busy = h2o_mpclient_guess_bw(mp_busy);

  size_t remaining = client_busy->range.end - client_busy->range.begin - client_busy->range.received;

  // take care of the overflow
  size_t data_idle = (uint64_t) remaining * bw_idle / (bw_idle + bw_busy);
  size_t data_end = client_busy->range.end;
  h2o_rangeclient_adjust_range_end(client_busy, client_busy->range.end - data_idle);

  mp_idle->rangeclient.pending =
    h2o_rangeclient_create(mp_idle->connpool, mp_idle, mp_idle->data_log, mp_idle->ctx,
                           client_busy->url_parsed, client_busy->save_to_file,
                           data_end - data_idle, data_end);

  h2o_mpclient_update(mp_idle);
}

void h2o_mpclient_destroy(h2o_mpclient_t* mp) {
  free(mp->connpool);
  free(mp);
  if (mp->data_log) {
    fclose(mp->data_log);
    mp->data_log = NULL;
  }
}