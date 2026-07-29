/*
 * Copyright (c) 2026 asj9005
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

void totem_esb_benchmark_rx(uint8_t source, uint32_t sequence, uint32_t gap,
                            uint32_t source_tick, uint8_t wire_type, uint8_t event_type,
                            uint8_t position, uint8_t pressed);
void totem_esb_benchmark_rx_invalid(uint8_t pipe, int error);
void totem_esb_benchmark_tx(uint16_t message_id, uint16_t attempts, bool success);
void totem_esb_peer_seen(uint8_t source);
uint8_t totem_esb_peer_connected_count(void);
