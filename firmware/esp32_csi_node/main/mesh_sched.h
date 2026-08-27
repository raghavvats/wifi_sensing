#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install AP BSSID into MAC whitelist and start SCHED + mesh probe TX. */
esp_err_t mesh_sched_start(const uint8_t ap_bssid[6]);

/** True if mac is AP or a known peer STA. */
bool mesh_mac_allowed(const uint8_t mac[6]);

/**
 * True if mac is the transmitter scheduled for the current PROBE slot
 * (Pi BSSID when tx_node=0, else that peer's STA MAC).
 */
bool mesh_mac_is_scheduled_tx(const uint8_t mac[6]);

/** Map transmitter MAC → logical node id (0=Pi/AP, or peer). Returns 255 if unknown. */
uint8_t mesh_tx_node_for_mac(const uint8_t mac[6]);

/** Current probe-phase context for CSI tagging (valid during PROBE). */
bool mesh_probe_active(void);
uint32_t mesh_current_cycle_id(void);
uint8_t mesh_current_slot_idx(void);
uint8_t mesh_current_tx_node(void);
uint8_t mesh_current_rx_target(void);
uint32_t mesh_last_probe_seq(void);
void mesh_note_probe_seq(uint32_t seq);

/** True once this node has already kept a CSI sample for (cycle, slot). */
bool mesh_slot_sample_taken(void);
void mesh_mark_slot_sample_taken(void);

#ifdef __cplusplus
}
#endif
