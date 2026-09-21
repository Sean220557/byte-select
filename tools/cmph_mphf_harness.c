/* Small CMPH adapter used by experiment_mphf_128.py.
 * CMPH's byte-vector adapter expects each key to be prefixed by uint32 length. */
#include <cmph.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CMPH_ALGO parse_algo(const char *s) {
  if (!strcmp(s, "FCH")) return CMPH_FCH;
  if (!strcmp(s, "BDZ")) return CMPH_BDZ;
  if (!strcmp(s, "CHD")) return CMPH_CHD;
  return CMPH_COUNT;
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  FILE *file = fopen(argv[2], "rb");
  if (!file) return 3;
  fseek(file, 0, SEEK_END);
  const long bytes = ftell(file);
  rewind(file);
  if (bytes <= 0 || bytes % 8) return 4;
  const unsigned count = (unsigned)(bytes / 8);
  unsigned char **keys = calloc(count, sizeof(*keys));
  for (unsigned i = 0; i < count; ++i) {
    keys[i] = calloc(1, 12);
    const unsigned length = 8;
    memcpy(keys[i], &length, sizeof(length));
    if (fread(keys[i] + 4, 1, 8, file) != 8) return 5;
  }
  fclose(file);

  cmph_io_adapter_t *source = cmph_io_byte_vector_adapter(keys, count);
  cmph_config_t *config = cmph_config_new(source);
  const CMPH_ALGO algorithm = parse_algo(argv[1]);
  if (algorithm == CMPH_COUNT) return 6;
  cmph_config_set_algo(config, algorithm);
  cmph_t *hash = cmph_new(config);
  cmph_config_destroy(config);
  if (!hash) return 7;

  unsigned char seen[256] = {0};
  unsigned unique = 0;
  for (unsigned i = 0; i < count; ++i) {
    const unsigned value = cmph_search(hash, (const char *)keys[i] + 4, 8);
    if (value < 256 && !seen[value]) {
      seen[value] = 1;
      ++unique;
    }
  }
  printf("{\"success\":%s,\"unique\":%u,\"packed_bytes\":%u,\"cmph_size\":%u}\n",
         unique == count ? "true" : "false", unique,
         cmph_packed_size(hash), cmph_size(hash));

  cmph_destroy(hash);
  cmph_io_byte_vector_adapter_destroy(source);
  for (unsigned i = 0; i < count; ++i) free(keys[i]);
  free(keys);
  return unique == count ? 0 : 8;
}
