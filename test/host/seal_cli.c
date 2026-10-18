// Runs the firmware's own sensehive_secure.h on a PC, so the tests can check it
// against SenseHive. Build (Linux): cc -O2 -o seal_cli seal_cli.c -lmbedcrypto
//
//   seal_cli <key id> <key hex> <topic> <unix time> <payload> [nonce hex (32)]
//
// Prints the sealed message. Without a nonce, a random one is used.
#include <stdio.h>
#include <stdlib.h>

static void host_random(unsigned char *buf, size_t len) {
  FILE *f = fopen("/dev/urandom", "rb");
  if (!f || fread(buf, 1, len, f) != len) {
    fprintf(stderr, "no /dev/urandom\n");
    exit(2);
  }
  fclose(f);
}
#define SH_RANDOM(buf, len) host_random((buf), (len))

#include "../../Firmware/PowerPulse-v2.0-auto/sensehive_secure.h"

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: seal_cli <key id> <key hex> <topic> <unix time> <payload> [nonce hex]\n");
    return 2;
  }
  uint8_t key[SH_KEY_LEN], nonce[16];
  if (!sh_parse_key(argv[1], argv[2], key)) {
    fprintf(stderr, "bad key id or key\n");
    return 3;
  }
  const uint8_t *fixed = NULL;
  if (argc > 6) {
    if (strlen(argv[6]) != 32) return 4;
    for (int i = 0; i < 16; i++) {
      int hi = sh_hex_value(argv[6][2 * i]), lo = sh_hex_value(argv[6][2 * i + 1]);
      if (hi < 0 || lo < 0) return 4;
      nonce[i] = (uint8_t)(hi << 4 | lo);
    }
    fixed = nonce;
  }
  size_t len = strlen(argv[5]);
  size_t size = SH_SEALED_SIZE(len);
  char *out = malloc(size);
  size_t n = sh_seal(key, argv[1], argv[3], strtoull(argv[4], NULL, 10), (const uint8_t *)argv[5], len, fixed, out, size);
  if (!n) {
    fprintf(stderr, "seal failed\n");
    return 5;
  }
  fwrite(out, 1, n, stdout);
  free(out);
  return 0;
}
