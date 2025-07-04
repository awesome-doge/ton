/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "crypto/Ed25519.h"

#include <openssl/opensslv.h>

#include "td/utils/base64.h"
#include "td/utils/BigNum.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/ScopeGuard.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace td {

Ed25519::PublicKey::PublicKey(SecureString octet_string) : octet_string_(std::move(octet_string)) {
}

SecureString Ed25519::PublicKey::as_octet_string() const {
  return octet_string_.copy();
}

Ed25519::PrivateKey::PrivateKey(SecureString octet_string) : octet_string_(std::move(octet_string)) {
}

SecureString Ed25519::PrivateKey::as_octet_string() const {
  return octet_string_.copy();
}

namespace detail {

static Result<SecureString> X25519_key_from_PKEY(EVP_PKEY *pkey, bool is_private) {
  auto func = is_private ? &EVP_PKEY_get_raw_private_key : &EVP_PKEY_get_raw_public_key;
  size_t len = 0;
  if (func(pkey, nullptr, &len) == 0) {
    return Status::Error("Failed to get raw key length");
  }
  CHECK(len == 32);

  SecureString result(len);
  if (func(pkey, result.as_mutable_slice().ubegin(), &len) == 0) {
    return Status::Error("Failed to get raw key");
  }
  return std::move(result);
}

static EVP_PKEY *X25519_key_to_PKEY(Slice key, bool is_private) {
  auto func = is_private ? &EVP_PKEY_new_raw_private_key : &EVP_PKEY_new_raw_public_key;
  return func(EVP_PKEY_ED25519, nullptr, key.ubegin(), key.size());
}

static Result<SecureString> X25519_pem_from_PKEY(EVP_PKEY *pkey, bool is_private, Slice password) {
  BIO *mem_bio = BIO_new(BIO_s_mem());
  SCOPE_EXIT {
    BIO_vfree(mem_bio);
  };
  if (is_private) {
    PEM_write_bio_PrivateKey(mem_bio, pkey, EVP_aes_256_cbc(), const_cast<unsigned char *>(password.ubegin()),
                             narrow_cast<int>(password.size()), nullptr, nullptr);
  } else {
    PEM_write_bio_PUBKEY(mem_bio, pkey);
  }
  char *data_ptr = nullptr;
  auto data_size = BIO_get_mem_data(mem_bio, &data_ptr);
  return std::string(data_ptr, data_size);
}

static int password_cb(char *buf, int size, int rwflag, void *u) {
  auto &password = *reinterpret_cast<Slice *>(u);
  auto password_size = narrow_cast<int>(password.size());
  if (size < password_size) {
    return -1;
  }
  if (rwflag == 0) {
    MutableSlice(buf, size).copy_from(password);
  }
  return password_size;
}

static EVP_PKEY *X25519_pem_to_PKEY(Slice pem, Slice password) {
  BIO *mem_bio = BIO_new_mem_buf(pem.ubegin(), narrow_cast<int>(pem.size()));
  SCOPE_EXIT {
    BIO_vfree(mem_bio);
  };

  return PEM_read_bio_PrivateKey(mem_bio, nullptr, password_cb, &password);
}

}  // namespace detail

Result<Ed25519::PrivateKey> Ed25519::generate_private_key() {
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(NID_ED25519, nullptr);
  if (pctx == nullptr) {
    return Status::Error("Can't create EVP_PKEY_CTX");
  }
  SCOPE_EXIT {
    EVP_PKEY_CTX_free(pctx);
  };

  if (EVP_PKEY_keygen_init(pctx) <= 0) {
    return Status::Error("Can't init keygen");
  }

  EVP_PKEY *pkey = nullptr;
  if (EVP_PKEY_keygen(pctx, &pkey) <= 0) {
    return Status::Error("Can't generate random private key");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey);
  };

  TRY_RESULT(private_key, detail::X25519_key_from_PKEY(pkey, true));
  return std::move(private_key);
}

Result<Ed25519::PublicKey> Ed25519::PrivateKey::get_public_key() const {
  auto pkey = detail::X25519_key_to_PKEY(octet_string_, true);
  if (pkey == nullptr) {
    return Status::Error("Can't import private key");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey);
  };

  TRY_RESULT(key, detail::X25519_key_from_PKEY(pkey, false));
  return Ed25519::PublicKey(std::move(key));
}

Result<SecureString> Ed25519::PrivateKey::as_pem(Slice password) const {
  auto pkey = detail::X25519_key_to_PKEY(octet_string_, true);
  if (pkey == nullptr) {
    return Status::Error("Can't import private key");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey);
  };

  return detail::X25519_pem_from_PKEY(pkey, true, password);
}

Result<Ed25519::PrivateKey> Ed25519::PrivateKey::from_pem(Slice pem, Slice password) {
  auto pkey = detail::X25519_pem_to_PKEY(pem, password);
  if (pkey == nullptr) {
    return Status::Error("Can't import private key from pem");
  }
  TRY_RESULT(key, detail::X25519_key_from_PKEY(pkey, true));
  return Ed25519::PrivateKey(std::move(key));
}

Result<SecureString> Ed25519::PrivateKey::sign(Slice data) const {
  auto pkey = detail::X25519_key_to_PKEY(octet_string_, true);
  if (pkey == nullptr) {
    return Status::Error("Can't import private key");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey);
  };

  EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
  if (md_ctx == nullptr) {
    return Status::Error("Can't create EVP_MD_CTX");
  }
  SCOPE_EXIT {
    EVP_MD_CTX_free(md_ctx);
  };

  if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
    return Status::Error("Can't init DigestSign");
  }

  SecureString res(64, '\0');
  size_t len = 64;
  if (EVP_DigestSign(md_ctx, res.as_mutable_slice().ubegin(), &len, data.ubegin(), data.size()) <= 0) {
    return Status::Error("Can't sign data");
  }
  return std::move(res);
}

Status Ed25519::PublicKey::verify_signature(Slice data, Slice signature) const {
  auto pkey = detail::X25519_key_to_PKEY(octet_string_, false);
  if (pkey == nullptr) {
    return Status::Error("Can't import public key");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey);
  };

  EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
  if (md_ctx == nullptr) {
    return Status::Error("Can't create EVP_MD_CTX");
  }
  SCOPE_EXIT {
    EVP_MD_CTX_free(md_ctx);
  };

  if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
    return Status::Error("Can't init DigestVerify");
  }

  if (EVP_DigestVerify(md_ctx, signature.ubegin(), signature.size(), data.ubegin(), data.size())) {
    return Status::OK();
  }
  return Status::Error("Wrong signature");
}

Result<SecureString> Ed25519::compute_shared_secret(const PublicKey &public_key, const PrivateKey &private_key) {
  BigNum p = BigNum::from_hex("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed").move_as_ok();
  auto public_y = public_key.as_octet_string();
  public_y.as_mutable_slice()[31] = static_cast<char>(public_y[31] & 127);
  BigNum y = BigNum::from_le_binary(public_y);
  BigNum y2 = y.clone();
  y += 1;
  y2 -= 1;

  BigNumContext context;

  BigNum::mod_sub(y2, p, y2, p, context);

  BigNum inverse_y_plus_1;
  BigNum::mod_inverse(inverse_y_plus_1, y2, p, context);

  BigNum u;
  BigNum::mod_mul(u, y, inverse_y_plus_1, p, context);

  auto pr_key = private_key.as_octet_string();
  unsigned char buf[64];
  SHA512(Slice(pr_key).ubegin(), 32, buf);
  buf[0] &= 248;
  buf[31] &= 127;
  buf[31] |= 64;

  auto pkey_private = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, buf, 32);
  if (pkey_private == nullptr) {
    return Status::Error("Can't import private key");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey_private);
  };
  // LOG(ERROR) << buffer_to_hex(Slice(buf, 32));

  auto pub_key = u.to_le_binary(32);
  auto pkey_public = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, Slice(pub_key).ubegin(), pub_key.size());
  if (pkey_public == nullptr) {
    return Status::Error("Can't import public key");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey_public);
  };
  // LOG(ERROR) << buffer_to_hex(pub_key);

  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey_private, nullptr);
  if (ctx == nullptr) {
    return Status::Error("Can't create EVP_PKEY_CTX");
  }
  SCOPE_EXIT {
    EVP_PKEY_CTX_free(ctx);
  };

  if (EVP_PKEY_derive_init(ctx) <= 0) {
    return Status::Error("Can't init derive");
  }
  if (EVP_PKEY_derive_set_peer(ctx, pkey_public) <= 0) {
    return Status::Error("Can't init derive");
  }

  size_t result_len = 0;
  if (EVP_PKEY_derive(ctx, nullptr, &result_len) <= 0) {
    return Status::Error("Can't get result length");
  }
  if (result_len != 32) {
    return Status::Error("Unexpected result length");
  }

  SecureString result(result_len, '\0');
  if (EVP_PKEY_derive(ctx, result.as_mutable_slice().ubegin(), &result_len) <= 0) {
    return Status::Error("Failed to compute shared secret");
  }
  return std::move(result);
}

int Ed25519::version() {
  return OPENSSL_VERSION_NUMBER;
}

}  // namespace td
