/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_types.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_pm.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_clk_tree.h"
#include "clk_ctrl_os.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/esp_clk.h"
#include "driver/twai.h"
#include "soc/soc_caps.h"
#include "soc/soc.h"
#include "soc/io_mux_reg.h"
#include "soc/twai_periph.h"
#include "hal/twai_hal.h"
#include "hal/gpio_hal.h"
#include "esp_rom_gpio.h"

/* ---------------------------- Definitions --------------------------------- */
//Internal Macros
#define TWAI_TAG "TWAI"
#define TWAI_CHECK(cond, ret_val) ({                                        \
            if (!(cond)) {                                                  \
                return (ret_val);                                           \
            }                                                               \
})
#define TWAI_CHECK_FROM_CRIT(cond, ret_val) ({                              \
            if (!(cond)) {                                                  \
                TWAI_EXIT_CRITICAL();                                       \
                return ret_val;                                             \
            }                                                               \
})
#define TWAI_SET_FLAG(var, mask)    ((var) |= (mask))
#define TWAI_RESET_FLAG(var, mask)  ((var) &= ~(mask))
#ifdef CONFIG_TWAI_ISR_IN_IRAM
#define TWAI_ISR_ATTR       IRAM_ATTR
#define TWAI_MALLOC_CAPS    (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#define TWAI_ISR_ATTR
#define TWAI_MALLOC_CAPS    MALLOC_CAP_DEFAULT
#endif  //CONFIG_TWAI_ISR_IN_IRAM

#define DRIVER_DEFAULT_INTERRUPTS   0xE7        //Exclude data overrun (bit[3]) and brp_div (bit[4])

#define ALERT_LOG_LEVEL_WARNING     TWAI_ALERT_ARB_LOST  //Alerts above and including this level use ESP_LOGW
#define ALERT_LOG_LEVEL_ERROR       TWAI_ALERT_TX_FAILED //Alerts above and including this level use ESP_LOGE

/* ------------------ Typedefs, structures, and variables ------------------- */

//Control structure for TWAI driver
typedef struct {
    int controller_id;
    periph_module_t module;  // peripheral module
    //Control and status members
    twai_state_t state;
    twai_mode_t mode;
    uint32_t rx_missed_count;
    uint32_t rx_overrun_count;
    uint32_t tx_failed_count;
    uint32_t arb_lost_count;
    uint32_t bus_error_count;
    intr_handle_t isr_handle;
    //TX and RX
#ifdef CONFIG_TWAI_ISR_IN_IRAM
    void *tx_queue_buff;
    void *tx_queue_struct;
    void *rx_queue_buff;
    void *rx_queue_struct;
    void *semphr_struct;
#endif
    QueueHandle_t tx_queue;
    QueueHandle_t rx_queue;
    int tx_msg_count;
    int rx_msg_count;
    //Alerts
    SemaphoreHandle_t alert_semphr;
    uint32_t alerts_enabled;
    uint32_t alerts_triggered;
    //Power Management Lock
    esp_pm_lock_handle_t pm_lock;
} twai_obj_t;

static twai_obj_t *p_twai_obj = NULL;
static portMUX_TYPE twai_spinlock = portMUX_INITIALIZER_UNLOCKED;
#define TWAI_ENTER_CRITICAL_ISR()   portENTER_CRITICAL_ISR(&twai_spinlock)
#define TWAI_EXIT_CRITICAL_ISR()    portEXIT_CRITICAL_ISR(&twai_spinlock)
#define TWAI_ENTER_CRITICAL()       portENTER_CRITICAL(&twai_spinlock)
#define TWAI_EXIT_CRITICAL()        portEXIT_CRITICAL(&twai_spinlock)

static twai_hal_context_t twai_context;

/* -------------------- Interrupt and Alert Handlers ------------------------ */

TWAI_ISR_ATTR static void twai_alert_handler(uint32_t alert_code, int *alert_req)
{
    if (p_twai_obj->alerts_enabled & alert_code) {
        //Signify alert has occurred
        TWAI_SET_FLAG(p_twai_obj->alerts_triggered, alert_code);
        *alert_req = 1;
#ifndef CONFIG_TWAI_ISR_IN_IRAM     //Only log if ISR is not in IRAM
        if (p_twai_obj->alerts_enabled & TWAI_ALERT_AND_LOG) {
            if (alert_code >= ALERT_LOG_LEVEL_ERROR) {
                ESP_EARLY_LOGE(TWAI_TAG, "Alert %d", alert_code);
            } else if (alert_code >= ALERT_LOG_LEVEL_WARNING) {
                ESP_EARLY_LOGW(TWAI_TAG, "Alert %d", alert_code);
            } else {
                ESP_EARLY_LOGI(TWAI_TAG, "Alert %d", alert_code);
            }
        }
#endif  //CONFIG_TWAI_ISR_IN_IRAM
    }
}

TWAI_ISR_ATTR
static inline void twai_handle_rx_buffer_frames(BaseType_t *task_woken, int *alert_req)
{
#ifdef SOC_TWAI_SUPPORTS_RX_STATUS
    uint32_t msg_count = twai_hal_get_rx_msg_count(&twai_context);

    for (uint32_t i = 0; i < msg_count; i++) {
        twai_hal_frame_t frame;
        if (twai_hal_read_rx_buffer_and_clear(&twai_context, &frame)) {
            //Valid frame copied from RX buffer
            if (xQueueSendFromISR(p_twai_obj->rx_queue, &frame, task_woken) == pdTRUE) {
                p_twai_obj->rx_msg_count++;
                twai_alert_handler(TWAI_ALERT_RX_DATA, alert_req);
            } else {    //Failed to send to queue
                p_twai_obj->rx_missed_count++;
                twai_alert_handler(TWAI_ALERT_RX_QUEUE_FULL, alert_req);
            }
        } else {    //Failed to read from RX buffer because message is overrun
            p_twai_obj->rx_overrun_count++;
            twai_alert_handler(TWAI_ALERT_RX_FIFO_OVERRUN, alert_req);
        }
    }
#else   //SOC_TWAI_SUPPORTS_RX_STATUS
    uint32_t msg_count = twai_hal_get_rx_msg_count(&twai_context);
    bool overrun = false;
    //Clear all valid RX frames
    for (int i = 0; i < msg_count; i++) {
        twai_hal_frame_t frame;
        if (twai_hal_read_rx_buffer_and_clear(&twai_context, &frame)) {
            //Valid frame copied from RX buffer
            if (xQueueSendFromISR(p_twai_obj->rx_queue, &frame, task_woken) == pdTRUE) {
                p_twai_obj->rx_msg_count++;
                twai_alert_handler(TWAI_ALERT_RX_DATA, alert_req);
            } else {
                p_twai_obj->rx_missed_count++;
                twai_alert_handler(TWAI_ALERT_RX_QUEUE_FULL, alert_req);
            }
        } else {
            overrun = true;
            break;
        }
    }
    //All remaining frames are treated as overrun. Clear them all
    if (overrun) {
        p_twai_obj->rx_overrun_count += twai_hal_clear_rx_fifo_overrun(&twai_context);
        twai_alert_handler(TWAI_ALERT_RX_FIFO_OVERRUN, alert_req);
    }
#endif  //SOC_TWAI_SUPPORTS_RX_STATUS
}

TWAI_ISR_ATTR
static inline void twai_handle_tx_buffer_frame(BaseType_t *task_woken, int *alert_req)
{
    //Handle previously transmitted frame
    if (twai_hal_check_last_tx_successful(&twai_context)) {
        twai_alert_handler(TWAI_ALERT_TX_SUCCESS, alert_req);
    } else {
        p_twai_obj->tx_failed_count++;
        twai_alert_handler(TWAI_ALERT_TX_FAILED, alert_req);
    }

    //Update TX message count
    p_twai_obj->tx_msg_count--;
    assert(p_twai_obj->tx_msg_count >= 0);      //Sanity check

    //Check if there are more frames to transmit
    if (p_twai_obj->tx_msg_count > 0 && p_twai_obj->tx_queue != NULL) {
        twai_hal_frame_t frame;
        int res = xQueueReceiveFromISR(p_twai_obj->tx_queue, &frame, task_woken);
        if (res == pdTRUE) {
            twai_hal_set_tx_buffer_and_transmit(&twai_context, &frame);
        } else {
            assert(false && "failed to get a frame from TX queue");
        }
    } else {
        //No more frames to transmit
        twai_alert_handler(TWAI_ALERT_TX_IDLE, alert_req);
    }
}

TWAI_ISR_ATTR static void twai_intr_handler_main(void *arg)
{
    BaseType_t task_woken = pdFALSE;
    int alert_req = 0;
    uint32_t events;
    TWAI_ENTER_CRITICAL_ISR();
    if (p_twai_obj == NULL) {    //In case intr occurs whilst driver is being uninstalled
        TWAI_EXIT_CRITICAL_ISR();
        return;
    }
    events = twai_hal_get_events(&twai_context);    //Get the events that triggered the interrupt

#if defined(CONFIG_TWAI_ERRATA_FIX_RX_FRAME_INVALID) || defined(CONFIG_TWAI_ERRATA_FIX_RX_FIFO_CORRUPT)
    if (events & TWAI_HAL_EVENT_NEED_PERIPH_RESET) {
        twai_hal_prepare_for_reset(&twai_context);
        periph_module_reset(p_twai_obj->module);
        twai_hal_recover_from_reset(&twai_context);
        p_twai_obj->rx_missed_count += twai_hal_get_reset_lost_rx_cnt(&twai_context);
        twai_alert_handler(TWAI_ALERT_PERIPH_RESET, &alert_req);
    }
#endif
    if (events & TWAI_HAL_EVENT_RX_BUFF_FRAME) {
        //Note: This event will never occur if there is a periph reset event
        twai_handle_rx_buffer_frames(&task_woken, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_TX_BUFF_FREE) {
        twai_handle_tx_buffer_frame(&task_woken, &alert_req);
    }

    //Handle events that only require alerting (i.e. no handler)
    if (events & TWAI_HAL_EVENT_BUS_OFF) {
        p_twai_obj->state = TWAI_STATE_BUS_OFF;
        twai_alert_handler(TWAI_ALERT_BUS_OFF, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_BUS_RECOV_CPLT) {
        p_twai_obj->state = TWAI_STATE_STOPPED;
        twai_alert_handler(TWAI_ALERT_BUS_RECOVERED, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_BUS_ERR) {
        p_twai_obj->bus_error_count++;
        twai_alert_handler(TWAI_ALERT_BUS_ERROR, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_ARB_LOST) {
        p_twai_obj->arb_lost_count++;
        twai_alert_handler(TWAI_ALERT_ARB_LOST, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_BUS_RECOV_PROGRESS) {
        //Bus-recovery in progress. TEC has dropped below error warning limit
        twai_alert_handler(TWAI_ALERT_RECOVERY_IN_PROGRESS, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_ERROR_PASSIVE) {
        //Entered error passive
        twai_alert_handler(TWAI_ALERT_ERR_PASS, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_ERROR_ACTIVE) {
        //Returned to error active
        twai_alert_handler(TWAI_ALERT_ERR_ACTIVE, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_ABOVE_EWL) {
        //TEC or REC surpassed error warning limit
        twai_alert_handler(TWAI_ALERT_ABOVE_ERR_WARN, &alert_req);
    }
    if (events & TWAI_HAL_EVENT_BELOW_EWL) {
        //TEC and REC are both below error warning
        twai_alert_handler(TWAI_ALERT_BELOW_ERR_WARN, &alert_req);
    }

    TWAI_EXIT_CRITICAL_ISR();

    if (p_twai_obj->alert_semphr != NULL && alert_req) {
        //Give semaphore if alerts were triggered
        xSemaphoreGiveFromISR(p_twai_obj->alert_semphr, &task_woken);
    }
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* -------------------------- Helper functions  ----------------------------- */

static void twai_configure_gpio(gpio_num_t tx, gpio_num_t rx, gpio_num_t clkout, gpio_num_t bus_status)
{
    // assert the GPIO number is not a negative number (shift operation on a negative number is undefined)
    assert(tx >= 0 && rx >= 0);
    int controller_id = p_twai_obj->controller_id;
    gpio_hal_context_t gpio_hal = {
        .dev = GPIO_HAL_GET_HW(GPIO_PORT_0)
    };

    //Set RX pin
    gpio_hal_func_sel(&gpio_hal, rx, PIN_FUNC_GPIO);
    gpio_hal_input_enable(&gpio_hal, rx);
    esp_rom_gpio_connect_in_signal(rx, twai_controller_periph_signals.controllers[controller_id].rx_sig, false);

    //Set TX pin
    gpio_hal_func_sel(&gpio_hal, tx, PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(tx, twai_controller_periph_signals.controllers[controller_id].tx_sig, false, false);

    //Configure output clock pin (Optional)
    if (clkout >= 0 && clkout < GPIO_NUM_MAX) {
        gpio_hal_func_sel(&gpio_hal, clkout, PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(clkout, twai_controller_periph_signals.controllers[controller_id].clk_out_sig, false, false);
    }

    //Configure bus status pin (Optional)
    if (bus_status >= 0 && bus_status < GPIO_NUM_MAX) {
        gpio_hal_func_sel(&gpio_hal, bus_status, PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(bus_status, twai_controller_periph_signals.controllers[controller_id].bus_off_sig, false, false);
    }
}

static void twai_free_driver_obj(twai_obj_t *p_obj)
{
    //Free driver object and any dependent SW resources it uses (queues, semaphores, interrupts, PM locks etc)
#if CONFIG_PM_ENABLE
    if (p_obj->pm_lock != NULL) {
        ESP_ERROR_CHECK(esp_pm_lock_delete(p_obj->pm_lock));
    }
#endif //CONFIG_PM_ENABLE
    if (p_obj->isr_handle) {
        ESP_ERROR_CHECK(esp_intr_free(p_obj->isr_handle));
    }
    //Delete queues and semaphores
    if (p_obj->tx_queue != NULL) {
        vQueueDelete(p_obj->tx_queue);
    }
    if (p_obj->rx_queue != NULL) {
        vQueueDelete(p_obj->rx_queue);
    }
    if (p_obj->alert_semphr != NULL) {
        vSemaphoreDelete(p_obj->alert_semphr);
    }
#ifdef CONFIG_TWAI_ISR_IN_IRAM
    //Free memory used by static queues and semaphores. free() allows freeing NULL pointers
    free(p_obj->tx_queue_buff);
    free(p_obj->tx_queue_struct);
    free(p_obj->rx_queue_buff);
    free(p_obj->rx_queue_struct);
    free(p_obj->semphr_struct);
#endif  //CONFIG_TWAI_ISR_IN_IRAM
    free(p_obj);
}

static esp_err_t twai_alloc_driver_obj(const twai_general_config_t *g_config, twai_clock_source_t clk_src, int controller_id,  twai_obj_t **p_twai_obj_ret)
{
    //Allocate driver object and any dependent SW resources it uses (queues, semaphores, interrupts, PM locks etc)
    esp_err_t ret;

    //Create a TWAI driver object
    twai_obj_t *p_obj = heap_caps_calloc(1, sizeof(twai_obj_t), TWAI_MALLOC_CAPS);
    if (p_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
#ifdef CONFIG_TWAI_ISR_IN_IRAM
    //Allocate memory for queues and semaphores in DRAM
    if (g_config->tx_queue_len > 0) {
        p_obj->tx_queue_buff = heap_caps_calloc(g_config->tx_queue_len, sizeof(twai_hal_frame_t), TWAI_MALLOC_CAPS);
        p_obj->tx_queue_struct = heap_caps_calloc(1, sizeof(StaticQueue_t), TWAI_MALLOC_CAPS);
        if (p_obj->tx_queue_buff == NULL || p_obj->tx_queue_struct == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
    }
    p_obj->rx_queue_buff = heap_caps_calloc(g_config->rx_queue_len, sizeof(twai_hal_frame_t), TWAI_MALLOC_CAPS);
    p_obj->rx_queue_struct = heap_caps_calloc(1, sizeof(StaticQueue_t), TWAI_MALLOC_CAPS);
    p_obj->semphr_struct = heap_caps_calloc(1, sizeof(StaticSemaphore_t), TWAI_MALLOC_CAPS);
    if (p_obj->rx_queue_buff == NULL || p_obj->rx_queue_struct == NULL || p_obj->semphr_struct == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }
    //Create static queues and semaphores
    if (g_config->tx_queue_len > 0) {
        p_obj->tx_queue = xQueueCreateStatic(g_config->tx_queue_len, sizeof(twai_hal_frame_t), p_obj->tx_queue_buff, p_obj->tx_queue_struct);
        if (p_obj->tx_queue == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
    }
    p_obj->rx_queue = xQueueCreateStatic(g_config->rx_queue_len, sizeof(twai_hal_frame_t), p_obj->rx_queue_buff, p_obj->rx_queue_struct);
    p_obj->alert_semphr = xSemaphoreCreateBinaryStatic(p_obj->semphr_struct);
    if (p_obj->rx_queue == NULL || p_obj->alert_semphr == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }
#else   //CONFIG_TWAI_ISR_IN_IRAM
    if (g_config->tx_queue_len > 0) {
        p_obj->tx_queue = xQueueCreate(g_config->tx_queue_len, sizeof(twai_hal_frame_t));
    }
    p_obj->rx_queue = xQueueCreate(g_config->rx_queue_len, sizeof(twai_hal_frame_t));
    p_obj->alert_semphr = xSemaphoreCreateBinary();
    if ((g_config->tx_queue_len > 0 && p_obj->tx_queue == NULL) || p_obj->rx_queue == NULL || p_obj->alert_semphr == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }
#endif  //CONFIG_TWAI_ISR_IN_IRAM
    //Allocate interrupt
    ret = esp_intr_alloc(twai_controller_periph_signals.controllers[controller_id].irq_id,
                         g_config->intr_flags | ESP_INTR_FLAG_INTRDISABLED,
                         twai_intr_handler_main,
                         NULL,
                         &p_obj->isr_handle);
    if (ret != ESP_OK) {
        goto err;
    }
#if CONFIG_PM_ENABLE
#if SOC_TWAI_CLK_SUPPORT_APB
    // DFS can change APB frequency. So add lock to prevent sleep and APB freq from changing
    if (clk_src == TWAI_CLK_SRC_APB) {
        // TODO: pm_lock name should also reflect the controller ID
        ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "twai", &(p_obj->pm_lock));
        if (ret != ESP_OK) {
            goto err;
        }
    }
#else // XTAL
    // XTAL freq can be closed in light sleep, so we need to create a lock to prevent light sleep
    ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "twai", &(p_obj->pm_lock));
    if (ret != ESP_OK) {
        goto err;
    }
#endif //SOC_TWAI_CLK_SUPPORT_APB
#endif //CONFIG_PM_ENABLE

    *p_twai_obj_ret = p_obj;
    return ESP_OK;

err:
    twai_free_driver_obj(p_obj);
    return ret;
}

/* ---------------------------- Public Functions ---------------------------- */

esp_err_t twai_driver_install(const twai_general_config_t *g_config, const twai_timing_config_t *t_config, const twai_filter_config_t *f_config)
{
    //Check arguments
    TWAI_CHECK(g_config != NULL, ESP_ERR_INVALID_ARG);
    TWAI_CHECK(t_config != NULL, ESP_ERR_INVALID_ARG);
    TWAI_CHECK(f_config != NULL, ESP_ERR_INVALID_ARG);
    TWAI_CHECK(g_config->rx_queue_len > 0, ESP_ERR_INVALID_ARG);
    TWAI_CHECK(GPIO_IS_VALID_OUTPUT_GPIO(g_config->tx_io), ESP_ERR_INVALID_ARG);
    TWAI_CHECK(GPIO_IS_VALID_GPIO(g_config->rx_io), ESP_ERR_INVALID_ARG);
#ifndef CONFIG_TWAI_ISR_IN_IRAM
    TWAI_CHECK(!(g_config->intr_flags & ESP_INTR_FLAG_IRAM), ESP_ERR_INVALID_ARG);
#endif
    TWAI_ENTER_CRITICAL();
    TWAI_CHECK_FROM_CRIT(p_twai_obj == NULL, ESP_ERR_INVALID_STATE);
    TWAI_EXIT_CRITICAL();

    //Get clock source resolution
    uint32_t clock_source_hz = 0;
    twai_clock_source_t clk_src = t_config->clk_src;
    //Fall back to default clock source
    if (clk_src == 0) {
        clk_src = TWAI_CLK_SRC_DEFAULT;
    }
    esp_clk_tree_src_get_freq_hz(clk_src, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &clock_source_hz);

    //Check brp validation
    uint32_t brp = t_config->brp;
    if (t_config->quanta_resolution_hz) {
        TWAI_CHECK(clock_source_hz % t_config->quanta_resolution_hz == 0, ESP_ERR_INVALID_ARG);
        brp = clock_source_hz / t_config->quanta_resolution_hz;
    }
    TWAI_CHECK(twai_ll_check_brp_validation(brp), ESP_ERR_INVALID_ARG);

    esp_err_t ret;
    twai_obj_t *p_twai_obj_dummy;
    // TODO: Currently only controller 0 is supported by the driver. IDF-4775
    const int controller_id = 0;

    //Create a TWAI object (including queues, semaphores, interrupts, and PM locks)
    ret = twai_alloc_driver_obj(g_config, clk_src, controller_id, &p_twai_obj_dummy);
    if (ret != ESP_OK) {
        return ret;
    }

    //Initialize flags and variables. All other members are already set to zero by twai_alloc_driver_obj()
    p_twai_obj_dummy->controller_id = controller_id;
    p_twai_obj_dummy->state = TWAI_STATE_STOPPED;
    p_twai_obj_dummy->mode = g_config->mode;
    p_twai_obj_dummy->alerts_enabled = g_config->alerts_enabled;
    p_twai_obj_dummy->module = twai_controller_periph_signals.controllers[controller_id].module;

    //Assign the TWAI object
    TWAI_ENTER_CRITICAL();
    if (p_twai_obj == NULL) {
        p_twai_obj = p_twai_obj_dummy;
    } else {
        //Check if driver is already installed
        TWAI_EXIT_CRITICAL();
        ret = ESP_ERR_INVALID_STATE;
        goto err;
    }
    //Enable TWAI peripheral register clock
    periph_module_reset(p_twai_obj_dummy->module);
    periph_module_enable(p_twai_obj_dummy->module);

    //Initialize TWAI HAL layer
    twai_hal_config_t hal_config = {
        .clock_source_hz = clock_source_hz,
        .controller_id = controller_id,
    };
    bool res = twai_hal_init(&twai_context, &hal_config);
    assert(res);
    twai_hal_configure(&twai_context, t_config, f_config, DRIVER_DEFAULT_INTERRUPTS, g_config->clkout_divider);
    TWAI_EXIT_CRITICAL();

    //Assign GPIO and Interrupts
    twai_configure_gpio(g_config->tx_io, g_config->rx_io, g_config->clkout_io, g_config->bus_off_io);
#if CONFIG_PM_ENABLE
    //Acquire PM lock
    if (p_twai_obj->pm_lock) {
        ESP_ERROR_CHECK(esp_pm_lock_acquire(p_twai_obj->pm_lock));     //Acquire pm_lock during the whole driver lifetime
    }
#endif //CONFIG_PM_ENABLE
    //Enable interrupt
    ESP_ERROR_CHECK(esp_intr_enable(p_twai_obj->isr_handle));

    return ESP_OK;      //TWAI module is still in reset mode, users need to call twai_start() afterwards

err:
    twai_free_driver_obj(p_twai_obj_dummy);
    return ret;
}

esp_err_t twai_driver_uninstall(void)
{
    twai_obj_t *p_twai_obj_dummy;

    TWAI_ENTER_CRITICAL();
    //Check state
    TWAI_CHECK_FROM_CRIT(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK_FROM_CRIT(p_twai_obj->state == TWAI_STATE_STOPPED || p_twai_obj->state == TWAI_STATE_BUS_OFF, ESP_ERR_INVALID_STATE);
    //Clear registers by reading
    twai_hal_deinit(&twai_context);
    periph_module_disable(p_twai_obj->module);   //Disable TWAI peripheral
    p_twai_obj_dummy = p_twai_obj;        //Use dummy to shorten critical section
    p_twai_obj = NULL;
    TWAI_EXIT_CRITICAL();

#if CONFIG_PM_ENABLE
    if (p_twai_obj_dummy->pm_lock) {
        //Release and delete power management lock
        ESP_ERROR_CHECK(esp_pm_lock_release(p_twai_obj_dummy->pm_lock));
    }
#endif //CONFIG_PM_ENABLE
    //Disable interrupt
    ESP_ERROR_CHECK(esp_intr_disable(p_twai_obj_dummy->isr_handle));
    //Free can driver object
    twai_free_driver_obj(p_twai_obj_dummy);
    return ESP_OK;
}

esp_err_t twai_start(void)
{
    //Check state
    TWAI_ENTER_CRITICAL();
    TWAI_CHECK_FROM_CRIT(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK_FROM_CRIT(p_twai_obj->state == TWAI_STATE_STOPPED, ESP_ERR_INVALID_STATE);

    //Reset RX queue, RX message count, amd TX queue
    xQueueReset(p_twai_obj->rx_queue);
    if (p_twai_obj->tx_queue != NULL) {
        xQueueReset(p_twai_obj->tx_queue);
    }
    p_twai_obj->rx_msg_count = 0;
    p_twai_obj->tx_msg_count = 0;
    twai_hal_start(&twai_context, p_twai_obj->mode);

    p_twai_obj->state = TWAI_STATE_RUNNING;
    TWAI_EXIT_CRITICAL();
    return ESP_OK;
}

esp_err_t twai_stop(void)
{
    //Check state
    TWAI_ENTER_CRITICAL();
    TWAI_CHECK_FROM_CRIT(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK_FROM_CRIT(p_twai_obj->state == TWAI_STATE_RUNNING, ESP_ERR_INVALID_STATE);

    twai_hal_stop(&twai_context);

    //Reset TX Queue and message count
    if (p_twai_obj->tx_queue != NULL) {
        xQueueReset(p_twai_obj->tx_queue);
    }
    p_twai_obj->tx_msg_count = 0;
    p_twai_obj->state = TWAI_STATE_STOPPED;

    TWAI_EXIT_CRITICAL();

    return ESP_OK;
}

esp_err_t twai_transmit(const twai_message_t *message, TickType_t ticks_to_wait)
{
    //Check arguments
    TWAI_CHECK(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK(message != NULL, ESP_ERR_INVALID_ARG);
    TWAI_CHECK((message->data_length_code <= TWAI_FRAME_MAX_DLC) || message->dlc_non_comp, ESP_ERR_INVALID_ARG);

    TWAI_ENTER_CRITICAL();
    //Check State
    TWAI_CHECK_FROM_CRIT(!(p_twai_obj->mode == TWAI_MODE_LISTEN_ONLY), ESP_ERR_NOT_SUPPORTED);
    TWAI_CHECK_FROM_CRIT(p_twai_obj->state == TWAI_STATE_RUNNING, ESP_ERR_INVALID_STATE);
    //Format frame
    esp_err_t ret = ESP_FAIL;
    twai_hal_frame_t tx_frame;
    twai_hal_format_frame(message, &tx_frame);

    //Check if frame can be sent immediately
    if (p_twai_obj->tx_msg_count == 0) {
        //No other frames waiting to transmit. Bypass queue and transmit immediately
        twai_hal_set_tx_buffer_and_transmit(&twai_context, &tx_frame);
        p_twai_obj->tx_msg_count++;
        ret = ESP_OK;
    }
    TWAI_EXIT_CRITICAL();

    if (ret != ESP_OK) {
        if (p_twai_obj->tx_queue == NULL) {
            //TX Queue is disabled and TX buffer is occupied, message was not sent
            ret = ESP_FAIL;
        } else if (xQueueSend(p_twai_obj->tx_queue, &tx_frame, ticks_to_wait) == pdTRUE) {
            //Copied to TX Queue
            TWAI_ENTER_CRITICAL();
            if ((!twai_hal_check_state_flags(&twai_context, TWAI_HAL_STATE_FLAG_TX_BUFF_OCCUPIED)) && uxQueueMessagesWaiting(p_twai_obj->tx_queue) > 0) {
                //If the TX buffer is free but the TX queue is not empty. Check if we need to manually start a transmission
                if (twai_hal_check_state_flags(&twai_context, TWAI_HAL_STATE_FLAG_BUS_OFF) || !twai_hal_check_state_flags(&twai_context, TWAI_HAL_STATE_FLAG_RUNNING)) {
                    //TX buffer became free due to bus-off or is no longer running. No need to start a transmission
                    ret = ESP_ERR_INVALID_STATE;
                } else {
                    //Manually start a transmission
                    int res = xQueueReceive(p_twai_obj->tx_queue, &tx_frame, 0);
                    assert(res == pdTRUE);
                    (void)res;
                    twai_hal_set_tx_buffer_and_transmit(&twai_context, &tx_frame);
                    p_twai_obj->tx_msg_count++;
                    ret = ESP_OK;
                }
            } else {
                //Frame was copied to queue, waiting to be transmitted
                p_twai_obj->tx_msg_count++;
                ret = ESP_OK;
            }
            TWAI_EXIT_CRITICAL();
        } else {
            //Timed out waiting for free space on TX queue
            ret = ESP_ERR_TIMEOUT;
        }
    }
    return ret;
}

esp_err_t twai_receive(twai_message_t *message, TickType_t ticks_to_wait)
{
    //Check arguments and state
    TWAI_CHECK(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK(message != NULL, ESP_ERR_INVALID_ARG);

    //Get frame from RX Queue or RX Buffer
    twai_hal_frame_t rx_frame;
    if (xQueueReceive(p_twai_obj->rx_queue, &rx_frame, ticks_to_wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    TWAI_ENTER_CRITICAL();
    p_twai_obj->rx_msg_count--;
    TWAI_EXIT_CRITICAL();

    //Decode frame
    twai_hal_parse_frame(&rx_frame, message);
    return ESP_OK;
}

esp_err_t twai_read_alerts(uint32_t *alerts, TickType_t ticks_to_wait)
{
    //Check arguments and state
    TWAI_CHECK(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK(alerts != NULL, ESP_ERR_INVALID_ARG);

    //Wait for an alert to occur
    if (xSemaphoreTake(p_twai_obj->alert_semphr, ticks_to_wait) == pdTRUE) {
        TWAI_ENTER_CRITICAL();
        *alerts = p_twai_obj->alerts_triggered;
        p_twai_obj->alerts_triggered = 0;    //Clear triggered alerts
        TWAI_EXIT_CRITICAL();
        return ESP_OK;
    } else {
        *alerts = 0;
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t twai_reconfigure_alerts(uint32_t alerts_enabled, uint32_t *current_alerts)
{
    TWAI_CHECK(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);

    TWAI_ENTER_CRITICAL();
    //Clear any unhandled alerts
    if (current_alerts != NULL) {
        *current_alerts = p_twai_obj->alerts_triggered;;
    }
    p_twai_obj->alerts_triggered = 0;
    p_twai_obj->alerts_enabled = alerts_enabled;         //Update enabled alerts
    TWAI_EXIT_CRITICAL();

    return ESP_OK;
}

esp_err_t twai_initiate_recovery(void)
{
    TWAI_ENTER_CRITICAL();
    //Check state
    TWAI_CHECK_FROM_CRIT(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK_FROM_CRIT(p_twai_obj->state == TWAI_STATE_BUS_OFF, ESP_ERR_INVALID_STATE);

    //Reset TX Queue/Counters
    if (p_twai_obj->tx_queue != NULL) {
        xQueueReset(p_twai_obj->tx_queue);
    }
    p_twai_obj->tx_msg_count = 0;

    //Trigger start of recovery process
    twai_hal_start_bus_recovery(&twai_context);
    p_twai_obj->state = TWAI_STATE_RECOVERING;
    TWAI_EXIT_CRITICAL();

    return ESP_OK;
}

esp_err_t twai_get_status_info(twai_status_info_t *status_info)
{
    //Check parameters and state
    TWAI_CHECK(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK(status_info != NULL, ESP_ERR_INVALID_ARG);

    TWAI_ENTER_CRITICAL();
    if (p_twai_obj->mode == TWAI_MODE_LISTEN_ONLY) {
        //Error counters are frozen under listen only mode thus are meaningless. Simply return 0 in this case.
        status_info->tx_error_counter = 0;
        status_info->rx_error_counter = 0;
    } else {
        status_info->tx_error_counter = twai_hal_get_tec(&twai_context);
        status_info->rx_error_counter = twai_hal_get_rec(&twai_context);
    }
    status_info->msgs_to_tx = p_twai_obj->tx_msg_count;
    status_info->msgs_to_rx = p_twai_obj->rx_msg_count;
    status_info->tx_failed_count = p_twai_obj->tx_failed_count;
    status_info->rx_missed_count = p_twai_obj->rx_missed_count;
    status_info->rx_overrun_count = p_twai_obj->rx_overrun_count;
    status_info->arb_lost_count = p_twai_obj->arb_lost_count;
    status_info->bus_error_count = p_twai_obj->bus_error_count;
    status_info->state = p_twai_obj->state;
    TWAI_EXIT_CRITICAL();

    return ESP_OK;
}

esp_err_t twai_clear_transmit_queue(void)
{
    //Check State
    TWAI_CHECK(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);
    TWAI_CHECK(p_twai_obj->tx_queue != NULL, ESP_ERR_NOT_SUPPORTED);

    TWAI_ENTER_CRITICAL();
    //If a message is currently undergoing transmission, the tx interrupt handler will decrement tx_msg_count
    p_twai_obj->tx_msg_count = twai_hal_check_state_flags(&twai_context, TWAI_HAL_STATE_FLAG_TX_BUFF_OCCUPIED) ? 1 : 0;
    xQueueReset(p_twai_obj->tx_queue);
    TWAI_EXIT_CRITICAL();

    return ESP_OK;
}

esp_err_t twai_clear_receive_queue(void)
{
    //Check State
    TWAI_CHECK(p_twai_obj != NULL, ESP_ERR_INVALID_STATE);

    TWAI_ENTER_CRITICAL();
    p_twai_obj->rx_msg_count = 0;
    xQueueReset(p_twai_obj->rx_queue);
    TWAI_EXIT_CRITICAL();

    return ESP_OK;
}
