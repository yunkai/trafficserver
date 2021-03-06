/** @file

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include <openssl/ocsp.h>
#include "P_OCSPStapling.h"
#include "P_Net.h"
#include "P_SSLConfig.h"

#ifdef HAVE_OPENSSL_OCSP_STAPLING

// Maxiumum OCSP stapling response size.
// This should be the response for a single certificate and will typically include the responder certificate chain,
// so 10K should be more than enough.
#define MAX_STAPLING_DER 10240

// Cached info stored in SSL_CTX ex_info
struct certinfo
{
  unsigned char idx[20];  // Index in session cache SHA1 hash of certificate
  OCSP_CERTID *cid; // Certificate ID for OCSP requests or NULL if ID cannot be determined
  char *uri;  // Responder details
  ink_mutex stapling_mutex;
  unsigned char resp_der[MAX_STAPLING_DER];
  unsigned int resp_derlen;
  bool is_expire;
  time_t expire_time;
};

void certinfo_free(void * /*parent*/, void *ptr, CRYPTO_EX_DATA * /*ad*/,
    int /*idx*/, long /*argl*/, void * /*argp*/)
{
  certinfo *cinf = (certinfo *)ptr;

  if (!cinf)
    return;
  if (cinf->uri)
    OPENSSL_free(cinf->uri);
  ink_mutex_destroy(&cinf->stapling_mutex);
  OPENSSL_free(cinf);
}

static int ssl_stapling_index = -1;

void ssl_stapling_ex_init(void)
{
  if (ssl_stapling_index != -1)
    return;
  ssl_stapling_index = SSL_CTX_get_ex_new_index(0, 0, 0, 0, certinfo_free);
}

static X509 *
stapling_get_issuer(SSL_CTX *ssl_ctx, X509 *x)
{
  X509 *issuer = NULL;
  int i;
  X509_STORE *st = SSL_CTX_get_cert_store(ssl_ctx);
  X509_STORE_CTX inctx;
  STACK_OF(X509) *extra_certs = NULL;

#ifdef SSL_CTX_get_extra_chain_certs
  SSL_CTX_get_extra_chain_certs(ssl_ctx, &extra_certs);
#else
  extra_certs = ssl_ctx->extra_certs;
#endif

  if (sk_X509_num(extra_certs) == 0)
    return NULL;

  for (i = 0; i < sk_X509_num(extra_certs); i++) {
    issuer = sk_X509_value(extra_certs, i);
    if (X509_check_issued(issuer, x) == X509_V_OK) {
      CRYPTO_add(&issuer->references, 1, CRYPTO_LOCK_X509);
      return issuer;
    }
  }

  if (!X509_STORE_CTX_init(&inctx, st, NULL, NULL))
    return NULL;
  if (X509_STORE_CTX_get1_issuer(&issuer, &inctx, x) <= 0)
    issuer = NULL;
  X509_STORE_CTX_cleanup(&inctx);

  return issuer;
}

bool
ssl_stapling_init_cert(SSL_CTX *ctx, const char *certfile)
{
  certinfo *cinf;
  X509 *cert = NULL;
  X509 *issuer = NULL;
  STACK_OF(OPENSSL_STRING) *aia = NULL;
  BIO *bio = BIO_new_file(certfile, "r");

  cert = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
  if (!cert) {
    Debug("ssl", "can not read cert from certfile %s!", certfile);
    return false;
  }

  cinf  = (certinfo *)SSL_CTX_get_ex_data(ctx, ssl_stapling_index);
  if (cinf) {
    Debug("ssl", "certificate already initialized!");
    return false;
  }

  cinf = (certinfo *)OPENSSL_malloc(sizeof(certinfo));
  if (!cinf) {
    Debug("ssl", "error allocating memory!");
    return false;
  }

  // Initialize certinfo
  cinf->cid = NULL;
  cinf->uri = NULL;
  cinf->resp_derlen = 0;
  ink_mutex_init(&cinf->stapling_mutex, "stapling_mutex");
  cinf->is_expire = true;
  cinf->expire_time = 0;

  SSL_CTX_set_ex_data(ctx, ssl_stapling_index, cinf);

  issuer = stapling_get_issuer(ctx, cert);
  if (issuer == NULL) {
        Debug("ssl", "can not get issuer certificate!");
    return false;
  }

  cinf->cid = OCSP_cert_to_id(NULL, cert, issuer);
  X509_free(issuer);
  if (!cinf->cid)
    return false;
  X509_digest(cert, EVP_sha1(), cinf->idx, NULL);

  aia = X509_get1_ocsp(cert);
  if (aia)
    cinf->uri = sk_OPENSSL_STRING_pop(aia);
  if (!cinf->uri) {
        Debug("ssl", "no responder URI");
  }
  if (aia)
    X509_email_free(aia);

  Debug("ssl", "success to init certinfo into SSL_CTX: %p", ctx);
  return true;
}

static certinfo *
stapling_get_cert_info(SSL_CTX *ctx)
{
  certinfo *cinf;

  cinf = (certinfo *)SSL_CTX_get_ex_data(ctx, ssl_stapling_index);
  if (cinf && cinf->cid)
    return cinf;

  return NULL;
}

static bool
stapling_cache_response(OCSP_RESPONSE *rsp, certinfo *cinf)
{
  unsigned char resp_der[MAX_STAPLING_DER];
  unsigned char *p;
  unsigned int resp_derlen;

  p = resp_der;
  resp_derlen = i2d_OCSP_RESPONSE(rsp, &p);

  if (resp_derlen == 0) {
    Error("stapling_cache_response: can not encode OCSP stapling response");
    return false;
  }

  if (resp_derlen > MAX_STAPLING_DER) {
    Error("stapling_cache_response: OCSP stapling response too big (%u bytes)", resp_derlen);
    return false;
  }

  ink_mutex_acquire(&cinf->stapling_mutex);
  memcpy(cinf->resp_der, resp_der, resp_derlen);
  cinf->resp_derlen = resp_derlen;
  cinf->is_expire = false;
  cinf->expire_time = time(NULL) + SSLConfigParams::ssl_ocsp_cache_timeout;
  ink_mutex_release(&cinf->stapling_mutex);

  Debug("ssl", "stapling_cache_response: success to cache response");
  return true;
}

static int
stapling_check_response(certinfo *cinf, OCSP_RESPONSE *rsp)
{
  int status, reason;
  OCSP_BASICRESP *bs = NULL;
  ASN1_GENERALIZEDTIME *rev, *thisupd, *nextupd;
  int response_status = OCSP_response_status(rsp);

  // Check to see if response is an error.
  // If so we automatically accept it because it would have expired from the cache if it was time to retry.
  if (response_status != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
    return SSL_TLSEXT_ERR_NOACK;
  }

  bs = OCSP_response_get1_basic(rsp);
  if (bs == NULL) {
    // If we can't parse response just pass it back to client
    Error("stapling_check_response: can not parsing response");
    return SSL_TLSEXT_ERR_OK;
  }
  if (!OCSP_resp_find_status(bs, cinf->cid, &status, &reason, &rev,
        &thisupd, &nextupd)) {
    // If ID not present just pass it back to client
    Error("stapling_check_response: certificate ID not present in response");
  } else {
    OCSP_check_validity(thisupd, nextupd, 300, -1);
  }
  OCSP_BASICRESP_free(bs);

  return SSL_TLSEXT_ERR_OK;
}

static OCSP_RESPONSE *
query_responder(BIO *b, char *path, OCSP_REQUEST *req, int req_timeout)
{
  ink_hrtime start, end;
  OCSP_RESPONSE *resp = NULL;
  OCSP_REQ_CTX *ctx;
  int rv;

  start = ink_get_hrtime();
  end = ink_hrtime_add(start, ink_hrtime_from_sec(req_timeout));

  ctx = OCSP_sendreq_new(b, path, req, -1);
  do {
    rv = OCSP_sendreq_nbio(&resp, ctx);
    ink_hrtime_sleep(HRTIME_MSECONDS(1));
  } while ((rv == -1) && BIO_should_retry(b) && (ink_get_hrtime() < end));

  OCSP_REQ_CTX_free(ctx);

  if (rv)
    return resp;

  return NULL;
}

static OCSP_RESPONSE *
process_responder(OCSP_REQUEST *req,
    char *host, char *path, char *port,
    int req_timeout)
{
  BIO *cbio = NULL;
  OCSP_RESPONSE *resp = NULL;
  cbio = BIO_new_connect(host);
  if (!cbio) {
    goto end;
  }
  if (port) BIO_set_conn_port(cbio, port);

  BIO_set_nbio(cbio, 1);
  if (BIO_do_connect(cbio) <= 0 && !BIO_should_retry(cbio)) {
    Debug("ssl", "process_responder: fail to connect to OCSP respond server");
    goto end;
  }
  resp = query_responder(cbio, path, req, req_timeout);

end:
  if (cbio)
    BIO_free_all(cbio);
  return resp;
}

static bool
stapling_refresh_response(certinfo *cinf, OCSP_RESPONSE **prsp)
{
  bool rv = true;
  OCSP_REQUEST *req = NULL;
  OCSP_CERTID *id = NULL;
  char *host, *path, *port;
  int ssl_flag = 0;
  int req_timeout = -1;

  Debug("ssl", "stapling_refresh_response: querying responder");
  *prsp = NULL;

  if (!OCSP_parse_url(cinf->uri, &host, &port, &path, &ssl_flag)) {
    goto err;
  }

  req = OCSP_REQUEST_new();
  if (!req)
    goto err;
  id = OCSP_CERTID_dup(cinf->cid);
  if (!id)
    goto err;
  if (!OCSP_request_add0_id(req, id))
    goto err;

  req_timeout = SSLConfigParams::ssl_ocsp_request_timeout;
  *prsp = process_responder(req, host, path, port, req_timeout);

  if (*prsp == NULL) {
    goto done;
  }

  if (OCSP_response_status(*prsp) == OCSP_RESPONSE_STATUS_SUCCESSFUL) {
    Debug("ssl", "stapling_refresh_response: query response received");
    stapling_check_response(cinf, *prsp);
  } else {
    Error("stapling_refresh_response: responder error");
  }

  if (!stapling_cache_response(*prsp, cinf)) {
    Error("stapling_refresh_response: can not cache response");
  } else {
    Debug("ssl", "stapling_refresh_response: success to refresh response");
  }

done:
  if (req)
    OCSP_REQUEST_free(req);
  if (*prsp)
    OCSP_RESPONSE_free(*prsp);
  return rv;

err:
  rv = false;
  Debug("ssl", "stapling_refresh_response: fail to refresh response");
  goto done;
}

void
ocsp_update()
{
  SSL_CTX *ctx;
  certinfo *cinf = NULL;
  OCSP_RESPONSE *resp = NULL;
  time_t current_time;

  SSLCertificateConfig::scoped_config certLookup;
  const unsigned ctxCount = certLookup->count();

  for (unsigned i = 0; i < ctxCount; i++) {
    ctx = certLookup->get(i);
    cinf = stapling_get_cert_info(ctx);
    if (cinf) {
      ink_mutex_acquire(&cinf->stapling_mutex);
      current_time = time(NULL);
      if (cinf->resp_derlen == 0 || cinf->is_expire || cinf->expire_time < current_time) {
        ink_mutex_release(&cinf->stapling_mutex);
        if (stapling_refresh_response(cinf, &resp)) {
          Note("Success to refresh OCSP response for 1 certificate.");
        } else {
          Note("Fail to refresh OCSP response for 1 certificate.");
        }
      } else {
        ink_mutex_release(&cinf->stapling_mutex);
      }
    }
  }
}

// RFC 6066 Section-8: Certificate Status Request
int
ssl_callback_ocsp_stapling(SSL *ssl)
{
  certinfo *cinf = NULL;
  time_t current_time;

  cinf = stapling_get_cert_info(ssl->ctx);
  if (cinf == NULL) {
    Debug("ssl", "ssl_callback_ocsp_stapling: fail to get certificate information");
    return SSL_TLSEXT_ERR_NOACK;
  }

  ink_mutex_acquire(&cinf->stapling_mutex);
  current_time = time(NULL);
  if (cinf->resp_derlen == 0 || cinf->is_expire || cinf->expire_time < current_time) {
    ink_mutex_release(&cinf->stapling_mutex);
    Debug("ssl", "ssl_callback_ocsp_stapling: fail to get certificate status");
    return SSL_TLSEXT_ERR_NOACK;
  } else {
    unsigned char *p = (unsigned char *)OPENSSL_malloc(cinf->resp_derlen);
    unsigned int len = cinf->resp_derlen;
    memcpy(p, cinf->resp_der, cinf->resp_derlen);
    ink_mutex_release(&cinf->stapling_mutex);
    SSL_set_tlsext_status_ocsp_resp(ssl, p, len);
    Debug("ssl", "ssl_callback_ocsp_stapling: success to get certificate status");
    return SSL_TLSEXT_ERR_OK;
  }
}

#endif /* HAVE_OPENSSL_OCSP_STAPLING */
