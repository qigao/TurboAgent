#include "tinytest.h"
#include "cjwt.h"
#include <string.h>
#include <stdlib.h>

suite("cjwt encode") {
  group("alg none") {
    it("encodes and decodes without key") {
      cjwt_t jwt = {0};
      jwt.header.alg = alg_none;
      jwt.iss = "test_issuer";
      jwt.sub = "test_subject";

      char *output = NULL;
      cjwt_code_t rv = cjwt_encode(&jwt, NULL, 0, &output);

      check_int_eq(CJWTE_OK, rv);
      check_not_null(output);

      cjwt_t *decoded = NULL;
      rv = cjwt_decode(output, strlen(output), OPT_ALLOW_ALG_NONE, NULL, 0, 0, 0, &decoded);
      check_int_eq(CJWTE_OK, rv);
      check_str_eq("test_issuer", decoded->iss);
      check_str_eq("test_subject", decoded->sub);

      cjwt_destroy(decoded);
      free(output);
    }
  }

  group("hs256") {
    it("encodes and decodes with key") {
      int64_t iat = 123456789;
      cjwt_t jwt = {0};
      jwt.header.alg = alg_hs256;
      jwt.iss = "hs_issuer";
      jwt.iat = &iat;

      const char *key = "secret_key";
      char *output = NULL;
      cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)key, strlen(key), &output);

      check_int_eq(CJWTE_OK, rv);
      check_not_null(output);

      cjwt_t *decoded = NULL;
      rv = cjwt_decode(output, strlen(output), OPT_ALLOW_ONLY_HS_ALG,
                       (const uint8_t *)key, strlen(key), 0, 0, &decoded);
      check_int_eq(CJWTE_OK, rv);
      check_str_eq("hs_issuer", decoded->iss);
      check_int_eq(123456789LL, *decoded->iat);

      cjwt_destroy(decoded);
      free(output);
    }
  }
}
