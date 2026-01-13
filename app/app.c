#include "CANopen.h"
#include "CO_driver_app.h"
#include "CO_driver_storage.h"
#include "OD.h"
#include "can_driver.h"
#include "config_system.h"
#include "crc.h"
#include "flasher_sdo.h"
#include "fw_header.h"
#include "led_drv.h"
#include "lss_cb.h"
#include "platform.h"
#include "prof.h"
#include "ret_mem.h"
#include "slcan.h"
#include "usb_hw.h"
#include "usbd_core_cdc.h"
#include <stdio.h>
#include <string.h>

#define USE_SLCAN

int gsts = -10;

#define SYSTICK_IN_US (168000000 / 1000000)
#define SYSTICK_IN_MS (168000000 / 1000)

#define LED_DECIM 0.01f

#define NMT_CONTROL                  \
	(CO_NMT_STARTUP_TO_OPERATIONAL | \
	 CO_NMT_ERR_ON_ERR_REG |         \
	 CO_NMT_ERR_ON_BUSOFF_HB |       \
	 CO_ERR_REG_GENERIC_ERR |        \
	 CO_ERR_REG_COMMUNICATION)

bool g_stay_in_boot = false;
uint32_t g_uid[3];
CO_t *CO = NULL;
uint16_t frame_cnt_ms;

volatile uint32_t system_time_ms = 0;
static int32_t prev_systick = 0;

uint8_t g_active_can_node_id = CO_LSS_NODE_ID_ASSIGNMENT;		/* Copied from CO_pending_can_node_id in the communication reset section */
static uint8_t pending_can_node_id = CO_LSS_NODE_ID_ASSIGNMENT; /* read from dip switches or nonvolatile memory, configurable by LSS slave */
// uint8_t g_active_can_node_id = 100;		  /* Copied from CO_pending_can_node_id in the communication reset section */
// static uint8_t pending_can_node_id = 100; /* read from dip switches or nonvolatile memory, configurable by LSS slave */
static uint16_t pending_can_baud = 500; /* read from dip switches or nonvolatile memory, configurable by LSS slave */

static float led_amnt[2] = {0};

config_entry_t g_device_config[] = {
	{"can_id", sizeof(pending_can_node_id), 0, &pending_can_node_id},
	{"can_baud", sizeof(pending_can_baud), 0, &pending_can_baud},
	{"hb_prod_ms", sizeof(OD_PERSIST_COMM.x1017_producerHeartbeatTime), 0, &OD_PERSIST_COMM.x1017_producerHeartbeatTime},
};
const uint32_t g_device_config_count = sizeof(g_device_config) / sizeof(g_device_config[0]);

void delay_ms(volatile uint32_t delay_ms)
{
	volatile uint32_t start = 0;
	int32_t mark_prev = 0;
	prof_mark(&mark_prev);
	const uint32_t time_limit = delay_ms * SYSTICK_IN_MS;
	for(;;)
	{
		start += (uint32_t)prof_mark(&mark_prev);
		if(start >= time_limit) return;
	}
}

void main(void)
{
	RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;

	platform_get_uid(g_uid);

	prof_init();
	platform_watchdog_init();
	platform_init();

	fw_header_check_all();

	ret_mem_init();
	ret_mem_set_load_src(LOAD_SRC_APP); // let preboot know it was booted from app

	can_drv_init(CAN1);

	CO = CO_new(NULL, (uint32_t[]){0});
	CO_driver_storage_init(OD_ENTRY_H1010_storeParameters, OD_ENTRY_H1011_restoreDefaultParameters);
	co_od_init_headers();
	flasher_sdo_init();

	usb_init();

	for(;;)
	{
		LSS_cb_obj_t lss_obj = {.lss_br_set_delay_counter = 0, .co = CO};
		CO->CANmodule->CANptr = CAN1;
		CO_NMT_reset_cmd_t reset = CO_RESET_NOT;

		while(reset != CO_RESET_APP)
		{
			CO->CANmodule->CANnormal = false;

			CO_CANsetConfigurationMode(CO->CANmodule->CANptr);
			CO_CANmodule_disable(CO->CANmodule);
			if(CO_CANinit(CO, CO->CANmodule->CANptr, pending_can_baud) != CO_ERROR_NO) return;

			CO_LSS_address_t lssAddress = {.identity = {.vendorID = OD_PERSIST_COMM.x1018_identity.serialNumber,
														.productCode = OD_PERSIST_COMM.x1018_identity.UID0,
														.revisionNumber = OD_PERSIST_COMM.x1018_identity.UID1,
														.serialNumber = OD_PERSIST_COMM.x1018_identity.UID2}};

			if(CO_LSSinit(CO, &lssAddress, &pending_can_node_id, &pending_can_baud) != CO_ERROR_NO) return;
			lss_cb_init(&lss_obj);

			g_active_can_node_id = pending_can_node_id;
			uint32_t errInfo = 0;
			CO_ReturnError_t err = CO_CANopenInit(CO,		   /* CANopen object */
												  NULL,		   /* alternate NMT */
												  NULL,		   /* alternate em */
												  OD,		   /* Object dictionary */
												  NULL,		   /* Optional OD_statusBits */
												  NMT_CONTROL, /* CO_NMT_control_t */
												  500,		   /* firstHBTime_ms */
												  1000,		   /* SDOserverTimeoutTime_ms */
												  500,		   /* SDOclientTimeoutTime_ms */
												  false,	   /* SDOclientBlockTransfer */
												  g_active_can_node_id,
												  &errInfo);

			CO->em->errorStatusBits = OD_RAM.x2000_errorBits;
			if(err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) return;

			err = CO_CANopenInitPDO(CO, CO->em, OD, g_active_can_node_id, &errInfo);
			if(err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) return;

			CO_CANsetNormalMode(CO->CANmodule);
			CO_driver_storage_error_report(CO->em);

			reset = CO_RESET_NOT;

			prof_mark(&prev_systick);

			while(reset == CO_RESET_NOT)
			{
				// time diff
				uint32_t time_diff_systick = (uint32_t)prof_mark(&prev_systick);

				static uint32_t remain_systick_us_prev = 0, remain_systick_ms_prev = 0;
				uint32_t diff_us = (time_diff_systick + remain_systick_us_prev) / (SYSTICK_IN_US);
				remain_systick_us_prev = (time_diff_systick + remain_systick_us_prev) % SYSTICK_IN_US;

				uint32_t diff_ms = (time_diff_systick + remain_systick_ms_prev) / (SYSTICK_IN_MS);
				remain_systick_ms_prev = (time_diff_systick + remain_systick_ms_prev) % SYSTICK_IN_MS;

				platform_watchdog_reset();
				system_time_ms += diff_ms;
				frame_cnt_ms += diff_ms;

				CO_CANinterrupt(CO->CANmodule);
				reset = CO_process(CO, false, diff_us, NULL);
				lss_cb_poll(&lss_obj, diff_us);

				usb_poll(diff_ms);
				led_drv_poll(diff_ms);

				if(!usb_is_configured())
				{
					led_drv_set_led(LED_ERR, LED_MODE_FLASH_2_5HZ);
				}
				else
				{
					led_drv_set_led(LED_ERR, LED_MODE_STROB_05HZ);
				}
				led_drv_set_led_manual(LED_TX, led_amnt[LED_TX]);
				led_drv_set_led_manual(LED_RX, led_amnt[LED_RX]);
				if(diff_ms)
				{
					led_amnt[LED_TX] -= diff_ms * LED_DECIM;
					led_amnt[LED_RX] -= diff_ms * LED_DECIM * .5f;
					if(led_amnt[LED_TX] < 0) led_amnt[LED_TX] = 0;
					if(led_amnt[LED_RX] < 0) led_amnt[LED_RX] = 0;
				}
			}
		}
	}

PLATFORM_RESET:
	CO_CANsetConfigurationMode(CO->CANmodule->CANptr);
	CO_delete(CO);

	platform_reset();
}

void usbd_cdc_rx(const uint8_t *data, uint32_t size)
{
#ifdef USE_SLCAN
	const uint8_t *ret = slcan_parse(CO->CANmodule->CANptr, data, size);
	if(ret) usbd_cdc_push_data(ret, strlen((const char *)ret));
#else
	if(size == sizeof(can_msg_t))
	{
		can_msg_t msg;
		memcpy(&msg, data, size);
		co_drv_send_ex(CAN1, msg.id.std, msg.data, msg.DLC, msg.IDE, msg.RTR);
	}
#endif
}

void can_drv_txed(void)
{
	if(led_amnt[LED_TX] < 0.05f) led_amnt[LED_TX] = 1.0f;
}

void can_drv_rxed(can_msg_t *msg)
{
#ifdef USE_SLCAN
	msg->ts = frame_cnt_ms;
	static uint8_t slcan_buf[64];
	int len = slcan_frame2buf(slcan_buf, msg);
	usbd_cdc_push_data(slcan_buf, len);
#else
	usbd_cdc_push_data((uint8_t *)msg, sizeof(can_msg_t));
#endif
	if(led_amnt[LED_RX] < 0.025f) led_amnt[LED_RX] = 0.5f; // blue led is too strong
}