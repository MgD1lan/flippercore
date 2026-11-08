/**
 * @file furi_hal_nfc_timer.c
 * @brief NFC HAL timers for the ST25R3916 front-end (ESP-IDF port).
 *
 * The upstream implementation drives two STM32 general-purpose timers
 * (TIM1/TIM17) in one-pulse mode and signals expiry from their update
 * interrupts. ESP-IDF has no direct equivalent, so the same two timers are
 * emulated with 1 MHz general-purpose GPTimers: the alarm callback runs in
 * ISR context and only raises the corresponding internal NFC event, exactly
 * like the original update interrupt.
 *
 * Timer semantics preserved from upstream:
 *  - "fwt" — frame wait time timeout used by pollers.
 *  - "block_tx" — transmission block timeout (also used for frame delay).
 *  - both accept a duration in 13.56 MHz carrier cycles;
 *  - per-technology compensation is subtracted before programming the alarm.
 */

#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_tech_i.h"

#include <driver/gptimer.h>
#include <esp_attr.h>

#include <furi_hal_nfc.h>

#define TAG "FuriHalNfcTimer"

#define FURI_HAL_NFC_TIMER_RESOLUTION_HZ (1000000ULL)

typedef enum {
    FuriHalNfcTimerFwt,
    FuriHalNfcTimerBlockTx,
    FuriHalNfcTimerCount,
} FuriHalNfcTimer;

typedef struct {
    gptimer_handle_t timer;
    FuriHalNfcEventInternalType event;
    volatile bool running;
} FuriHalNfcTimerConfig;

static FuriHalNfcTimerConfig furi_hal_nfc_timers[FuriHalNfcTimerCount] = {
    [FuriHalNfcTimerFwt] =
        {
            .timer = NULL,
            .event = FuriHalNfcEventInternalTypeTimerFwtExpired,
            .running = false,
        },
    [FuriHalNfcTimerBlockTx] =
        {
            .timer = NULL,
            .event = FuriHalNfcEventInternalTypeTimerBlockTxExpired,
            .running = false,
        },
};

static bool IRAM_ATTR furi_hal_nfc_timer_irq_callback(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t* edata,
    void* context) {
    UNUSED(timer);
    UNUSED(edata);
    FuriHalNfcTimerConfig* config = context;
    config->running = false;
    furi_hal_nfc_event_set(config->event);
    return false;
}

static void furi_hal_nfc_timer_init(FuriHalNfcTimer timer) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];

    /* low_power_mode_stop() is not guaranteed to run exactly once per
     * power-down (the NFC app reconfigures modes several times), and the
     * ESP32-S3 only has four GPTimers in total. Re-creating a timer that is
     * already live leaked the previous handle and, once the pool ran dry,
     * gptimer_new_timer() failed and left the timer permanently dead — every
     * FWT/block-TX wait after that hung forever. */
    if(config->timer) return;

    const gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = FURI_HAL_NFC_TIMER_RESOLUTION_HZ,
    };
    esp_err_t err = gptimer_new_timer(&timer_config, &config->timer);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
        config->timer = NULL;
        return;
    }

    const gptimer_event_callbacks_t callbacks = {
        .on_alarm = furi_hal_nfc_timer_irq_callback,
    };
    gptimer_register_event_callbacks(config->timer, &callbacks, config);
    gptimer_enable(config->timer);
}

static void furi_hal_nfc_timer_deinit(FuriHalNfcTimer timer) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];
    if(!config->timer) return;

    gptimer_stop(config->timer);
    gptimer_disable(config->timer);
    gptimer_del_timer(config->timer);
    config->timer = NULL;
    config->running = false;
}

static int32_t furi_hal_nfc_timer_get_compensation(FuriHalNfcTimer timer) {
    const FuriHalNfcTechBase* current_tech = furi_hal_nfc_tech[furi_hal_nfc.tech];

    if(furi_hal_nfc.mode == FuriHalNfcModePoller) {
        const FuriHalNfcPollerCompensation* comp = &current_tech->poller.compensation;
        if(timer == FuriHalNfcTimerFwt)
            return comp->fwt;
        else if(timer == FuriHalNfcTimerBlockTx)
            return comp->fdt;

    } else if(furi_hal_nfc.mode == FuriHalNfcModeListener) {
        const FuriHalNfcListenerCompensation* comp = &current_tech->listener.compensation;
        if(timer == FuriHalNfcTimerBlockTx) return comp->fdt;
    }

    return 0;
}

static inline bool furi_hal_nfc_timer_is_running(FuriHalNfcTimer timer) {
    return furi_hal_nfc_timers[timer].running;
}

static void furi_hal_nfc_timer_start_tick(FuriHalNfcTimer timer, uint32_t time_us) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];

    furi_check(!config->running);
    if(!config->timer) return;

    /* 0 would program an alarm in the past; fire on the next tick instead. */
    if(time_us == 0) time_us = 1;

    gptimer_set_raw_count(config->timer, 0);

    const gptimer_alarm_config_t alarm_config = {
        .alarm_count = time_us,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    gptimer_set_alarm_action(config->timer, &alarm_config);

    config->running = true;
    gptimer_start(config->timer);
}

/** Convert carrier cycles to microseconds, rounding to nearest. */
static uint32_t furi_hal_nfc_timer_fc_to_us(uint32_t time_fc) {
    /* FURI_HAL_NFC_CARRIER_HZ == 13 560 000 */
    return (uint32_t)(((uint64_t)time_fc * 1000U + (FURI_HAL_NFC_CARRIER_HZ / 2U)) /
                      FURI_HAL_NFC_CARRIER_HZ);
}

static void furi_hal_nfc_timer_start_fc(FuriHalNfcTimer timer, uint32_t time_fc) {
    const int32_t comp_fc = furi_hal_nfc_timer_get_compensation(timer);
    /* Not starting the timer if the compensation value is greater than the
     * requested delay. */
    if(comp_fc >= (int32_t)time_fc) return;

    furi_hal_nfc_timer_start_tick(timer, furi_hal_nfc_timer_fc_to_us(time_fc - comp_fc));
}

static void furi_hal_nfc_timer_stop(FuriHalNfcTimer timer) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];
    if(!config->timer) return;

    gptimer_stop(config->timer);
    gptimer_set_alarm_action(config->timer, NULL);
    config->running = false;
}

void furi_hal_nfc_timers_init(void) {
    for(size_t i = 0; i < FuriHalNfcTimerCount; i++) {
        furi_hal_nfc_timer_init(i);
    }
}

void furi_hal_nfc_timers_deinit(void) {
    for(size_t i = 0; i < FuriHalNfcTimerCount; i++) {
        furi_hal_nfc_timer_deinit(i);
    }
}

void furi_hal_nfc_timer_fwt_start(uint32_t time_fc) {
    furi_hal_nfc_timer_start_fc(FuriHalNfcTimerFwt, time_fc);
}

void furi_hal_nfc_timer_fwt_stop(void) {
    furi_hal_nfc_timer_stop(FuriHalNfcTimerFwt);
}

void furi_hal_nfc_timer_block_tx_start(uint32_t time_fc) {
    furi_hal_nfc_timer_start_fc(FuriHalNfcTimerBlockTx, time_fc);
}

void furi_hal_nfc_timer_block_tx_start_us(uint32_t time_us) {
    furi_hal_nfc_timer_start_tick(FuriHalNfcTimerBlockTx, time_us);
}

void furi_hal_nfc_timer_block_tx_stop(void) {
    furi_hal_nfc_timer_stop(FuriHalNfcTimerBlockTx);
}

bool furi_hal_nfc_timer_block_tx_is_running(void) {
    return furi_hal_nfc_timer_is_running(FuriHalNfcTimerBlockTx);
}