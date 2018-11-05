#include <stdio.h>
#include <scheduler.h>
#include <net/event.h>
#include <net/swen-l3.h>
#include <net/swen-cmds.h>
#include <adc.h>
#include <sys/array.h>
#include <power-management.h>
#include <watchdog.h>
#include <interrupts.h>
#include <drivers/sensors.h>
#include <eeprom.h>
#include "gpio.h"
#include "../module-common.h"

#define THIS_MODULE_FEATURES MODULE_FEATURE_HUMIDITY |	  \
	MODULE_FEATURE_TEMPERATURE | MODULE_FEATURE_FAN | \
	MODULE_FEATURE_SIREN | MODULE_FEATURE_RF

#define ONE_SECOND 1000000
#define ONE_HOUR (3600 * 1000000U)
#define HUMIDITY_ANALOG_PIN 1
#define FAN_ON_DURATION (4 * 3600) /* max 4h of activity */
#define TEMPERATURE_ANALOG_PIN 3
#define HUMIDITY_SAMPLING 30 /* sample every 30s */
#define GLOBAL_HUMIDITY_ARRAY_LENGTH 30
#define MAX_HUMIDITY_VALUE 80
#define INIT_TIME 60 /* seconds */
#define WD_TIMEOUT 8 /* watchdog timeout set in main.c */

/* inactivity timeout in seconds */
#define INACTIVITY_TIMEOUT 15

static uint8_t rf_addr = RF_GENERIC_MOD_HW_ADDR;
static iface_t rf_iface;
#ifdef RF_DEBUG
iface_t *rf_debug_iface2 = &rf_iface;
#endif

static module_cfg_t module_cfg;
static uint16_t fan_sec_cnt;
static tim_t siren_timer;
static tim_t timer_1sec;
static int global_humidity_array[GLOBAL_HUMIDITY_ARRAY_LENGTH];
static uint8_t prev_tendency;
static uint8_t humidity_sampling_update;
static swen_l3_assoc_t mod1_assoc;
static uint16_t report_hval_elapsed_secs;
static uint8_t init_time;
static uint8_t pwr_state;
static uint8_t pwr_state_report;

static module_cfg_t EEMEM persistent_data;

typedef struct humidity_info {
	int val;
	uint8_t tendency;
} humidity_info_t;

static inline uint8_t get_humidity_cur_value(void)
{
	adc_init_external_64prescaler();
	return HIH_4000_TO_RH(adc_read_mv(ADC_5V_REF_VOLTAGE,
					  HUMIDITY_ANALOG_PIN));
}

static inline int8_t get_temperature_cur_value(void)
{
	adc_init_external_64prescaler();
	return TMP36GZ_TO_C_DEGREES(adc_read_mv(ADC_5V_REF_VOLTAGE,
						TEMPERATURE_ANALOG_PIN));
}

static void humidity_sampling_task_cb(void *arg);

#ifdef CONFIG_POWER_MANAGEMENT
void watchdog_on_wakeup(void *arg)
{
	static uint8_t sampling_cnt;

	DEBUG_LOG("WD interrupt\n");

	/* sample every 30 seconds ((8-sec sleep + 2 secs below) * 3) */
	if (sampling_cnt >= 3) {
		schedule_task(humidity_sampling_task_cb, NULL);
		sampling_cnt = 0;
	} else
		sampling_cnt++;

	if (fan_sec_cnt > WD_TIMEOUT)
		fan_sec_cnt -= WD_TIMEOUT;
	report_hval_elapsed_secs += WD_TIMEOUT;
	if (init_time < INIT_TIME)
		init_time += WD_TIMEOUT;
	/* stay active for 2 seconds in order to catch incoming RF packets */
	power_management_set_inactivity(INACTIVITY_TIMEOUT - 2);
}

static void pwr_mgr_on_sleep(void *arg)
{
	DEBUG_LOG("sleeping...\n");
	gpio_led_off();
	watchdog_enable_interrupt(watchdog_on_wakeup, NULL);
}
#endif

static void report_hum_value(void)
{
	if (module_cfg.humidity_report_interval == 0)
		return;
	if (report_hval_elapsed_secs >=
	    module_cfg.humidity_report_interval) {
		report_hval_elapsed_secs = 0;
		module_add_op(CMD_REPORT_HUM_VAL, 0);
		swen_l3_event_set_mask(&mod1_assoc, EV_READ | EV_WRITE);
	} else
		report_hval_elapsed_secs++;
}

static inline void set_fan_off(void)
{
	gpio_fan_off();
}

static void set_fan_on(void)
{
	gpio_fan_on();
	fan_sec_cnt = FAN_ON_DURATION;
}

static void timer_1sec_cb(void *arg)
{
	timer_reschedule(&timer_1sec, ONE_SECOND);

	if (pwr_state_report) {
		uint8_t cmd = pwr_state ? CMD_NOTIF_MAIN_PWR_UP
			: CMD_NOTIF_MAIN_PWR_DOWN;

		module_add_op(cmd, 1);
		swen_l3_event_set_mask(&mod1_assoc, EV_READ | EV_WRITE);
		pwr_state_report = 0;
	}

	/* skip sampling when the siren is on */
	if (!timer_is_pending(&siren_timer)
	    && humidity_sampling_update++ >= HUMIDITY_SAMPLING)
		schedule_task(humidity_sampling_task_cb, NULL);

	gpio_led_toggle();

#ifdef CONFIG_POWER_MANAGEMENT
	/* do not sleep if the siren is on */
	if (timer_is_pending(&siren_timer))
		power_management_pwr_down_reset();
#endif
	report_hum_value();
	if (fan_sec_cnt) {
		if (fan_sec_cnt == 1)
			set_fan_off();
		fan_sec_cnt--;
	}
	if (init_time < INIT_TIME)
		init_time++;
}

static void set_siren_off(void)
{
	gpio_siren_off();
}

static void siren_tim_cb(void *arg)
{
	gpio_siren_off();
}

static void set_siren_on(uint8_t force)
{
	if (module_cfg.state != MODULE_STATE_ARMED && !force)
		return;

	gpio_siren_on();
	timer_del(&siren_timer);
	timer_add(&siren_timer, module_cfg.siren_duration * 1000000,
		  siren_tim_cb, NULL);
	if (module_cfg.state == MODULE_STATE_ARMED || force) {
		module_add_op(CMD_NOTIF_ALARM_ON, 1);
		swen_l3_event_set_mask(&mod1_assoc, EV_READ | EV_WRITE);
	}
}

#ifndef CONFIG_AVR_SIMU
static void pir_on_action(void *arg)
{
	set_siren_on(0);
}

static void siren_on_tim_cb(void *arg)
{
	schedule_task(pir_on_action, NULL);
}

ISR(PCINT0_vect)
{
	uint8_t pwr_on;

	if (module_cfg.state == MODULE_STATE_DISABLED)
		return;
	pwr_on = gpio_is_main_pwr_on();
	if (pwr_on && !pwr_state)
		pwr_state = 1;
	else if (!pwr_on && pwr_state)
		pwr_state = 0;
	else
		return;
	pwr_state_report = 1;
#ifdef CONFIG_POWER_MANAGEMENT
	power_management_pwr_down_reset();
#endif
}

ISR(PCINT2_vect)
{
	if (module_cfg.state == MODULE_STATE_DISABLED || !gpio_is_pir_on() ||
	    init_time < INIT_TIME || gpio_is_siren_on() ||
	    timer_is_pending(&siren_timer))
		return;

	if (module_cfg.siren_timeout)
		timer_add(&siren_timer, module_cfg.siren_timeout * 1000000,
			  siren_on_tim_cb, NULL);
	else
		schedule_task(pir_on_action, NULL);
#ifdef CONFIG_POWER_MANAGEMENT
	power_management_pwr_down_reset();
#endif
}
#endif

static uint8_t get_hum_tendency(void)
{
	int val;

	if (global_humidity_array[0] == 0)
		return HUMIDITY_TENDENCY_STABLE;

	val = global_humidity_array[GLOBAL_HUMIDITY_ARRAY_LENGTH - 1]
		- global_humidity_array[0];

	if (abs(val) < DEFAULT_HUMIDITY_THRESHOLD)
		return HUMIDITY_TENDENCY_STABLE;
	if (val < 0)
		return HUMIDITY_TENDENCY_FALLING;
	return HUMIDITY_TENDENCY_RISING;
}

static void get_humidity(humidity_info_t *info)
{
	uint8_t val = get_humidity_cur_value();
	uint8_t tendency;

	array_left_shift(global_humidity_array, GLOBAL_HUMIDITY_ARRAY_LENGTH, 1);
	global_humidity_array[GLOBAL_HUMIDITY_ARRAY_LENGTH - 1] = val;
	tendency = get_hum_tendency();
	if (info->tendency != tendency)
		prev_tendency = info->tendency;
	info->tendency = tendency;
}

static void humidity_sampling_task_cb(void *arg)
{
	humidity_info_t info;

	humidity_sampling_update = 0;
	get_humidity(&info);
	if (!module_cfg.fan_enabled || global_humidity_array[0] == 0)
		return;
	if (info.tendency == HUMIDITY_TENDENCY_RISING ||
	    global_humidity_array[GLOBAL_HUMIDITY_ARRAY_LENGTH - 1]
	    >= MAX_HUMIDITY_VALUE) {
		set_fan_on();
	} else if (info.tendency == HUMIDITY_TENDENCY_STABLE)
		set_fan_off();
}

static void get_status(module_status_t *status)
{
	int humidity_array[GLOBAL_HUMIDITY_ARRAY_LENGTH];

	status->humidity.report_interval = module_cfg.humidity_report_interval;
	status->humidity.threshold = module_cfg.humidity_threshold;
	status->humidity.val = get_humidity_cur_value();
	array_copy(humidity_array, global_humidity_array,
		   GLOBAL_HUMIDITY_ARRAY_LENGTH);
	status->humidity.global_val =
		array_get_median(humidity_array, GLOBAL_HUMIDITY_ARRAY_LENGTH);
	status->humidity.tendency = get_hum_tendency();

	status->flags = 0;
	if (gpio_is_fan_on())
		status->flags = STATUS_STATE_FAN_ON;
	if (module_cfg.fan_enabled)
		status->flags |= STATUS_STATE_FAN_ENABLED;
	if (gpio_is_siren_on())
		status->flags |= STATUS_STATE_SIREN_ON;

	status->state = module_cfg.state;
	if (swen_l3_get_state(&mod1_assoc) == S_STATE_CONNECTED)
		status->flags |= STATUS_STATE_CONN_RF_UP;
	status->siren.duration = module_cfg.siren_duration;
	status->siren.timeout = module_cfg.siren_timeout;
	status->temperature = get_temperature_cur_value();
}

static inline void update_storage(void)
{
	eeprom_update(&persistent_data, &module_cfg, sizeof(module_cfg_t));
}

static void load_cfg_from_storage(void)
{
	if (!module_check_magic()) {
		module_set_default_cfg(&module_cfg);
		update_storage();
		module_update_magic();
		return;
	}
	eeprom_load(&module_cfg, &persistent_data, sizeof(module_cfg_t));
}

static void handle_rx_commands(uint8_t cmd, uint16_t value);

#ifdef CONFIG_RF_RECEIVER
#ifdef CONFIG_RF_GENERIC_COMMANDS
static void rf_kerui_cb(int nb)
{
	uint8_t cmd;

	DEBUG_LOG("received kerui cmd %d\n", nb);
	switch (nb) {
	case 0:
		cmd = CMD_ARM;
		break;
	case 1:
		cmd = CMD_DISARM;
		break;
	case 2:
		cmd = CMD_RUN_FAN;
		break;
	case 3:
		cmd = CMD_STOP_FAN;
		break;
	default:
		return;
	}
	handle_rx_commands(cmd, 0);
}
#endif
#endif

static void arm_cb(void *arg)
{
	static uint8_t on;

	if (on == 0) {
		if (gpio_is_siren_on())
			return;
		gpio_siren_on();
		timer_reschedule(&siren_timer, 10000);
		on = 1;
	} else {
		gpio_siren_off();
		on = 0;
	}
}

static void module_arm(uint8_t on)
{
	if (on)
		module_cfg.state = MODULE_STATE_ARMED;
	else {
		module_cfg.state = MODULE_STATE_DISARMED;
		set_siren_off();
	}
	update_storage();
	timer_del(&siren_timer);
	timer_add(&siren_timer, 10000, arm_cb, NULL);
}

static int handle_tx_commands(uint8_t cmd)
{
	module_status_t status;
	void *data = NULL;
	int len = 0;
	uint8_t hum_val;
	uint8_t features;

	switch (cmd) {
	case CMD_STATUS:
		get_status(&status);
		data = &status;
		len = sizeof(module_status_t);
		break;
	case CMD_REPORT_HUM_VAL:
		hum_val = get_humidity_cur_value();
		data = &hum_val;
		len = sizeof(hum_val);
		break;
	case CMD_NOTIF_ALARM_ON:
	case CMD_NOTIF_MAIN_PWR_DOWN:
	case CMD_NOTIF_MAIN_PWR_UP:
		break;
	case CMD_FEATURES:
		cmd = CMD_FEATURES;
		features = THIS_MODULE_FEATURES;
		data = &features;
		len = sizeof(features);
		break;
	default:
		return 0;
	}
	return send_rf_msg(&mod1_assoc, cmd, data, len);
}

static void handle_rx_commands(uint8_t cmd, uint16_t value)
{
	DEBUG_LOG("mod1: got cmd:0x%X\n", cmd);
	switch (cmd) {
	case CMD_GET_FEATURES:
		module_add_op(CMD_FEATURES, 1);
		swen_l3_event_set_mask(&mod1_assoc, EV_READ | EV_WRITE);
		return;
	case CMD_ARM:
		module_arm(1);
		return;
	case CMD_DISARM:
		module_arm(0);
		return;
	case CMD_STATUS:
	case CMD_NOTIF_ALARM_ON:
		/* unsupported */
		return;
	case CMD_RUN_FAN:
		set_fan_on();
		return;
	case CMD_STOP_FAN:
		set_fan_off();
		return;
	case CMD_DISABLE_FAN:
		module_cfg.fan_enabled = 0;
		break;
	case CMD_ENABLE_FAN:
		module_cfg.fan_enabled = 1;
		break;
	case CMD_SIREN_ON:
		set_siren_on(1);
		return;
	case CMD_SIREN_OFF:
		set_siren_off();
		return;
	case CMD_SET_HUM_TH:
		if (value && value <= 100) {
			module_cfg.humidity_threshold = value;
			break;
		}
		return;
	case CMD_SET_SIREN_DURATION:
		if (value) {
			module_cfg.siren_duration = value;
			break;
		}
		return;
	case CMD_SET_SIREN_TIMEOUT:
		module_cfg.siren_timeout = value;
		break;
	case CMD_GET_STATUS:
		module_add_op(CMD_STATUS, 0);
		swen_l3_event_set_mask(&mod1_assoc, EV_READ | EV_WRITE);
		return;
	case CMD_GET_REPORT_HUM_VAL:
		module_cfg.humidity_report_interval = value;
		break;
	}
	update_storage();
}

static void rf_connecting_on_event(event_t *ev, uint8_t events);

static void module1_parse_commands(buf_t *buf)
{
	uint8_t cmd;
	uint8_t v8 = 0;
	uint16_t v16 = 0;

	if (buf_getc(buf, &cmd) < 0)
		return;

	if (buf_get_u16(buf, &v16) < 0)
		if (buf_getc(buf, &v8) >= 0)
			v16 = v8;
	handle_rx_commands(cmd, v16);
}

#ifdef DEBUG
static void module_print_status(void)
{
	int humidity_array[GLOBAL_HUMIDITY_ARRAY_LENGTH];
	uint8_t hval = get_humidity_cur_value();

	array_copy(humidity_array, global_humidity_array,
		   GLOBAL_HUMIDITY_ARRAY_LENGTH);

	LOG("\nStatus:\n");
	LOG(" State:  %s\n",
	    module_cfg.state == MODULE_STATE_ARMED ? "armed" : "disarmed");
	LOG(" Humidity value:  %ld%%\n"
	    " Global humidity value:  %d%%\n"
	    " Humidity tendency:  %u\n"
	    " Humidity threshold:  %u%%\n",
	    hval > 100 ? 0 : hval,
	    array_get_median(humidity_array, GLOBAL_HUMIDITY_ARRAY_LENGTH),
	    get_hum_tendency(), module_cfg.humidity_threshold);
	LOG(" Temperature:  %d\n", get_temperature_cur_value());
	LOG(" Fan: %d\n Fan enabled: %d\n", gpio_is_fan_on(),
	    module_cfg.fan_enabled);
	LOG(" Siren:  %d\n", gpio_is_siren_on()),
	LOG(" Siren duration:  %u secs\n", module_cfg.siren_duration);
	LOG(" RF:  %s\n",
	    swen_l3_get_state(&mod1_assoc) == S_STATE_CONNECTED ? "on" : "off");
}

void module1_parse_uart_commands(buf_t *buf)
{
	sbuf_t s;

	if (buf_get_sbuf_upto_and_skip(buf, &s, "rf buf") >= 0) {
		LOG("\nifce pool: %d rx: %d tx:%d\npkt pool: %d\n",
		    ring_len(rf_iface.pkt_pool), ring_len(rf_iface.rx),
		    ring_len(rf_iface.tx), pkt_pool_get_nb_free());
		return;
	}

	if (buf_get_sbuf_upto_and_skip(buf, &s, "get status") >= 0) {
		module_print_status();
		return;
	}
	if (buf_get_sbuf_upto_and_skip(buf, &s, "disarm") >= 0) {
		module_arm(0);
		return;
	}
	if (buf_get_sbuf_upto_and_skip(buf, &s, "arm") >= 0) {
		module_arm(1);
		return;
	}
	LOG("unsupported command\n");
}
#endif

static void rf_event_cb(event_t *ev, uint8_t events)
{
	swen_l3_assoc_t *assoc = swen_l3_event_get_assoc(ev);
#ifdef DEBUG
	uint8_t id = addr_to_module_id(assoc->dst);
#endif

#ifdef CONFIG_POWER_MANAGEMENT
	power_management_pwr_down_reset();
#endif
	if (events & EV_READ) {
		pkt_t *pkt;

		while ((pkt = swen_l3_get_pkt(assoc)) != NULL) {
			DEBUG_LOG("got pkt of len:%d from mod%d\n",
				  buf_len(&pkt->buf), id);
			module1_parse_commands(&pkt->buf);
			pkt_free(pkt);
		}
	}
	if (events & EV_ERROR) {
		swen_l3_associate(assoc);
		goto error;
	}
	if (events & EV_HUNGUP)
		goto error;

	if (events & EV_WRITE) {
		uint8_t op;

		if (module_get_op(&op) < 0) {
			swen_l3_event_set_mask(assoc, EV_READ);
			return;
		}
		DEBUG_LOG("mod1: sending op:0x%X to mod%d\n", op, id);
		if (handle_tx_commands(op) >= 0) {
			module_skip_op();
			return;
		}
		if (swen_l3_get_state(assoc) != S_STATE_CONNECTED) {
			swen_l3_associate(assoc);
			goto error;
		}
	}
	return;
 error:
	DEBUG_LOG("mod%d disconnected\n", id);
	swen_l3_event_unregister(assoc);
	swen_l3_event_register(assoc, EV_WRITE, rf_connecting_on_event);
}

static void rf_connecting_on_event(event_t *ev, uint8_t events)
{
	swen_l3_assoc_t *assoc = swen_l3_event_get_assoc(ev);
#ifdef DEBUG
	uint8_t id = addr_to_module_id(assoc->dst);
#endif
	uint8_t op;

	if (events & (EV_ERROR | EV_HUNGUP)) {
		DEBUG_LOG("failed to connect to mod%d\n", id);
		swen_l3_event_unregister(assoc);
		swen_l3_associate(assoc);
		swen_l3_event_register(assoc, EV_WRITE, rf_connecting_on_event);
		return;
	}
	if (events & EV_WRITE) {
		uint8_t flags = EV_READ;

		DEBUG_LOG("connected to mod%d\n", id);
		if (module_get_op(&op) >= 0)
			flags |= EV_WRITE;
		swen_l3_event_register(assoc, flags, rf_event_cb);
	}
}

void module1_init(void)
{
	pwr_state = gpio_is_main_pwr_on();

#ifdef CONFIG_RF_CHECKS
	if (rf_checks(&rf_iface) < 0)
		__abort();
#endif
#ifdef CONFIG_RF_GENERIC_COMMANDS
	swen_generic_cmds_init(rf_kerui_cb, rf_ke_cmds);
#endif
	load_cfg_from_storage();

	timer_init(&siren_timer);
	timer_init(&timer_1sec);
	timer_add(&timer_1sec, ONE_SECOND, timer_1sec_cb, NULL);

#ifdef CONFIG_POWER_MANAGEMENT
	power_management_power_down_init(INACTIVITY_TIMEOUT, pwr_mgr_on_sleep,
					 NULL);
#endif
	module_init_iface(&rf_iface, &rf_addr);
	swen_l3_assoc_init(&mod1_assoc, rf_enc_defkey);
	swen_l3_assoc_bind(&mod1_assoc, RF_MASTER_MOD_HW_ADDR, &rf_iface);
	swen_l3_event_register(&mod1_assoc, EV_WRITE, rf_connecting_on_event);

	if (module_cfg.state == MODULE_STATE_DISABLED)
		return;
	if (swen_l3_associate(&mod1_assoc) < 0)
		__abort();
}
