/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define TOTEM_ESB_V3_KEY_SIZE 16U
#define TOTEM_ESB_V3_TAG_SIZE 4U
#define TOTEM_ESB_V3_NONCE_SIZE 13U

enum totem_esb_v3_direction {
    TOTEM_ESB_V3_UPLINK = 0,
    TOTEM_ESB_V3_DOWNLINK = 1,
};

enum totem_esb_v3_key_stage {
    TOTEM_ESB_V3_ROOT_KEY = 0,
    TOTEM_ESB_V3_ACTIVE_KEY,
    TOTEM_ESB_V3_PENDING_KEY,
};

int totem_esb_v3_crypto_init(void);

int totem_esb_v3_prepare_pending(uint8_t source, uint64_t peripheral_nonce,
                                 uint64_t central_nonce, uint64_t *session_id);
int totem_esb_v3_activate_pending(uint8_t source);
void totem_esb_v3_discard_pending(uint8_t source);
void totem_esb_v3_clear_active(uint8_t source);
uint64_t totem_esb_v3_pending_session(uint8_t source);
uint64_t totem_esb_v3_active_session(uint8_t source);

int totem_esb_v3_seal(uint8_t source, enum totem_esb_v3_direction direction,
                      enum totem_esb_v3_key_stage stage, uint64_t nonce_context,
                      uint32_t sequence, const uint8_t *aad, size_t aad_len,
                      uint8_t *body, size_t body_len,
                      uint8_t tag[TOTEM_ESB_V3_TAG_SIZE]);

int totem_esb_v3_open(uint8_t source, enum totem_esb_v3_direction direction,
                      enum totem_esb_v3_key_stage stage, uint64_t nonce_context,
                      uint32_t sequence, const uint8_t *aad, size_t aad_len,
                      uint8_t *body, size_t body_len,
                      const uint8_t tag[TOTEM_ESB_V3_TAG_SIZE]);
