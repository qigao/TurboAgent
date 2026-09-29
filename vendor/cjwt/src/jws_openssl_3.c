/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <stddef.h>
#include <stdint.h>

#define _CRT_SECURE_NO_WARNINGS
#include "cjwt.h"
#include "jws.h"
#include "utils.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/
static cjwt_code_t process_okp_jwk(json_value_t *json, EVP_PKEY **pkey);

cjwt_code_t verify_hmac(const EVP_MD *sha, const struct sig_input *in)
{
    cjwt_code_t rv     = CJWTE_SIGNATURE_VALIDATION_FAILED;
    EVP_MD_CTX *md_ctx = NULL;
    EVP_PKEY *pkey     = NULL;
    uint8_t buff[EVP_MAX_MD_SIZE];
    size_t size = sizeof(buff);

    if (INT_MAX < in->key.len) {
        return CJWTE_KEY_TOO_LARGE;
    }

    md_ctx = EVP_MD_CTX_new();
    pkey   = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, in->key.data, (int) in->key.len);

    if (md_ctx && pkey
        && (1 == EVP_DigestSignInit(md_ctx, NULL, sha, NULL, pkey))
        && (1 == EVP_DigestSignUpdate(md_ctx, in->full.data, in->full.len))
        && (1 == EVP_DigestSignFinal(md_ctx, buff, &size))
        && (in->sig.len == size)
        && (0 == CRYPTO_memcmp(in->sig.data, buff, size)))
    {
        rv = CJWTE_OK;
    }

    EVP_PKEY_free(pkey);
    EVP_MD_CTX_free(md_ctx);
    return rv;
}

int add_padding(int type, EVP_PKEY_CTX *ctx, int padding)
{
    if (EVP_PKEY_EC == type) {
        return 1;
    }

    return EVP_PKEY_CTX_set_rsa_padding(ctx, padding);
}

int calc_sig(int type, const struct sig_input *in, uint8_t **sig, int *len)
{
    int rv               = 0; /* Match the other openssl symantics for consistency */
    ECDSA_SIG *ecdsa_sig = NULL;
    BIGNUM *pr           = NULL;
    BIGNUM *ps           = NULL;
    int new_sig_len      = 0;
    uint8_t *new_sig     = NULL;

    if (EVP_PKEY_RSA == type) {
        *sig = (uint8_t *) in->sig.data;
        *len = (int)in->sig.len;
        return 1;
    }

    ecdsa_sig = ECDSA_SIG_new();
    if (ecdsa_sig == NULL) {
        return 0;
    }

    /* Read out the r,s numbers from the signature for later.
     * We must convert from this format into DEC because that's
     * all openssl supports. */
    pr = BN_bin2bn(in->sig.data, (int) in->sig.len / 2, NULL);
    ps = BN_bin2bn(in->sig.data + in->sig.len / 2, (int) in->sig.len / 2, NULL);

    if (1 == ECDSA_SIG_set0(ecdsa_sig, pr, ps)) {
        new_sig_len = i2d_ECDSA_SIG(ecdsa_sig, &new_sig);
        if (0 <= new_sig_len) {
            /* We don't own the memory now, don't free it. */
            pr = NULL;
            ps = NULL;

            if (0 < new_sig_len) {
                *sig    = new_sig;
                *len    = new_sig_len;
                new_sig = NULL; /* Passed back now, so don't free the buffer. */
                rv      = 1;
            }
        }
    }

    OPENSSL_free(new_sig);
    ECDSA_SIG_free(ecdsa_sig);
    BN_free(ps);
    BN_free(pr);

    return rv;
}

cjwt_code_t verify_most(const EVP_MD *sha, const struct sig_input *in, int type, int padding)
{
    cjwt_code_t rv         = CJWTE_SIGNATURE_VALIDATION_FAILED;
    EVP_MD_CTX *md_ctx     = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;
    EVP_PKEY *pkey         = NULL;
    BIO *keybio            = NULL;
    int sig_len            = 0;
    uint8_t *sig           = NULL;

    if (!in) return CJWTE_INVALID_PARAMETERS;

    if (in->pkey) {
        pkey = (EVP_PKEY *)in->pkey;
        EVP_PKEY_up_ref(pkey);
    } else {
        if ((0 == in->key.len) || (NULL == in->key.data)) {
            return CJWTE_SIGNATURE_MISSING_KEY;
        }

        /* Read the RSA key in from a PEM encoded blob of memory */
        keybio = BIO_new_mem_buf(in->key.data, (int) in->key.len);
        if (!keybio) {
            return CJWTE_OUT_OF_MEMORY;
        }

        pkey = PEM_read_bio_PUBKEY(keybio, NULL, NULL, NULL);
        if (!pkey) {
            rv = CJWTE_SIGNATURE_INVALID_KEY;
            goto done;
        }
    }

    if (type != EVP_PKEY_id(pkey)) {
        rv = CJWTE_SIGNATURE_INVALID_KEY;
        goto done;
    }

    md_ctx = EVP_MD_CTX_create();

    if (md_ctx
        && (1 == calc_sig(type, in, &sig, &sig_len))
        && (1 == EVP_DigestVerifyInit(md_ctx, &pkey_ctx, sha, NULL, pkey))
        && (type != EVP_PKEY_ED25519 && type != EVP_PKEY_ED448 ? 0 < add_padding(type, pkey_ctx, padding) : 1)
        && (1 == EVP_DigestVerifyUpdate(md_ctx, in->full.data, in->full.len))
        && (1 == EVP_DigestVerifyFinal(md_ctx, sig, sig_len)))
    {
        rv = CJWTE_OK;
    }

done:

    if (sig != in->sig.data) OPENSSL_free(sig);

    if (keybio) BIO_free(keybio);
    if (pkey) EVP_PKEY_free(pkey);
    if (md_ctx) EVP_MD_CTX_free(md_ctx);

    return rv;
}


/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
cjwt_code_t jws_verify_signature(const cjwt_t *jwt, const struct sig_input *in)
{
    switch (jwt->header.alg) {
        case alg_es256:
            return verify_most(EVP_sha256(), in, EVP_PKEY_EC, 0);
        case alg_es384:
            return verify_most(EVP_sha384(), in, EVP_PKEY_EC, 0);
        case alg_es512:
            return verify_most(EVP_sha512(), in, EVP_PKEY_EC, 0);

        case alg_hs256:
            return verify_hmac(EVP_sha256(), in);
        case alg_hs384:
            return verify_hmac(EVP_sha384(), in);
        case alg_hs512:
            return verify_hmac(EVP_sha512(), in);

        case alg_ps256:
            return verify_most(EVP_sha256(), in, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING);
        case alg_ps384:
            return verify_most(EVP_sha384(), in, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING);
        case alg_ps512:
            return verify_most(EVP_sha512(), in, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING);

        case alg_rs256:
            return verify_most(EVP_sha256(), in, EVP_PKEY_RSA, RSA_PKCS1_PADDING);
        case alg_rs384:
            return verify_most(EVP_sha384(), in, EVP_PKEY_RSA, RSA_PKCS1_PADDING);
        case alg_rs512:
            return verify_most(EVP_sha512(), in, EVP_PKEY_RSA, RSA_PKCS1_PADDING);

        case alg_es256k:
            return verify_most(EVP_sha256(), in, EVP_PKEY_EC, 0);

        case alg_eddsa:
            return verify_most(NULL, in, EVP_PKEY_ED25519, 0);

        default:
            break;
    }

    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

static cjwt_code_t sign_hmac(const EVP_MD *sha, const uint8_t *full, size_t full_len,
                             const uint8_t *key, size_t key_len,
                             uint8_t **sig, size_t *sig_len)
{
    cjwt_code_t rv     = CJWTE_OUT_OF_MEMORY;
    EVP_MD_CTX *md_ctx = NULL;
    EVP_PKEY *pkey     = NULL;
    uint8_t buff[EVP_MAX_MD_SIZE];
    size_t size = sizeof(buff);

    md_ctx = EVP_MD_CTX_new();
    pkey   = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, key, (int) key_len);

    if (md_ctx && pkey
        && (1 == EVP_DigestSignInit(md_ctx, NULL, sha, NULL, pkey))
        && (1 == EVP_DigestSignUpdate(md_ctx, full, full_len))
        && (1 == EVP_DigestSignFinal(md_ctx, buff, &size)))
    {
        *sig = malloc(size);
        if (*sig) {
            memcpy(*sig, buff, size);
            *sig_len = size;
            rv = CJWTE_OK;
        }
    }

    EVP_PKEY_free(pkey);
    EVP_MD_CTX_free(md_ctx);
    return rv;
}

static cjwt_code_t sign_most(const EVP_MD *sha, const uint8_t *full, size_t full_len,
                             const uint8_t *key_data, size_t key_len,
                             int type, int padding,
                             uint8_t **sig, size_t *sig_len)
{
    cjwt_code_t rv         = CJWTE_OUT_OF_MEMORY;
    EVP_MD_CTX *md_ctx     = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;
    EVP_PKEY *pkey         = NULL;
    BIO *keybio            = NULL;
    uint8_t *tmp_sig       = NULL;
    size_t tmp_sig_len     = 0;

    keybio = BIO_new_mem_buf(key_data, (int) key_len);
    if (!keybio) return CJWTE_OUT_OF_MEMORY;

    pkey = PEM_read_bio_PrivateKey(keybio, NULL, NULL, NULL);
    if (!pkey) {
        BIO_free(keybio);
        return CJWTE_SIGNATURE_INVALID_KEY;
    }

    md_ctx = EVP_MD_CTX_create();
    if (md_ctx
        && (1 == EVP_DigestSignInit(md_ctx, &pkey_ctx, sha, NULL, pkey))
        && (type != EVP_PKEY_ED25519 && type != EVP_PKEY_ED448 ? 0 < add_padding(type, pkey_ctx, padding) : 1)
        && (1 == EVP_DigestSignUpdate(md_ctx, full, full_len))
        && (1 == EVP_DigestSignFinal(md_ctx, NULL, &tmp_sig_len)))
    {
        tmp_sig = malloc(tmp_sig_len);
        if (tmp_sig && (1 == EVP_DigestSignFinal(md_ctx, tmp_sig, &tmp_sig_len))) {
            if (type == EVP_PKEY_EC) {
                /* EC signature is DER (Sequence of r, s). Need to convert to raw r|s */
                ECDSA_SIG *ec_sig = d2i_ECDSA_SIG(NULL, (const uint8_t **) &tmp_sig, (long)tmp_sig_len);
                if (ec_sig) {
                    const BIGNUM *r, *s;
                    ECDSA_SIG_get0(ec_sig, &r, &s);
                    int degree = EVP_PKEY_get_bits(pkey);
                    int order_len = (degree + 7) / 8;
                    *sig_len = 2 * order_len;
                    *sig = calloc(1, *sig_len);
                    if (*sig) {
                        BN_bn2binpad(r, *sig, order_len);
                        BN_bn2binpad(s, *sig + order_len, order_len);
                        rv = CJWTE_OK;
                    }
                    ECDSA_SIG_free(ec_sig);
                }
                /* tmp_sig was incremented by d2i, need to free original pointer if we had it, 
                   but d2i_ECDSA_SIG is a bit tricky with the pointer. 
                   Actually, d2i_ECDSA_SIG updates the pointer passed in. 
                   We should have used a temporary pointer for d2i. */
            } else {
                *sig = tmp_sig;
                *sig_len = tmp_sig_len;
                tmp_sig = NULL;
                rv = CJWTE_OK;
            }
        }
    }

    free(tmp_sig);
    BIO_free(keybio);
    EVP_PKEY_free(pkey);
    EVP_MD_CTX_free(md_ctx);
    return rv;
}

cjwt_code_t jws_sign(const cjwt_alg_t alg, const uint8_t *full, size_t full_len,
                     const uint8_t *key, size_t key_len,
                     uint8_t **sig, size_t *sig_len)
{
    switch (alg) {
        case alg_es256:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);
        case alg_es384:
            return sign_most(EVP_sha384(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);
        case alg_es512:
            return sign_most(EVP_sha512(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);

        case alg_hs256:
            return sign_hmac(EVP_sha256(), full, full_len, key, key_len, sig, sig_len);
        case alg_hs384:
            return sign_hmac(EVP_sha384(), full, full_len, key, key_len, sig, sig_len);
        case alg_hs512:
            return sign_hmac(EVP_sha512(), full, full_len, key, key_len, sig, sig_len);

        case alg_ps256:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, sig, sig_len);
        case alg_ps384:
            return sign_most(EVP_sha384(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, sig, sig_len);
        case alg_ps512:
            return sign_most(EVP_sha512(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, sig, sig_len);

        case alg_rs256:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PADDING, sig, sig_len);
        case alg_rs384:
            return sign_most(EVP_sha384(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PADDING, sig, sig_len);
        case alg_rs512:
            return sign_most(EVP_sha512(), full, full_len, key, key_len, EVP_PKEY_RSA, RSA_PKCS1_PADDING, sig, sig_len);

        case alg_es256k:
            return sign_most(EVP_sha256(), full, full_len, key, key_len, EVP_PKEY_EC, 0, sig, sig_len);

        case alg_eddsa:
            return sign_most(NULL, full, full_len, key, key_len, EVP_PKEY_ED25519, 0, sig, sig_len);

        default:
            break;
    }

    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

static int set_one_bn(OSSL_PARAM_BLD *build, const char *ossl_name, json_value_t *val)
{
    if (!val || turbo_json_type(val) != TURBO_JSON_STRING) return 0;
    size_t len = 0;
    const char *s = turbo_json_string(val);
    uint8_t *bin = b64url_decode_with_alloc((const uint8_t *)s, strlen(s), &len);
    if (!bin) return 0;
    BIGNUM *bn = BN_bin2bn(bin, (int)len, NULL);
    free(bin);
    if (!bn) return 0;
    OSSL_PARAM_BLD_push_BN(build, ossl_name, bn);
    BN_free(bn); // OSSL_PARAM_BLD_push_BN duplicates it
    return 1;
}

static cjwt_code_t process_rsa_jwk(json_value_t *json, EVP_PKEY **pkey)
{
    OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();
    if (!build) return CJWTE_OUT_OF_MEMORY;

    int ok = 1;
    ok &= set_one_bn(build, OSSL_PKEY_PARAM_RSA_N, turbo_json_object_get(json, "n"));
    ok &= set_one_bn(build, OSSL_PKEY_PARAM_RSA_E, turbo_json_object_get(json, "e"));

    json_value_t *d = turbo_json_object_get(json, "d");
    if (d) {
        ok &= set_one_bn(build, OSSL_PKEY_PARAM_RSA_D, d);
        set_one_bn(build, OSSL_PKEY_PARAM_RSA_FACTOR1, turbo_json_object_get(json, "p"));
        set_one_bn(build, OSSL_PKEY_PARAM_RSA_FACTOR2, turbo_json_object_get(json, "q"));
        set_one_bn(build, OSSL_PKEY_PARAM_RSA_EXPONENT1, turbo_json_object_get(json, "dp"));
        set_one_bn(build, OSSL_PKEY_PARAM_RSA_EXPONENT2, turbo_json_object_get(json, "dq"));
        set_one_bn(build, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, turbo_json_object_get(json, "qi"));
    }

    if (!ok) { OSSL_PARAM_BLD_free(build); return CJWTE_INVALID_PARAMETERS; }

    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    if (pctx && (EVP_PKEY_fromdata_init(pctx) > 0)) {
        EVP_PKEY_fromdata(pctx, pkey, EVP_PKEY_KEYPAIR, params);
    }
    EVP_PKEY_CTX_free(pctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    return (*pkey) ? CJWTE_OK : CJWTE_INVALID_PARAMETERS;
}

static cjwt_code_t process_ec_jwk(json_value_t *json, EVP_PKEY **pkey)
{
    const char *crv_str = turbo_json_get_string(json, "crv");
    if (!crv_str) return CJWTE_INVALID_PARAMETERS;

    const char *ossl_crv = crv_str;
    if (!strcmp(ossl_crv, "P-256")) ossl_crv = "prime256v1";
    else if (!strcmp(ossl_crv, "P-384")) ossl_crv = "secp384r1";
    else if (!strcmp(ossl_crv, "P-521")) ossl_crv = "secp521r1";
    else if (!strcmp(ossl_crv, "secp256k1")) ossl_crv = "secp256k1";
    else if (!strcmp(ossl_crv, "K-256")) ossl_crv = "secp256k1";

    OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(build, OSSL_PKEY_PARAM_GROUP_NAME, ossl_crv, 0);

    size_t x_len = 0, y_len = 0;
    json_value_t *x_json = turbo_json_object_get(json, "x");
    json_value_t *y_json = turbo_json_object_get(json, "y");
    const char *x_str = x_json ? turbo_json_string(x_json) : NULL;
    const char *y_str = y_json ? turbo_json_string(y_json) : NULL;
    uint8_t *x_bin = x_str ? b64url_decode_with_alloc((const uint8_t *)x_str, strlen(x_str), &x_len) : NULL;
    uint8_t *y_bin = y_str ? b64url_decode_with_alloc((const uint8_t *)y_str, strlen(y_str), &y_len) : NULL;

    if (x_bin && y_bin) {
        /* Construct uncompressed point: 0x04 | x | y */
        size_t pub_len = 1 + x_len + y_len;
        uint8_t *pub = malloc(pub_len);
        if (pub) {
            pub[0] = 0x04;
            memcpy(pub + 1, x_bin, x_len);
            memcpy(pub + 1 + x_len, y_bin, y_len);
            OSSL_PARAM_BLD_push_octet_string(build, OSSL_PKEY_PARAM_PUB_KEY, pub, pub_len);
            free(pub);
        }
    }
    free(x_bin); free(y_bin);

    set_one_bn(build, OSSL_PKEY_PARAM_PRIV_KEY, turbo_json_object_get(json, "d"));

    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (pctx && (EVP_PKEY_fromdata_init(pctx) > 0)) {
        EVP_PKEY_fromdata(pctx, pkey, EVP_PKEY_KEYPAIR, params);
    }
    EVP_PKEY_CTX_free(pctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    return (*pkey) ? CJWTE_OK : CJWTE_INVALID_PARAMETERS;
}

cjwt_code_t jws_jwk_to_pkey(const cjwt_jwk_t *jwk, void **pkey)
{
    if (!jwk || !jwk->key_json) return CJWTE_INVALID_PARAMETERS;
    if (jwk->kty == CJWT_KTY_RSA) return process_rsa_jwk(jwk->key_json, (EVP_PKEY **)pkey);
    if (jwk->kty == CJWT_KTY_EC) return process_ec_jwk(jwk->key_json, (EVP_PKEY **)pkey);
    if (jwk->kty == CJWT_KTY_OKP) return process_okp_jwk(jwk->key_json, (EVP_PKEY **)pkey);
    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

static cjwt_code_t process_okp_jwk(json_value_t *json, EVP_PKEY **pkey)
{
    const char *crv_str = turbo_json_get_string(json, "crv");
    if (!crv_str) return CJWTE_INVALID_PARAMETERS;

    const char *ossl_name = NULL;
    if (!strcmp(crv_str, "Ed25519")) ossl_name = "ED25519";
    else if (!strcmp(crv_str, "Ed448")) ossl_name = "ED448";
    else return CJWTE_SIGNATURE_UNSUPPORTED_ALG;

    OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();

    set_one_bn(build, OSSL_PKEY_PARAM_PUB_KEY, turbo_json_object_get(json, "x"));
    set_one_bn(build, OSSL_PKEY_PARAM_PRIV_KEY, turbo_json_object_get(json, "d"));

    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, ossl_name, NULL);
    if (pctx && (EVP_PKEY_fromdata_init(pctx) > 0)) {
        EVP_PKEY_fromdata(pctx, pkey, EVP_PKEY_KEYPAIR, params);
    }
    EVP_PKEY_CTX_free(pctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    return (*pkey) ? CJWTE_OK : CJWTE_INVALID_PARAMETERS;
}

void jws_pkey_free(void *pkey)
{
    if (pkey) {
        EVP_PKEY_free((EVP_PKEY *)pkey);
    }
}
