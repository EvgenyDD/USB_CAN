#include "platform.h"
#include "fw_header.h"
#include "ret_mem.h"
#include <string.h>

extern void usb_disconnect(void);

typedef void (*pFunction)(void);
uint32_t JumpAddress;
pFunction Jump_To_Application;

typedef struct
{
	uint32_t start;
	uint32_t len;
	uint32_t num;
	bool erased;
} sector_t;

#define CFG_SECT 4

sector_t flash_desc[] = {
	{0x08000000, 16 * 1024, FLASH_Sector_0, false},
	{0x08004000, 16 * 1024, FLASH_Sector_1, false},
	{0x08008000, 16 * 1024, FLASH_Sector_2, false},
	{0x0800C000, 16 * 1024, FLASH_Sector_3, false},
	{0x08010000, 64 * 1024, FLASH_Sector_4, false},
	{0x08020000, 128 * 1024, FLASH_Sector_5, false},
	{0x08040000, 128 * 1024, FLASH_Sector_6, false},
	{0x08060000, 128 * 1024, FLASH_Sector_7, false},
};

static int find_sector(uint32_t addr)
{
	for(uint32_t i = 0; i < sizeof(flash_desc) / sizeof(flash_desc[0]); i++)
	{
		if((addr >= flash_desc[i].start) && (addr < (flash_desc[i].start + flash_desc[i].len)))
		{
			return (int)i;
		}
	}
	return -1;
}

void platform_flash_erase_flag_reset(void)
{
	for(uint32_t i = 0; i < sizeof(flash_desc) / sizeof(flash_desc[0]); i++)
	{
		flash_desc[i].erased = false;
	}
}

void platform_flash_erase_flag_reset_sect_cfg(void)
{
	flash_desc[CFG_SECT].erased = false;
}

void platform_flash_unlock(void) { FLASH_Unlock(); }

void platform_flash_lock(void) { FLASH_Lock(); }

int platform_flash_write(uint32_t dest, const uint8_t *src, uint32_t sz)
{
	if(sz == 0) return 0;

	platform_flash_unlock();
	FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR | FLASH_FLAG_PGAERR | FLASH_FLAG_PGSERR);
	int sts;
	for(uint32_t i = 0; i < sz; i++)
	{
		int sect = find_sector(dest + i);
		if(sect < 0)
		{
			platform_flash_lock();
			return 1;
		}

		if(!flash_desc[sect].erased)
		{
			flash_desc[sect].erased = true;
			sts = FLASH_EraseSector(flash_desc[sect].num, VoltageRange_3);
			if(sts != FLASH_COMPLETE)
			{
				platform_flash_lock();
				return 2;
			}
		}

		sts = FLASH_ProgramByte(dest + i, *(src + i));
		if(sts != FLASH_COMPLETE)
		{
			platform_flash_lock();
			return 4;
		}
		if(*(__IO uint8_t *)(dest + i) != *(src + i))
		{
			platform_flash_lock();
			return 5;
		}
	}
	platform_flash_lock();
	return 0;
}

int platform_flash_read(uint32_t addr, uint8_t *src, uint32_t sz)
{
	if(addr < FLASH_START || addr + sz >= FLASH_FINISH) return 1;
	memcpy(src, (void *)addr, sz);
	return 0;
}

// ========================================================================

__attribute__((noreturn)) void platform_reset(void)
{
#if FW_TYPE == FW_LDR
	ret_mem_set_bl_stuck(false);
#endif
	usb_disconnect();
	platform_deinit();
	NVIC_SystemReset();
}

__attribute__((optimize("-O0"))) __attribute__((always_inline)) static __inline void boot_jump(uint32_t address)
{
	JumpAddress = *(__IO uint32_t *)(address + 4);
	Jump_To_Application = (pFunction)JumpAddress;
	__set_MSP(*(__IO uint32_t *)address);
	Jump_To_Application();
}

__attribute__((optimize("-O0"))) void platform_run_address(uint32_t address)
{
	platform_deinit();
	SCB->VTOR = address;
	boot_jump(address);
}

void platform_get_uid(uint32_t *id)
{
	memcpy(id, (void *)UNIQUE_ID, 3 * sizeof(uint32_t));
}

void platform_watchdog_init(void)
{
	IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
	IWDG_SetPrescaler(IWDG_Prescaler_256);
	IWDG_SetReload(10000 /*time_ms*/ / 8);
	IWDG_Enable();
	IWDG_ReloadCounter();
}

static const char *get_rst_src(void)
{
	if(RCC_GetFlagStatus(RCC_FLAG_LPWRRST)) return "low power";
	if(RCC_GetFlagStatus(RCC_FLAG_WWDGRST)) return "window watchdog";
	if(RCC_GetFlagStatus(RCC_FLAG_IWDGRST)) return "independent watchdog";
	if(RCC_GetFlagStatus(RCC_FLAG_SFTRST)) return "software";
	if(RCC_GetFlagStatus(RCC_FLAG_PORRST)) return "power on power down";
	if(RCC_GetFlagStatus(RCC_FLAG_PINRST)) return "reset pin";
	if(RCC_GetFlagStatus(RCC_FLAG_BORRST)) return "brownout";
	return "unknown";
}

const char *platform_reset_cause_get(void)
{
	static const char *reset_src_str = NULL;
	if(reset_src_str) return reset_src_str;
	reset_src_str = get_rst_src();
	RCC_ClearFlag();
	return reset_src_str;
}

void platform_deinit(void)
{
	__disable_irq();
	SysTick->CTRL = 0;
	for(uint32_t i = 0; i < sizeof(NVIC->ICPR) / sizeof(NVIC->ICPR[0]); i++)
	{
		NVIC->ICPR[i] = 0xfffffffflu;
	}
	__enable_irq();
}

void platform_init(void)
{
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOD, ENABLE);

	GPIO_InitTypeDef GPIO_InitStructure = {0};
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_25MHz;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
}