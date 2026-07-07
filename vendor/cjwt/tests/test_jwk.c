#include "tinytest.h"
#include "cjwt.h"
#include <string.h>
#include <stdlib.h>

suite("cjwt jwk") {
  group("parse") {
    it("parses RSA JWK") {
      const char *jwk_json = "{"
          "\"kty\":\"RSA\","
          "\"kid\":\"test_kid\","
          "\"use\":\"sig\","
          "\"n\":\"o76m_D87p9B...example\","
          "\"e\":\"AQAB\""
      "}";

      cjwt_jwk_t *jwk = NULL;
      cjwt_code_t rv = cjwt_jwk_parse(jwk_json, &jwk);

      check_int_eq(CJWTE_OK, rv);
      check_not_null(jwk);
      check_int_eq(CJWT_KTY_RSA, jwk->kty);
      check_str_eq("test_kid", jwk->kid);
      check_str_eq("sig", jwk->use);

      cjwt_jwk_destroy(jwk);
    }

    it("parses EC JWK") {
      const char *jwk_json = "{"
          "\"kty\":\"EC\","
          "\"crv\":\"P-256\","
          "\"x\":\"f83OJ3D2x1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEUo\","
          "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\""
      "}";

      cjwt_jwk_t *jwk = NULL;
      cjwt_code_t rv = cjwt_jwk_parse(jwk_json, &jwk);

      check_int_eq(CJWTE_OK, rv);
      check_not_null(jwk);
      check_int_eq(CJWT_KTY_EC, jwk->kty);

      cjwt_jwk_destroy(jwk);
    }
  }
}
