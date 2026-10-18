// SenseHive secure topics, protocol sst1 (https://github.com/855princekumar/sense-hive/blob/main/docs/SECURE-TOPICS.md)
//
//   enc_key = HMAC-SHA256(key, "sst1 enc")      mac_key = HMAC-SHA256(key, "sst1 mac")
//   c = AES-256-CTR(enc_key, nonce = 16 random bytes, payload)
//   m = first 16 bytes of HMAC-SHA256(mac_key, "sst1" 0 topic 0 key_id 0 t 0 nonce c)
//   message = {"v":1,"k":"<key id>","t":<unix>,"i":"<b64 nonce>","c":"<b64 c>","m":"<b64 m>"}
//
// Plain C with mbedTLS only, so the same file builds for the ESP32 (mbedTLS is part of
// the core) and on a PC, where the tests check it against SenseHive itself.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#define SH_KEY_ID_LEN 16
#define SH_KEY_LEN 32
#ifndef SH_MAX_PAYLOAD
#define SH_MAX_PAYLOAD 1024   // largest payload sh_seal() takes (bytes)
#endif

// Fills buf with random bytes; the ESP32 build uses the hardware generator.
#ifndef SH_RANDOM
#include "esp_random.h"
#define SH_RANDOM(buf, len) esp_fill_random((buf), (len))
#endif

static inline int sh_hmac(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len, msg, msg_len, out);
}

static inline int sh_hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 1 when kid is 16 hex characters and key_hex 64; the key is written to key.
static inline int sh_parse_key(const char *kid, const char *key_hex, uint8_t key[SH_KEY_LEN]) {
  if (!kid || !key_hex || strlen(kid) != SH_KEY_ID_LEN || strlen(key_hex) != 2 * SH_KEY_LEN) return 0;
  for (int i = 0; i < SH_KEY_ID_LEN; i++)
    if (sh_hex_value(kid[i]) < 0) return 0;
  for (int i = 0; i < SH_KEY_LEN; i++) {
    int hi = sh_hex_value(key_hex[2 * i]), lo = sh_hex_value(key_hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return 0;
    key[i] = (uint8_t)(hi << 4 | lo);
  }
  return 1;
}

// Seals payload for topic into out (a NUL terminated JSON envelope).
// nonce may be NULL (random, the normal case); tests pass a fixed one.
// Returns the envelope length, or 0 when out is too small or a step fails.
// Room needed: 4/3 of the payload plus 140 bytes (SH_SEALED_SIZE). Not reentrant.
#define SH_SEALED_SIZE(len) (4 * (((len) + 2) / 3) + 140)
static inline size_t sh_seal(const uint8_t key[SH_KEY_LEN], const char *kid, const char *topic, unsigned long long t,
                             const uint8_t *payload, size_t len, const uint8_t *fixed_nonce, char *out, size_t out_size) {
  uint8_t enc_key[32], mac_key[32], nonce[16], stream[16], tag[32];
  char t_text[24];
  static uint8_t ct[SH_MAX_PAYLOAD];
  if (!out || out_size < SH_SEALED_SIZE(len) || len > SH_MAX_PAYLOAD) return 0;

  if (sh_hmac(key, SH_KEY_LEN, (const uint8_t *)"sst1 enc", 8, enc_key)) return 0;
  if (sh_hmac(key, SH_KEY_LEN, (const uint8_t *)"sst1 mac", 8, mac_key)) return 0;
  if (fixed_nonce) memcpy(nonce, fixed_nonce, 16);
  else SH_RANDOM(nonce, 16);

  uint8_t counter[16];
  size_t off = 0;
  memcpy(counter, nonce, 16);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int bad = mbedtls_aes_setkey_enc(&aes, enc_key, 256) ||
            mbedtls_aes_crypt_ctr(&aes, len, &off, counter, stream, payload, ct);
  mbedtls_aes_free(&aes);
  if (bad) return 0;

  // m = HMAC(mac_key, "sst1" 0 topic 0 kid 0 t 0 nonce ct), fed piece by piece
  snprintf(t_text, sizeof t_text, "%llu", t);
  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  const uint8_t zero = 0;
  bad = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) ||
        mbedtls_md_hmac_starts(&md, mac_key, 32) ||
        mbedtls_md_hmac_update(&md, (const uint8_t *)"sst1", 4) || mbedtls_md_hmac_update(&md, &zero, 1) ||
        mbedtls_md_hmac_update(&md, (const uint8_t *)topic, strlen(topic)) || mbedtls_md_hmac_update(&md, &zero, 1) ||
        mbedtls_md_hmac_update(&md, (const uint8_t *)kid, strlen(kid)) || mbedtls_md_hmac_update(&md, &zero, 1) ||
        mbedtls_md_hmac_update(&md, (const uint8_t *)t_text, strlen(t_text)) || mbedtls_md_hmac_update(&md, &zero, 1) ||
        mbedtls_md_hmac_update(&md, nonce, 16) || mbedtls_md_hmac_update(&md, ct, len) ||
        mbedtls_md_hmac_finish(&md, tag);
  mbedtls_md_free(&md);
  if (bad) return 0;

  // {"v":1,"k":"..","t":..,"i":"..","c":"..","m":".."}
  size_t pos = 0, w = 0;
  int n = snprintf(out, out_size, "{\"v\":1,\"k\":\"%s\",\"t\":%s,\"i\":\"", kid, t_text);
  if (n < 0) return 0;
  pos = (size_t)n;
  if (mbedtls_base64_encode((unsigned char *)out + pos, out_size - pos, &w, nonce, 16)) return 0;
  pos += w;
  memcpy(out + pos, "\",\"c\":\"", 7);
  pos += 7;
  if (mbedtls_base64_encode((unsigned char *)out + pos, out_size - pos, &w, ct, len)) return 0;
  pos += w;
  memcpy(out + pos, "\",\"m\":\"", 7);
  pos += 7;
  if (mbedtls_base64_encode((unsigned char *)out + pos, out_size - pos, &w, tag, 16)) return 0;
  pos += w;
  if (pos + 3 > out_size) return 0;
  out[pos++] = '"';
  out[pos++] = '}';
  out[pos] = 0;
  return pos;
}
