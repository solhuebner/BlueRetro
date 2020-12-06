/*
 * Copyright (c) 2019-2020, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <esp_timer.h>
#include "../zephyr/types.h"
#include "../util.h"
#include "kb_monitor.h"

struct kb_monitor {
    uint32_t dev_id;
    kbmon_id_to_code_t callback;
    RingbufHandle_t scq_hdl;
    uint32_t keys_state[4];
    uint32_t tm_delay;
    uint32_t tm_rate;
    esp_timer_handle_t tm_timer_hdl;
    uint8_t tm_key_id;
    uint8_t tm_state;
};

static struct kb_monitor kb_monitors[BT_MAX_DEV] = {0};

static void kbmon_typematic_cb(void *arg) {
    struct kb_monitor *kbmon = (struct kb_monitor *)arg;

    kbmon->callback(kbmon->dev_id, KBMON_TYPE_MAKE, kbmon->tm_key_id);
    esp_timer_start_once(kbmon->tm_timer_hdl, kbmon->tm_rate);
}

void kbmon_init(uint8_t dev_id, kbmon_id_to_code_t callback) {
    if (dev_id < BT_MAX_DEV) {
        esp_timer_create_args_t tm_timer_args = {
            .callback = &kbmon_typematic_cb,
            .arg = (void *)&kb_monitors[dev_id],
            .name = "kbmon_typematic_cb"
        };
        esp_timer_create(&tm_timer_args, &kb_monitors[dev_id].tm_timer_hdl);
        kb_monitors[dev_id].tm_state = 0;
        kb_monitors[dev_id].dev_id = dev_id;
        kb_monitors[dev_id].callback = callback;
        kb_monitors[dev_id].scq_hdl = xRingbufferCreate(64, RINGBUF_TYPE_NOSPLIT);
        if (kb_monitors[dev_id].scq_hdl == NULL) {
            printf("# Failed to create KBMON:%d ring buffer\n", dev_id);
        }
    }
}

void kbmon_update(uint8_t dev_id, struct generic_ctrl *ctrl_data) {
    struct kb_monitor *kbmon = &kb_monitors[dev_id];
    uint32_t keys_change[4];

    for (uint32_t i = 0; i < 4; i++) {
        keys_change[i] = ctrl_data->btns[i].value ^ kbmon->keys_state[i];
        kbmon->keys_state[i] = ctrl_data->btns[i].value;
    }

    for (uint32_t i = 0; i < KBM_MAX; i++) {
        if (ctrl_data->map_mask[i / 32] & BIT(i & 0x1F)) {
            if (keys_change[i / 32] & BIT(i & 0x1F)) {
                esp_timer_stop(kbmon->tm_timer_hdl);
                if (ctrl_data->btns[i / 32].value & BIT(i & 0x1F)) {
                    kbmon->callback(dev_id, KBMON_TYPE_MAKE, i);
                    if (kbmon->tm_state) {
                        kbmon->tm_key_id = i;
                        esp_timer_start_once(kbmon->tm_timer_hdl, kbmon->tm_delay);
                    }
                }
                else {
                    kb_monitors[dev_id].callback(dev_id, KBMON_TYPE_BREAK, i);
                }
            }
        }
    }
}

int32_t kbmon_set_code(uint8_t dev_id, uint8_t *code, uint32_t len) {
    struct kb_monitor *kbmon = &kb_monitors[dev_id];
    UBaseType_t ret = pdFALSE;

    if (kbmon->scq_hdl) {
        ret = xRingbufferSend(kbmon->scq_hdl, (void *)code, len, 0);
        if (ret != pdTRUE) {
            printf("# %s KBMON:%d scq full!\n", __FUNCTION__, dev_id);
        }
    }
    return (ret == pdTRUE ? 0 : -1);
}

int32_t kbmon_get_code(uint8_t dev_id, uint8_t *code, uint32_t *len) {
    struct kb_monitor *kbmon = &kb_monitors[dev_id];
    int32_t ret = -1;
    uint32_t qlen;
    if (kbmon->scq_hdl) {
        uint8_t *qcode = (uint8_t *)xRingbufferReceive(kbmon->scq_hdl, &qlen, 0);
        if (qcode) {
            memcpy(code, qcode, qlen);
            vRingbufferReturnItem(kbmon->scq_hdl, (void *)qcode);
            *len = qlen;
            ret = 0;
        }
    }
    return ret;
}

void kbmon_set_typematic(uint8_t dev_id, uint8_t state, uint32_t delay, uint32_t rate) {
    struct kb_monitor *kbmon = &kb_monitors[dev_id];

    kbmon->tm_state = state;
    kbmon->tm_delay = delay;
    kbmon->tm_rate = rate;
}

