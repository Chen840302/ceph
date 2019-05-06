// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2010-2011 Dreamhost
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/ceph_context.h"
#include "common/config.h"
#include "ceph_crypto.h"

#include <openssl/evp.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#  include <openssl/conf.h>
#  include <openssl/engine.h>
#  include <openssl/err.h>
#endif /* OPENSSL_VERSION_NUMBER < 0x10100000L */

namespace ceph::crypto::ssl {

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static std::atomic_uint32_t crypto_refs;

// XXX: vector instead?
static pthread_mutex_t* ssl_mutexes;
static size_t ssl_num_locks;

static struct {
  // we could use e.g. unordered_set instead at the price of providing
  // std::hash<...> specialization. However, we can live with duplicates
  // quite well while the benefit is not worth the effort.
  std::vector<CRYPTO_THREADID> tids;
  ceph::mutex lock = ceph::make_mutex("crypto::ssl::init_records::lock");;
} init_records;

static void
ssl_locking_callback(int mode, int mutex_num, const char *file, int line)
{
  static_cast<void>(line);
  static_cast<void>(file);

  if (mode & 1) {
    /* 1 is CRYPTO_LOCK */
    [[maybe_unused]] auto r = pthread_mutex_lock(&ssl_mutexes[mutex_num]);
  } else {
    [[maybe_unused]] auto r = pthread_mutex_unlock(&ssl_mutexes[mutex_num]);
  }
}

static unsigned long
ssl_get_thread_id(void)
{
  static_assert(sizeof(unsigned long) >= sizeof(pthread_t));
  /* pthread_t may be any data type, so a simple cast to unsigned long
   * can rise a warning/error, depending on the platform.
   * Here memcpy is used as an anything-to-anything cast. */
  unsigned long ret = 0;
  pthread_t t = pthread_self();
  memcpy(&ret, &t, sizeof(pthread_t));
  return ret;
}
#endif /* not OPENSSL_VERSION_NUMBER < 0x10100000L */

static void init() {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  if (++crypto_refs == 1) {
    // according to
    // https://wiki.openssl.org/index.php/Library_Initialization#libcrypto_Initialization
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();

    ssl_num_locks = std::max(CRYPTO_num_locks(), 0);
    if (ssl_num_locks > 0) {
      try {
        ssl_mutexes = new pthread_mutex_t[ssl_num_locks];
      } catch (...) {
        ceph_assert_always("can't allocate memory for OpenSSL init" == nullptr);
      }
    }

    pthread_mutexattr_t pthread_mutex_attr;
    pthread_mutexattr_init(&pthread_mutex_attr);
    pthread_mutexattr_settype(&pthread_mutex_attr, PTHREAD_MUTEX_RECURSIVE);

    for (size_t i = 0; i < ssl_num_locks; i++) {
      pthread_mutex_init(&ssl_mutexes[i], &pthread_mutex_attr);
    }

    // initialize locking callbacks, needed for thread safety.
    // http://www.openssl.org/support/faq.html#PROG1
    CRYPTO_set_locking_callback(&ssl_locking_callback);
    CRYPTO_set_id_callback(&ssl_get_thread_id);

    OPENSSL_config(nullptr);
  }

  // we need to record IDs of all threads calling the initialization in
  // order to *manually* free per-thread memory OpenSSL *automagically*
  // allocated in ERR_get_state().
  // XXX: this solution/nasty hack is IMPERFECT. A leak will appear when
  // a client init()ializes the crypto subsystem with one thread and then
  // uses it from another one in a way that results in ERR_get_state().
  // XXX: for discussion about another approaches please refer to:
  // https://www.mail-archive.com/openssl-users@openssl.org/msg59070.html
  {
    std::lock_guard l(init_records.lock);
    CRYPTO_THREADID tmp;
    CRYPTO_THREADID_current(&tmp);
    init_records.tids.emplace_back(std::move(tmp));
  }
#endif /* OPENSSL_VERSION_NUMBER < 0x10100000L */
}

static void shutdown() {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  if (--crypto_refs != 0) {
    return;
  }

  // drop error queue for each thread that called the init() function to
  // satisfy valgrind.
  {
    std::lock_guard l(init_records.lock);

    // NOTE: in OpenSSL 1.0.2g the signature is:
    //    void ERR_remove_thread_state(const CRYPTO_THREADID *tid);
    // but in 1.1.0j it has been changed to
    //    void ERR_remove_thread_state(void *);
    // We're basing on the OPENSSL_VERSION_NUMBER check to preserve
    // const-correctness without failing builds on modern envs.
    for (const auto& tid : init_records.tids) {
      ERR_remove_thread_state(&tid);
    }
  }

  // Shutdown according to
  // https://wiki.openssl.org/index.php/Library_Initialization#Cleanup
  // http://stackoverflow.com/questions/29845527/how-to-properly-uninitialize-openssl
  //
  // The call to CONF_modules_free() has been introduced after a valgring run.
  CRYPTO_set_locking_callback(nullptr);
  CRYPTO_set_id_callback(nullptr);
  ENGINE_cleanup();
  CONF_modules_free();
  CONF_modules_unload(1);
  ERR_free_strings();
  EVP_cleanup();
  CRYPTO_cleanup_all_ex_data();

  for (size_t i = 0; i < ssl_num_locks; i++) {
    pthread_mutex_destroy(&ssl_mutexes[i]);
  }
  delete[] ssl_mutexes;
  ssl_mutexes = nullptr;
#endif /* OPENSSL_VERSION_NUMBER < 0x10100000L */
}

} // namespace ceph::crypto::openssl


void ceph::crypto::init() {
  ceph::crypto::ssl::init();
}

void ceph::crypto::shutdown([[maybe_unused]] const bool shared) {
  ceph::crypto::ssl::shutdown();
}

ceph::crypto::ssl::OpenSSLDigest::OpenSSLDigest(const EVP_MD * _type)
  : mpContext(EVP_MD_CTX_create())
  , mpType(_type) {
  this->Restart();
}

ceph::crypto::ssl::OpenSSLDigest::~OpenSSLDigest() {
  EVP_MD_CTX_destroy(mpContext);
}

void ceph::crypto::ssl::OpenSSLDigest::Restart() {
  EVP_DigestInit_ex(mpContext, mpType, NULL);
}

void ceph::crypto::ssl::OpenSSLDigest::Update(const unsigned char *input, size_t length) {
  if (length) {
    EVP_DigestUpdate(mpContext, const_cast<void *>(reinterpret_cast<const void *>(input)), length);
  }
}

void ceph::crypto::ssl::OpenSSLDigest::Final(unsigned char *digest) {
  unsigned int s;
  EVP_DigestFinal_ex(mpContext, digest, &s);
}
