/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <totem/esb_benchmark.h>
#include <totem/esb_v3_crypto.h>
#include <totem/esb_v3_keys.h>

LOG_MODULE_REGISTER(totem_esb_v3_crypto, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define TOTEM_ESB_V3_LINK_COUNT 2U
#define TOTEM_ESB_V3_CCM_ALG                                                               \
    PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, TOTEM_ESB_V3_TAG_SIZE)
#define TOTEM_ESB_V3_DOMAIN_VERSION 0xA0U
#define TOTEM_ESB_V3_DOMAIN_DOWNLINK BIT(2)
#define TOTEM_ESB_V3_DOMAIN_ROOT BIT(3)

struct totem_esb_v3_link_keys {
    psa_key_id_t root_ccm;
    psa_key_id_t root_cmac;
    psa_key_id_t active;
    psa_key_id_t pending;
    uint64_t active_session;
    uint64_t pending_session;
};

static struct totem_esb_v3_link_keys links[TOTEM_ESB_V3_LINK_COUNT];
static bool crypto_initialized;

static int status_to_errno(psa_status_t status) {
    switch (status) {
    case PSA_SUCCESS:
        return 0;
    case PSA_ERROR_INVALID_SIGNATURE:
        return -EACCES;
    case PSA_ERROR_INSUFFICIENT_MEMORY:
        return -ENOMEM;
    case PSA_ERROR_NOT_PERMITTED:
        return -EPERM;
    case PSA_ERROR_INVALID_ARGUMENT:
        return -EINVAL;
    case PSA_ERROR_NOT_SUPPORTED:
        return -ENOTSUP;
    case PSA_ERROR_BAD_STATE:
        return -EIO;
    default:
        return -EIO;
    }
}

static int import_aes_key(const uint8_t key[TOTEM_ESB_V3_KEY_SIZE],
                          psa_key_usage_t usage, psa_algorithm_t algorithm,
                          psa_key_id_t *key_id) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, algorithm);
    psa_status_t status =
        psa_import_key(&attributes, key, TOTEM_ESB_V3_KEY_SIZE, key_id);
    psa_reset_key_attributes(&attributes);
    return status_to_errno(status);
}

static bool ccm_known_answer_rejects(
    psa_key_id_t key_id, const uint8_t nonce[TOTEM_ESB_V3_NONCE_SIZE],
    const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
    size_t ciphertext_len, size_t plaintext_len) {
    uint8_t output[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
    size_t output_len = 0;
    psa_status_t status = psa_aead_decrypt(
        key_id, TOTEM_ESB_V3_CCM_ALG, nonce, TOTEM_ESB_V3_NONCE_SIZE, aad,
        aad_len, ciphertext, ciphertext_len, output, plaintext_len, &output_len);
    mbedtls_platform_zeroize(output, sizeof(output));
    return status == PSA_ERROR_INVALID_SIGNATURE;
}

static void destroy_key(psa_key_id_t *key_id) {
    if (*key_id != 0) {
        (void)psa_destroy_key(*key_id);
        *key_id = 0;
    }
}

static int run_known_answer_tests(void) {
    static const uint8_t key[TOTEM_ESB_V3_KEY_SIZE] = {
        0x43, 0xb1, 0xa6, 0xbc, 0x8d, 0x0d, 0x22, 0xd6,
        0xd1, 0xca, 0x95, 0xc1, 0x85, 0x93, 0xcc, 0xa5,
    };
    static const uint8_t plaintext[] = {
        0xa2, 0xb3, 0x81, 0xc7, 0xd1, 0x54, 0x5c, 0x40,
        0x8f, 0xe2, 0x98, 0x17, 0xa2, 0x1d, 0xc4, 0x35,
        0xa1, 0x54, 0xc8, 0x72, 0x56, 0x34, 0x6b, 0x05,
    };
    static const uint8_t nonce[TOTEM_ESB_V3_NONCE_SIZE] = {
        0x98, 0x82, 0x57, 0x8e, 0x75, 0x0b, 0x96,
        0x82, 0xc6, 0xca, 0x7f, 0x8f, 0x86,
    };
    static const uint8_t aad[] = {
        0x20, 0x84, 0xf3, 0x86, 0x1c, 0x9a, 0xd0, 0xcc,
        0xee, 0x7c, 0x63, 0xa7, 0xe0, 0x5a, 0xec, 0xe5,
        0xdb, 0x8b, 0x34, 0xbd, 0x87, 0x24, 0xcc, 0x06,
        0xb4, 0xca, 0x99, 0xa7, 0xf9, 0xc4, 0x91, 0x4f,
    };
    static const uint8_t expected[] = {
        0xcc, 0x69, 0xed, 0x76, 0x98, 0x5e, 0x0e, 0xd4,
        0xc8, 0x36, 0x5a, 0x72, 0x77, 0x5e, 0x5a, 0x19,
        0xbf, 0xcc, 0xc7, 0x1a, 0xeb, 0x11, 0x6c, 0x85,
        0xa8, 0xc7, 0x46, 0x77,
    };
    static const uint8_t cmac_context[] = {
        0x5a, 0x6d, 0x4b, 0x33, 0x73, 0x65, 0x73, 0x73,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    };
    static const uint8_t expected_cmac[TOTEM_ESB_V3_KEY_SIZE] = {
        0x32, 0x78, 0xc8, 0xce, 0xf1, 0xe8, 0xc9, 0x55,
        0x23, 0xda, 0xdf, 0x2a, 0x27, 0xb8, 0x30, 0xda,
    };

    psa_key_id_t kat_key = 0;
    psa_key_id_t wrong_key = 0;
    psa_key_id_t cmac_key = 0;
    uint8_t ciphertext[sizeof(expected)];
    uint8_t decrypted[sizeof(plaintext)];
    uint8_t tampered[sizeof(expected)];
    uint8_t tampered_aad[sizeof(aad)];
    uint8_t tampered_nonce[sizeof(nonce)];
    uint8_t wrong_key_bytes[sizeof(key)];
    uint8_t cmac_output[TOTEM_ESB_V3_KEY_SIZE];
    int err = import_aes_key(key, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
                             TOTEM_ESB_V3_CCM_ALG, &kat_key);
    if (err != 0) {
        return err;
    }

    size_t ciphertext_len = 0;
    psa_status_t status = psa_aead_encrypt(
        kat_key, TOTEM_ESB_V3_CCM_ALG, nonce, sizeof(nonce), aad, sizeof(aad),
        plaintext, sizeof(plaintext), ciphertext, sizeof(ciphertext),
        &ciphertext_len);
    if (status != PSA_SUCCESS || ciphertext_len != sizeof(expected) ||
        memcmp(ciphertext, expected, sizeof(expected)) != 0) {
        err = -EIO;
        goto out;
    }

    size_t decrypted_len = 0;
    status = psa_aead_decrypt(
        kat_key, TOTEM_ESB_V3_CCM_ALG, nonce, sizeof(nonce), aad, sizeof(aad),
        ciphertext, ciphertext_len, decrypted, sizeof(decrypted), &decrypted_len);
    if (status != PSA_SUCCESS || decrypted_len != sizeof(plaintext) ||
        memcmp(decrypted, plaintext, sizeof(plaintext)) != 0) {
        err = -EIO;
        goto out;
    }

    memcpy(tampered, ciphertext, sizeof(tampered));
    tampered[0] ^= 1U;
    if (!ccm_known_answer_rejects(
            kat_key, nonce, aad, sizeof(aad), tampered, sizeof(tampered),
            sizeof(plaintext))) {
        err = -EIO;
        goto out;
    }

    memcpy(tampered, ciphertext, sizeof(tampered));
    tampered[sizeof(tampered) - 1U] ^= 1U;
    if (!ccm_known_answer_rejects(
            kat_key, nonce, aad, sizeof(aad), tampered, sizeof(tampered),
            sizeof(plaintext))) {
        err = -EIO;
        goto out;
    }

    memcpy(tampered_aad, aad, sizeof(tampered_aad));
    tampered_aad[0] ^= 1U;
    if (!ccm_known_answer_rejects(
            kat_key, nonce, tampered_aad, sizeof(tampered_aad), ciphertext,
            ciphertext_len, sizeof(plaintext))) {
        err = -EIO;
        goto out;
    }

    memcpy(tampered_nonce, nonce, sizeof(tampered_nonce));
    tampered_nonce[0] ^= 1U;
    if (!ccm_known_answer_rejects(
            kat_key, tampered_nonce, aad, sizeof(aad), ciphertext,
            ciphertext_len, sizeof(plaintext))) {
        err = -EIO;
        goto out;
    }

    memcpy(wrong_key_bytes, key, sizeof(wrong_key_bytes));
    wrong_key_bytes[0] ^= 1U;
    err = import_aes_key(
        wrong_key_bytes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
        TOTEM_ESB_V3_CCM_ALG, &wrong_key);
    mbedtls_platform_zeroize(wrong_key_bytes, sizeof(wrong_key_bytes));
    if (err != 0) {
        goto out;
    }
    if (!ccm_known_answer_rejects(
            wrong_key, nonce, aad, sizeof(aad), ciphertext, ciphertext_len,
            sizeof(plaintext))) {
        err = -EIO;
        goto out;
    }
    destroy_key(&wrong_key);

    err = import_aes_key(
        key, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_CMAC, &cmac_key);
    if (err != 0) {
        goto out;
    }
    size_t cmac_output_len = 0;
    status = psa_mac_compute(
        cmac_key, PSA_ALG_CMAC, cmac_context, sizeof(cmac_context),
        cmac_output, sizeof(cmac_output), &cmac_output_len);
    if (status != PSA_SUCCESS ||
        cmac_output_len != sizeof(expected_cmac) ||
        memcmp(cmac_output, expected_cmac, sizeof(expected_cmac)) != 0) {
        mbedtls_platform_zeroize(cmac_output, sizeof(cmac_output));
        err = -EIO;
        goto out;
    }
    mbedtls_platform_zeroize(cmac_output, sizeof(cmac_output));
    err = 0;

out:
    destroy_key(&wrong_key);
    destroy_key(&cmac_key);
    destroy_key(&kat_key);
    mbedtls_platform_zeroize(ciphertext, sizeof(ciphertext));
    mbedtls_platform_zeroize(decrypted, sizeof(decrypted));
    mbedtls_platform_zeroize(tampered, sizeof(tampered));
    mbedtls_platform_zeroize(tampered_aad, sizeof(tampered_aad));
    mbedtls_platform_zeroize(tampered_nonce, sizeof(tampered_nonce));
    mbedtls_platform_zeroize(wrong_key_bytes, sizeof(wrong_key_bytes));
    mbedtls_platform_zeroize(cmac_output, sizeof(cmac_output));
    return err;
}

static int import_link_roots(uint8_t source,
                             const uint8_t key[TOTEM_ESB_V3_KEY_SIZE]) {
    int err = import_aes_key(
        key, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
        TOTEM_ESB_V3_CCM_ALG, &links[source].root_ccm);
    if (err != 0) {
        return err;
    }
    err = import_aes_key(key, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_CMAC,
                         &links[source].root_cmac);
    if (err != 0) {
        destroy_key(&links[source].root_ccm);
    }
    return err;
}

int totem_esb_v3_crypto_init(void) {
    if (crypto_initialized) {
        return 0;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        LOG_ERR("PSA crypto initialization failed (%d)", status);
        return status_to_errno(status);
    }

    int err = run_known_answer_tests();
    if (err != 0) {
        LOG_ERR("AES-CCM/MIC4 or CMAC known-answer self-test failed (%d)", err);
        return err;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    static const uint8_t left_key[TOTEM_ESB_V3_KEY_SIZE] = {
        TOTEM_ESB_V3_LEFT_KEY_BYTES};
    static const uint8_t right_key[TOTEM_ESB_V3_KEY_SIZE] = {
        TOTEM_ESB_V3_RIGHT_KEY_BYTES};
    err = import_link_roots(0, left_key);
    if (err == 0) {
        err = import_link_roots(1, right_key);
    }
#else
    static const uint8_t local_key[TOTEM_ESB_V3_KEY_SIZE] = {
        TOTEM_ESB_V3_LOCAL_KEY_BYTES};
    const uint8_t source = CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID - 1U;
    BUILD_ASSERT(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID >= 1 &&
                     CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID <= TOTEM_ESB_V3_LINK_COUNT,
                 "ESB v3 supports Totem peripheral IDs 1 and 2");
    err = import_link_roots(source, local_key);
#endif
    if (err != 0) {
        LOG_ERR("Unable to import ESB v3 link key (%d)", err);
        return err;
    }

    crypto_initialized = true;
#if IS_ENABLED(CONFIG_TOTEM_ESB_V3_CI_TEST_KEYS)
    LOG_ERR("ESB V3 PUBLIC CI TEST KEY ACTIVE - DO NOT USE FOR SECURITY");
#endif
    return 0;
}

int totem_esb_v3_prepare_pending(uint8_t source, uint64_t peripheral_nonce,
                                 uint64_t central_nonce, uint64_t *session_id) {
    if (!crypto_initialized || source >= ARRAY_SIZE(links) ||
        links[source].root_cmac == 0 || session_id == NULL) {
        return -EINVAL;
    }

    uint64_t new_session = peripheral_nonce ^ central_nonce;
    if (peripheral_nonce == 0 || central_nonce == 0 || new_session == 0) {
        return -EAGAIN;
    }

    uint8_t context[24] = {'Z', 'm', 'K', '3', 's', 'e', 's', 's'};
    sys_put_le64(peripheral_nonce, &context[8]);
    sys_put_le64(central_nonce, &context[16]);
    uint8_t derived[TOTEM_ESB_V3_KEY_SIZE];
    size_t derived_len = 0;
    psa_status_t status = psa_mac_compute(
        links[source].root_cmac, PSA_ALG_CMAC, context, sizeof(context), derived,
        sizeof(derived), &derived_len);
    if (status != PSA_SUCCESS || derived_len != sizeof(derived)) {
        mbedtls_platform_zeroize(derived, sizeof(derived));
        return status == PSA_SUCCESS ? -EIO : status_to_errno(status);
    }

    if (links[source].pending != links[source].active) {
        destroy_key(&links[source].pending);
    } else {
        links[source].pending = 0;
    }
    links[source].pending_session = 0;
    int err = import_aes_key(
        derived, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
        TOTEM_ESB_V3_CCM_ALG, &links[source].pending);
    mbedtls_platform_zeroize(derived, sizeof(derived));
    if (err != 0) {
        return err;
    }

    links[source].pending_session = new_session;
    *session_id = new_session;
    return 0;
}

int totem_esb_v3_activate_pending(uint8_t source) {
    if (source >= ARRAY_SIZE(links) || links[source].pending == 0 ||
        links[source].pending_session == 0) {
        return -ENOENT;
    }
    if (links[source].active != links[source].pending) {
        destroy_key(&links[source].active);
    }
    links[source].active = links[source].pending;
    links[source].active_session = links[source].pending_session;
    return 0;
}

void totem_esb_v3_discard_pending(uint8_t source) {
    if (source < ARRAY_SIZE(links)) {
        if (links[source].pending != links[source].active) {
            destroy_key(&links[source].pending);
        }
        links[source].pending = 0;
        links[source].pending_session = 0;
    }
}

void totem_esb_v3_clear_active(uint8_t source) {
    if (source < ARRAY_SIZE(links)) {
        if (links[source].pending == links[source].active) {
            links[source].pending = 0;
            links[source].pending_session = 0;
        }
        destroy_key(&links[source].active);
        links[source].active_session = 0;
    }
}

uint64_t totem_esb_v3_pending_session(uint8_t source) {
    return source < ARRAY_SIZE(links) ? links[source].pending_session : 0;
}

uint64_t totem_esb_v3_active_session(uint8_t source) {
    return source < ARRAY_SIZE(links) ? links[source].active_session : 0;
}

static psa_key_id_t key_for(uint8_t source,
                            enum totem_esb_v3_key_stage stage,
                            uint64_t nonce_context) {
    if (source >= ARRAY_SIZE(links)) {
        return 0;
    }
    switch (stage) {
    case TOTEM_ESB_V3_ROOT_KEY:
        return links[source].root_ccm;
    case TOTEM_ESB_V3_ACTIVE_KEY:
        return links[source].active_session == nonce_context
                   ? links[source].active
                   : 0;
    case TOTEM_ESB_V3_PENDING_KEY:
        return links[source].pending_session == nonce_context
                   ? links[source].pending
                   : 0;
    default:
        return 0;
    }
}

static void make_nonce(uint8_t source, enum totem_esb_v3_direction direction,
                       enum totem_esb_v3_key_stage stage, uint64_t nonce_context,
                       uint32_t sequence,
                       uint8_t nonce[TOTEM_ESB_V3_NONCE_SIZE]) {
    nonce[0] = TOTEM_ESB_V3_DOMAIN_VERSION | ((source + 1U) & 0x03U);
    if (direction == TOTEM_ESB_V3_DOWNLINK) {
        nonce[0] |= TOTEM_ESB_V3_DOMAIN_DOWNLINK;
    }
    if (stage == TOTEM_ESB_V3_ROOT_KEY) {
        nonce[0] |= TOTEM_ESB_V3_DOMAIN_ROOT;
    }
    sys_put_le64(nonce_context, &nonce[1]);
    sys_put_le32(sequence, &nonce[9]);
}

int totem_esb_v3_seal(uint8_t source, enum totem_esb_v3_direction direction,
                      enum totem_esb_v3_key_stage stage, uint64_t nonce_context,
                      uint32_t sequence, const uint8_t *aad, size_t aad_len,
                      uint8_t *body, size_t body_len,
                      uint8_t tag[TOTEM_ESB_V3_TAG_SIZE]) {
    psa_key_id_t key = key_for(source, stage, nonce_context);
    if (key == 0 || body_len > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
        return -ENOKEY;
    }

    uint8_t nonce[TOTEM_ESB_V3_NONCE_SIZE];
    make_nonce(source, direction, stage, nonce_context, sequence, nonce);
    uint8_t output[CONFIG_ESB_MAX_PAYLOAD_LENGTH + TOTEM_ESB_V3_TAG_SIZE];
    size_t output_len = 0;
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    uint32_t started = k_cycle_get_32();
#endif
    psa_status_t status = psa_aead_encrypt(
        key, TOTEM_ESB_V3_CCM_ALG, nonce, sizeof(nonce), aad, aad_len, body,
        body_len, output, body_len + TOTEM_ESB_V3_TAG_SIZE, &output_len);
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    uint32_t elapsed = k_cycle_get_32() - started;
    totem_esb_benchmark_crypto(source, true, body_len, elapsed, status);
#endif
    if (status != PSA_SUCCESS ||
        output_len != body_len + TOTEM_ESB_V3_TAG_SIZE) {
        mbedtls_platform_zeroize(output, sizeof(output));
        return status == PSA_SUCCESS ? -EIO : status_to_errno(status);
    }

    memcpy(body, output, body_len);
    memcpy(tag, &output[body_len], TOTEM_ESB_V3_TAG_SIZE);
    mbedtls_platform_zeroize(output, sizeof(output));
    return 0;
}

int totem_esb_v3_open(uint8_t source, enum totem_esb_v3_direction direction,
                      enum totem_esb_v3_key_stage stage, uint64_t nonce_context,
                      uint32_t sequence, const uint8_t *aad, size_t aad_len,
                      uint8_t *body, size_t body_len,
                      const uint8_t tag[TOTEM_ESB_V3_TAG_SIZE]) {
    psa_key_id_t key = key_for(source, stage, nonce_context);
    if (key == 0 || body_len > CONFIG_ESB_MAX_PAYLOAD_LENGTH) {
        return -ENOKEY;
    }

    uint8_t nonce[TOTEM_ESB_V3_NONCE_SIZE];
    make_nonce(source, direction, stage, nonce_context, sequence, nonce);
    uint8_t input[CONFIG_ESB_MAX_PAYLOAD_LENGTH + TOTEM_ESB_V3_TAG_SIZE];
    uint8_t output[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
    memcpy(input, body, body_len);
    memcpy(&input[body_len], tag, TOTEM_ESB_V3_TAG_SIZE);
    size_t output_len = 0;
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    uint32_t started = k_cycle_get_32();
#endif
    psa_status_t status = psa_aead_decrypt(
        key, TOTEM_ESB_V3_CCM_ALG, nonce, sizeof(nonce), aad, aad_len, input,
        body_len + TOTEM_ESB_V3_TAG_SIZE, output, body_len, &output_len);
#if IS_ENABLED(CONFIG_TOTEM_ESB_BENCHMARK)
    uint32_t elapsed = k_cycle_get_32() - started;
    totem_esb_benchmark_crypto(source, false, body_len, elapsed, status);
#endif
    if (status != PSA_SUCCESS || output_len != body_len) {
        mbedtls_platform_zeroize(input, sizeof(input));
        mbedtls_platform_zeroize(output, sizeof(output));
        return status == PSA_SUCCESS ? -EIO : status_to_errno(status);
    }

    memcpy(body, output, body_len);
    mbedtls_platform_zeroize(input, sizeof(input));
    mbedtls_platform_zeroize(output, sizeof(output));
    return 0;
}
