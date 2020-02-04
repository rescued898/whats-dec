/*

  whats-dec -- WhatsApp media decryption

  This program decrypts media files that were encrypted via WhatsApp's
  end-to-end encryption. Because the keys are located on the ends (devices),
  we can decrypt these files by first pulling the keys from the device
  (or from a backup of the device).

  Ciphersuite:
    - key-exchange: none
    - bulk-encryption: AES-256-CBC
    - message-authentication (MAC): HMAC-SHA256
    - pseudo-random-function (PRF): HKDF-SHA256
*/
#define _CRT_SECURE_NO_WARNINGS
#include "crypto-aes256.h"
#include "crypto-base64.h"
#include "crypto-hex.h"
#include "crypto-sha256-hkdf.h"
#include "crypto-sha256-hmac.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(WIN32) || defined(_WIN32)
#define snprintf _snprintf
#endif

static const char *infostrings[6] = {
		"WhatsApp Video Keys", /* default value */
        "WhatsApp Image Keys",
		"WhatsApp Video Keys",
		"WhatsApp Audio Keys",
		"WhatsApp Document Keys",
        0
};

/**
 * This holds the configuration parsed from the command-line.
 */
struct configuration {
  size_t mediakey_length;
  unsigned char mediakey[32];
  char infilename[512];
  char outfilename[512];
  size_t mediatype;
};

static size_t media_type(const char *filename)
{
    static const char *types[6][32] = {
        {0},
        {
        "image", ".gif", ".jpg", ".jpeg", ".png", ".tiff", ".raw", ".svg", 0},
        {
        "video", ".mp4", ".mpeg", ".mpg", ".mpeg4", ".mpv", ".qt", ".quicktime",
        ".vc1", ".flv", ".vob", ".ogg", ".ogv", ".avi", ".mov", ".wmv",
        ".m4p", ".m4v", ".3gp", ".3g2", 0},
        {
        "audio", ".mp3", ".aiff", ".aac", ".flac", ".wav", ".webm", 0},
        {
        "doc", ".doc", ".pdf", 0
        },
        0
    };
    size_t i;

    for (i=0; i<4; i++) {
        size_t j;
        for (j=0; types[i][j]; j++) {
            const char *ext = types[i][j];
            if (strcmp(ext, filename + strlen(filename) - strlen(ext)) == 0)
                return i;
        }
    }
    return 0;
}

/**
 * For selftests, prints a message when the test fails/succeeds
 */
static int status(int test_result, const char *name) {
  if (test_result)
    fprintf(stderr, "[-] %s: failed\n", name);
  else
    fprintf(stderr, "[+] %s: succeeded\n", name);
  return test_result;
}

/**
 * Call all the quick selftest functions for the crypto modules, to make
 * sure all the routines are running well. It's not a comprehensive test,
 * though. */
static void selftest(void) {
  int failure_count = 0;

  failure_count += status(AES256_selftest(0), "AES-256");
  failure_count += status(base64_selftest(), "BASE64");
  failure_count += status(crypto_hkdf_selftest(), "HKDF-SHA256");
  exit(status(failure_count, "selftest") != 0);
}

/**
 * Prints a quick message telling people how to use this module.
 */
static void print_help(void) {
  fprintf(stderr,
          "Usage:\n whats-dec --key <key> --in <filename> --out <filename>\n");
  exit(1);
}

/**
 * Parse a single command-line parameter
 */
static void parse_param(struct configuration *cfg, const char *name,
                        const char *value) {
  /* --mediakey */
  if (strcmp(name, "key") == 0 || strcmp(name, "mediakey") == 0) {
    cfg->mediakey_length =
        hex_decode(value, cfg->mediakey, sizeof(cfg->mediakey));
    if (cfg->mediakey_length == sizeof(cfg->mediakey))
      return;
    cfg->mediakey_length = base64_decode(cfg->mediakey, sizeof(cfg->mediakey),
                                         value, strlen(value));
    if (cfg->mediakey_length == sizeof(cfg->mediakey))
      return;
    fprintf(stderr, "[-] invalid key, need %u-bytes encoded as hex or base64\n",
            (unsigned)sizeof(cfg->mediakey));
    exit(1);
  /* --in */
  } else if (strcmp(name, "in") == 0 || strcmp(name, "filename") == 0 ||
             strcmp(name, "infilename") == 0) {
    if (strlen(value) + 1 >= sizeof(cfg->infilename)) {
      fprintf(stderr, "[-] infilename too long\n");
      exit(1);
    } else {
      snprintf(cfg->infilename, sizeof(cfg->infilename), "%s", value);
    }
  /* --out */
  } else if (strcmp(name, "out") == 0 || strcmp(name, "outfilename") == 0) {
    if (strlen(value) + 1 >= sizeof(cfg->outfilename)) {
      fprintf(stderr, "[-] outfilename too long\n");
      exit(1);
    } 
    snprintf(cfg->outfilename, sizeof(cfg->outfilename), "%s", value);
    if (cfg->mediatype == 0)
        cfg->mediatype = media_type(cfg->outfilename);
    
  /* --type */
  } else if (strcmp(name, "type") == 0 || strcmp(name, "mediatype") == 0) {
      size_t t = media_type(name);
      if (t == 0) {
          fprintf(stderr, "[-] unknown media type=%s. Valid parms: video, audio, image, doc\n", value);
          exit(1);
      }
      cfg->mediatype = t;
  } else {
    fprintf(stderr, "[-] unknown parameter: --%s (try --help)\n", name);
    exit(1);
  }
}

struct configuration parse_command_line(int argc, char *argv[]) {
  int i;
  struct configuration cfg = {0,{0},{0},{0},0};

  for (i = 1; i < argc; i++) {
    if (argv[i][0] != '-') {
      fprintf(stderr, "[-] unexpected param: %s\n", argv[i]);
      break;
    }
    switch (argv[i][1]) {
    case '-':
      if (strcmp(argv[i], "--help") == 0)
        print_help();
      if (strcmp(argv[i], "--test") == 0)
        selftest();
      if (i + 1 < argc) {
        /* Of the form '--foo bar' */
        parse_param(&cfg, argv[i] + 2, argv[i + 1]);
        i++;
      } else {
        fprintf(stderr, "[-] missing expected parameter after '%s'\n", argv[i]);
        exit(1);
      }
      break;
    case 'h':
    case '?':
      print_help();
      break;
    default:
      fprintf(stderr, "[-] invalid parameter: -%c (try -h for help)\n",
              argv[i][1]);
      exit(1);
      break;
    }
  }

  return cfg;
}

void decrypt_stream(FILE *fp_in, FILE *fp_out, const unsigned char *aeskey,
                    const unsigned char *iv, const unsigned char *mackey) {
  unsigned char prevblock[16];
  size_t bytes_read;
  struct AES_ctx ctx;
  HMAC_CTX hmac;

  /*
   * Initialize the decryptions
   */
  AES_init_ctx_iv(&ctx, aeskey, iv);
  hmac_sha256_init(&hmac, mackey, 32);
  hmac_sha256_update(&hmac, iv, 16);

  /*
   * Read in at least one full block
   */
  bytes_read = fread(prevblock, 1, sizeof(prevblock), fp_in);
  if (bytes_read != sizeof(prevblock)) {
    printf("[-] file too short (%u bytes read, expected at least 16)\n",
           (unsigned)bytes_read);
    exit(1);
  }
  hmac_sha256_update(&hmac, prevblock, sizeof(prevblock));
  AES_CBC_decrypt_buffer(&ctx, prevblock, sizeof(prevblock));
  hex_print("[+] block[0] = ", prevblock, sizeof(prevblock));

  for (;;) {
    unsigned char block[16];
    size_t bytes_written;

    /*
     * Read the next block
     */
    bytes_read = fread(block, 1, sizeof(block), fp_in);

    /*
     * If a full block, then process it
     */
    if (bytes_read == sizeof(block)) {
      /* flush the previous block */
      bytes_written = fwrite(prevblock, 1, sizeof(prevblock), fp_out);
      if (bytes_written != sizeof(prevblock)) {
        printf("[-] error writing decrypted output\n");
        exit(1);
      }

      /* decrypt this block and store if for later */
      memcpy(prevblock, block, sizeof(prevblock));
      hmac_sha256_update(&hmac, prevblock, sizeof(prevblock));
      AES_CBC_decrypt_buffer(&ctx, prevblock, sizeof(prevblock));
      continue;
    }

    /* We've reached the end of the file. The current read needs to
     * be 10-bytes long representing the 'MAC'. The previous block
     * may be padded */
    if (bytes_read != 10) {
      printf("[-] expected 10 remaining bytes at end of file, found %u\n",
             (unsigned)bytes_read);
      exit(1);
    } else {
      unsigned padding_length;
      unsigned last_length;

      /* The number of bytes of padding are given by the last byte in the
       * the block, and should have a value from between [1..16]. If it's
       * over that number, then it means corruption happened, in which the
       * MAC (below) should also not check out */
      padding_length =  prevblock[15];
      if (padding_length > 16) {
        printf("[-] invalid padding length: %u (must be 16 or less)\n",
               padding_length);
        padding_length = 16;
      }

      /* Here we print the last block, minus the padding. If the padding
       * length is 16 bytes, then the final block will be empty */
      last_length = sizeof(prevblock) - padding_length;
      hex_print("[+] block[n] = ", prevblock, last_length);

      /* Here we print the padding and MAC. These are additional [11..26]
       * bytes at the end of the file that the FTI researchers falsely
       * believed were an exploit or malware. */
      hex_print("[+]-padding = ", prevblock + last_length, padding_length);
      hex_print("[+]-mac = ", block, bytes_read);

      /* Write the last block */
      bytes_written = fwrite(prevblock, 1, last_length, fp_out);
      if (bytes_written != last_length) {
        printf("[-] error writing decrypted output\n");
        exit(1);
      }

      /* calcualte the expected match and see if they match */
      {
        unsigned char finalmac[32];
        hmac_sha256_final(&hmac, finalmac, sizeof(finalmac));
        hex_print("[+] MAC = ", finalmac, sizeof(finalmac));
        if (memcmp(block, finalmac, 10) == 0) {
          printf("[+] matched! (verified not corrupted)\n");
        } else {
          printf("[-] match failed, file corrupted\n");
        }
      }

      break;
    }
  }
}

int main(int argc, char *argv[]) {
  struct configuration cfg;
  unsigned char okm[112] = {0};
  unsigned char iv[16] = {0};
  unsigned char aeskey[32] = {0};
  unsigned char mackey[32] = {0};
  FILE *fp_in;
  FILE *fp_out;
  static const char *medianames[6] = {"video", "image", "video", "audio", "doc", "unknown"};

  /* Read in the command-line configuration */
  cfg = parse_command_line(argc, argv);

  /*
   * Verify we have all the necessary parameters
   */
  if (cfg.mediakey_length == 0) {
    fprintf(stderr, "[-] missing key, use '--key' parameter\n");
    return 1;
  }
  if (cfg.infilename[0] == '\0') {
    fprintf(stderr, "[-] missing input file, use '--infilename' parameter\n");
    return 1;
  }
  if (cfg.outfilename[0] == '\0') {
    fprintf(stderr, "[-] missing output file, use '--outfilename' parameter\n");
    return 1;
  }

  /*
   * Expand the key
   * This is a common issue with encryption in that we need more than
   * a simple 'key', but also an 'initialization vector' (aka. 'nonce')
   * and a verification or 'message authentication code' key. Thus,
   * we need to take the original input 'mediakey' and expand or stretch
   * it using a pseudo-random function.
   */
  crypto_hkdf(0, 0, cfg.mediakey, cfg.mediakey_length,
              infostrings[cfg.mediatype], strlen(infostrings[cfg.mediatype]), 
              okm, sizeof(okm));
  memcpy(iv, okm + 0, sizeof(iv));
  memcpy(aeskey, okm + 16, sizeof(aeskey));
  memcpy(mackey, okm + 48, sizeof(mackey));

  /* Print the extracted values */
  printf("[+] ciphertext = %s\n", cfg.infilename);
  printf("[+] plaintext  = %s\n", cfg.outfilename);
  printf("[+] mediatype = %s\n", medianames[cfg.mediatype]);
  printf("[+] info = %s\n", infostrings[cfg.mediatype]);
  hex_print("[+] mediakey = ", cfg.mediakey, cfg.mediakey_length);
  hex_print("[+] mediakey.iv = ", iv, sizeof(iv));
  hex_print("[+] mediakey.aeskey = ", aeskey, sizeof(aeskey));
  hex_print("[+] mediakey.mackey = ", mackey, sizeof(mackey));

  /*
   * Open the files
   */
  fp_in = fopen(cfg.infilename, "rb");
  if (fp_in == NULL) {
    printf("[-] %s: %s\n", cfg.infilename, strerror(errno));
    exit(1);
  }
  fp_out = fopen(cfg.outfilename, "wb");
  if (fp_out == NULL) {
    printf("[-] %s: %s\n", cfg.outfilename, strerror(errno));
    exit(1);
  }

  decrypt_stream(fp_in, fp_out, aeskey, iv, mackey);

  fclose(fp_in);
  fclose(fp_out);

  return 0;
}
