#include "imxrt.h"
#include "wiring.h"
#include "usb_dev.h"
#include "avr/pgmspace.h"
#include "smalloc.h"
#include <string.h>

#include "debug/printf.h"

#include "startup_usbhost.h"

// from the linker
extern unsigned long _stextload;
extern unsigned long _stext;
extern unsigned long _etext;
extern unsigned long _sdataload;
extern unsigned long _sdata;
extern unsigned long _edata;
extern unsigned long _sbss;
extern unsigned long _ebss;
extern unsigned long _flexram_bank_config;
extern unsigned long _estack;
extern unsigned long _extram_start;
extern unsigned long _extram_end;

__attribute__ ((used, aligned(1024), section(".vectorsram")))
void (* volatile _VectorsRam[NVIC_NUM_INTERRUPTS+16])(void);

static void memory_copy(uint32_t *dest, const uint32_t *src, uint32_t *dest_end);
static void memory_clear(uint32_t *dest, uint32_t *dest_end);
static void configure_systick(void);
static void reset_PFD();
extern void systick_isr(void);
extern void pendablesrvreq_isr(void);
void configure_cache(void);
void configure_external_ram(void);
void unused_interrupt_vector(void);
void usb_pll_start();
extern void analog_init(void); // analog.c
extern void pwm_init(void); // pwm.c
extern void tempmon_init(void);  //tempmon.c
extern float tempmonGetTemp(void);
extern unsigned long rtc_get(void);
uint32_t set_arm_clock(uint32_t frequency); // clockspeed.c
extern void __libc_init_array(void); // C++ standard library

uint8_t external_psram_size = 0;
#ifdef ARDUINO_TEENSY41
struct smalloc_pool extmem_smalloc_pool;
#endif

extern int main (void); // Forward declaration for main

// --- Standard Teensy Hooks (ensure these are defined only once) ---
FLASHMEM void startup_default_early_hook(void) {}
void startup_early_hook(void)	__attribute__ ((weak, alias("startup_default_early_hook")));

FLASHMEM void startup_default_middle_hook(void) {}
void startup_middle_hook(void)	__attribute__ ((weak, alias("startup_default_middle_hook")));

// --- Late Hook setup MODIFIED ---
// This C function will be the actual startup_late_hook.
// It will call the extern "C" function from startup_usbhost.cpp.
void actual_startup_late_hook_c_implementation(void) {
    printf("C: actual_startup_late_hook_c_implementation() calling C++ hook...\n");
    int result = startup_host_init_and_spoof(); // <<< Direct call to the C-linkage function
    if (result != 0) {
        printf("C: startup_host_init_and_spoof() returned error %d\n", result);
    }
    printf("C: Returned from C++ hook startup_host_init_and_spoof().\n");
}

// The default empty hook (if no alias is used or if aliasing fails)
FLASHMEM void startup_default_late_hook(void) {}
// Alias startup_late_hook to our C wrapper function
void startup_late_hook(void) __attribute__ ((weak, alias("actual_startup_late_hook_c_implementation")));

extern void startup_debug_reset(void) __attribute__((noinline));
FLASHMEM void startup_debug_reset(void) { __asm__ volatile("nop"); }

static void ResetHandler2(void);

__attribute__((section(".startup"), naked))
void ResetHandler(void)
{
	IOMUXC_GPR_GPR17 = (uint32_t)&_flexram_bank_config;
	IOMUXC_GPR_GPR16 = 0x00200007;
	IOMUXC_GPR_GPR14 = 0x00AA0000;
	__asm__ volatile("mov sp, %0" : : "r" ((uint32_t)&_estack) : "memory");
#if 0
	__asm__ volatile("dsb":::"memory");
	__asm__ volatile("isb":::"memory");
#endif
	ResetHandler2();
	__builtin_unreachable();
}

// <<< START: Corrected usb_host_phy_pll_start with required defines >>>

// Define bit masks/shifts ONLY IF they are missing from your specific imxrt.h
// It's safer to assume they ARE in imxrt.h and remove these defines if you get errors.
#ifndef CCM_CBCMR_USBOH3_CLK_SEL_MASK
#define CCM_CBCMR_USBOH3_CLK_SEL_BIT (19)
#define CCM_CBCMR_USBOH3_CLK_SEL_MASK (1U << CCM_CBCMR_USBOH3_CLK_SEL_BIT)
#endif
#ifndef CCM_CBCDR_USBOH3_PODF_MASK
#define CCM_CBCDR_USBOH3_PODF_SHIFT (10)
#define CCM_CBCDR_USBOH3_PODF_MASK (0x7U << CCM_CBCDR_USBOH3_PODF_SHIFT)
#endif
// Correct CCM_CCGR6 define name as per imxrt.h (use the existing one)
// #ifndef CCM_CCGR6_USB_OTG2_CLK_ENABLE
// #define CCM_CCGR6_USB_OTG2_CLK_ENABLE(n) CCM_CCGR6_USBOH3(n) // Don't redefine if CCM_CCGR6_USBOH3 exists
// #endif
// GPC defines (ensure these match your imxrt.h or SDK)
#ifndef PGC_MEGA_CTRL
#define PGC_MEGA_CTRL   (*(volatile uint32_t *)(IMXRT_GPC_ADDRESS + 0x220))
#define PGC_MEGA_CTRL_PCR (1U << 0) // Power Control Request/Status bit
#endif
#ifndef PGC_MEGA_SR
#define PGC_MEGA_SR     (*(volatile uint32_t *)(IMXRT_GPC_ADDRESS + 0x22C))
#define PGC_MEGA_SR_PSR   (1U << 0) // Power Status bit (1=Not Ready, 0=Ready)
#endif


FLASHMEM static void usb_host_phy_pll_start() {
    printf(">>> usb_host_phy_pll_start() called [Final Working Sequence]\n");
    uint32_t wait_count = 0;
    uint32_t wait_limit = 2000000;

    // --- Stage 0: GPC Power Up ---
    printf("  --- Stage 0: Checking/Requesting GPC MEGA Power ---\n");
    if (!(PGC_MEGA_CTRL & PGC_MEGA_CTRL_PCR) || (PGC_MEGA_SR & PGC_MEGA_SR_PSR)) {
         if (!(PGC_MEGA_CTRL & PGC_MEGA_CTRL_PCR)) {
              printf("  Requesting power up (PCR=1)...\n"); PGC_MEGA_CTRL |= PGC_MEGA_CTRL_PCR;
         } else {
              printf("  WARN: PCR=1, but PSR indicates not ready! Waiting...\n");
         }
         printf("  Waiting for MEGA domain power up (PSR=0)...\n"); wait_count = 0; while (PGC_MEGA_SR & PGC_MEGA_SR_PSR) { if (++wait_count > wait_limit) { printf("  ERROR: Timeout GPC MEGA power up/ready!\n"); goto host_phy_pll_fail_local; } }
         printf("  SUCCESS: GPC MEGA domain ready. CTRL:0x%08lX, SR:0x%08lX\n", PGC_MEGA_CTRL, PGC_MEGA_SR);
    } else {
         printf("  GPC MEGA domain already ready (PCR=1, PSR=0).\n");
    }
    printf("  --- End Stage 0 ---\n\n");

    // --- Stage 1: Configure USBOH3 Clock ---
    printf("  --- Stage 1: Configuring USBOH3 Clock ---\n");
    uint32_t initial_cbcdr = CCM_CBCDR; uint32_t target_podf_val = 7; uint32_t cbcdr_current_podf_val = (initial_cbcdr & CCM_CBCDR_USBOH3_PODF_MASK) >> CCM_CBCDR_USBOH3_PODF_SHIFT; if (cbcdr_current_podf_val != target_podf_val) { CCM_CBCDR = (initial_cbcdr & ~CCM_CBCDR_USBOH3_PODF_MASK) | (target_podf_val << CCM_CBCDR_USBOH3_PODF_SHIFT); for (volatile int i = 0; i < 50; i++); }
    // CCM_CBCMR bit 19 (PLL3 select) wasn't reliably setting, so we accept the default source and just set divider.
    printf("  Final CCM_CBCDR: 0x%08lX\n", CCM_CBCDR);
    printf("  --- End Stage 1 ---\n\n");

    // --- Stage 2: Enable Controller Clock Gate ---
    printf("  --- Stage 2: Enable Controller Clock Gate ---\n");
    CCM_CCGR6 |= CCM_CCGR6_USBOH3(CCM_CCGR_ON); // Use correct define
    printf("  USB_OTG2 clock gate enabled (CCM_CCGR6: 0x%08lX)\n", CCM_CCGR6);
    printf("  --- End Stage 2 ---\n\n");

    // --- Stage 3: Configure USBPHY2 Control ---
    printf("  --- Stage 3: Configure USBPHY2_CTRL ---\n");
    USBPHY2_CTRL_CLR = USBPHY_CTRL_SFTRST | USBPHY_CTRL_CLKGATE; int phy_sftrst_wait = 0; while(USBPHY2_CTRL & USBPHY_CTRL_SFTRST) { if (++phy_sftrst_wait > 10000) break; } USBPHY2_CTRL_SET = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3; // Use base settings
    printf("  USBPHY2_CTRL after setup: 0x%08lX\n", USBPHY2_CTRL);
    printf("  --- End Stage 3 ---\n\n");

    // --- Stage 4: Power Up USBPHY2 via PWD ---
    printf("  --- Stage 4: USBPHY2_PWD Power Up ---\n"); USBPHY2_PWD = 0; for (volatile int i = 0; i < 500; i++); volatile uint32_t pwd_readback = USBPHY2_PWD; printf("  USBPHY2_PWD after write 0: 0x%08lX\n", pwd_readback); if (pwd_readback != 0) { printf("  ERROR: USBPHY2_PWD did not clear. Halting.\n"); goto host_phy_pll_fail_local; } printf("  SUCCESS: USBPHY2_PWD cleared.\n"); printf("  --- End Stage 4 ---\n\n");

    // --- Stage 5: Configure and Enable USB2_PLL ---
    printf("  --- Stage 5: Configure and Enable USB2_PLL ---\n"); if (!(CCM_ANALOG_PLL_USB2 & CCM_ANALOG_PLL_USB2_POWER)) { CCM_ANALOG_PLL_USB2_SET = CCM_ANALOG_PLL_USB2_POWER; for (volatile int i = 0; i < 1000; i++); } if (!(CCM_ANALOG_PLL_USB2 & CCM_ANALOG_PLL_USB2_ENABLE)) { CCM_ANALOG_PLL_USB2_SET = CCM_ANALOG_PLL_USB2_ENABLE; for (volatile int i = 0; i < 100; i++); } wait_count = 0; while (!(CCM_ANALOG_PLL_USB2 & CCM_ANALOG_PLL_USB2_LOCK)) { if (++wait_count > wait_limit) { printf("  ERROR: Timeout PLL_USB2 lock!\n"); goto host_phy_pll_fail_local; } } if (CCM_ANALOG_PLL_USB2 & CCM_ANALOG_PLL_USB2_BYPASS) { CCM_ANALOG_PLL_USB2_CLR = CCM_ANALOG_PLL_USB2_BYPASS; for (volatile int i = 0; i < 100; i++); } if (!(CCM_ANALOG_PLL_USB2 & CCM_ANALOG_PLL_USB2_EN_USB_CLKS)) { CCM_ANALOG_PLL_USB2_SET = CCM_ANALOG_PLL_USB2_EN_USB_CLKS; for (volatile int i = 0; i < 100; i++); }
    printf("  PLL_USB2 configured. Final CCM_ANALOG_PLL_USB2: 0x%08lX\n", CCM_ANALOG_PLL_USB2);
    printf("  --- End Stage 5 ---\n\n");

    // --- Stage 6: USB Controller Reset (Blind Write First) ---
    printf("  --- Stage 6: USB Controller Init (Reset -> Mode) ---\n");
    printf("  Asserting USB_USBCMD_RST..."); USB2_USBCMD |= USB_USBCMD_RST; for(volatile int d=0; d<10; d++) { asm volatile ("nop"); } printf(" Write attempted.\n");
    printf("  Waiting for controller reset complete (RST bit to clear)...\n"); wait_count = 0; while (USB2_USBCMD & USB_USBCMD_RST) { if (++wait_count > wait_limit) { printf("  ERROR: Timeout waiting USB_OTG2 reset clear! USBCMD: 0x%08lX\n", USB2_USBCMD); goto host_phy_pll_fail_local; } }
    printf("  USB_OTG2 controller reset complete. USBCMD: 0x%08lX\n", USB2_USBCMD);
    printf("  --- End Stage 6 ---\n\n");

    // --- Stage 7: Set USBMODE to Host ---
    printf("  --- Stage 7: Set USBMODE to Host ---\n"); volatile uint32_t current_usbmode = USB2_USBMODE; if ((current_usbmode & USB_USBMODE_CM(3)) != USB_USBMODE_CM(3)) { USB2_USBMODE = (current_usbmode & ~USB_USBMODE_CM(3)) | USB_USBMODE_CM(3); for(volatile int d=0; d<100; d++); }
    printf("  Final USB2_USBMODE: 0x%08lX\n", USB2_USBMODE);
    printf("  --- End Stage 7 ---\n\n");

    // --- Stage 8: Final Stop Controller ---
    printf("  --- Stage 8: Final Stop Controller ---\n"); USB2_USBCMD &= ~USB_USBCMD_RS;
    printf("  Final USB2_USBCMD after RS clear: 0x%08lX\n", USB2_USBCMD);
    printf("  --- End Stage 8 ---\n\n");

    printf("  usb_host_phy_pll_start: Hardware setup appears complete.\n");
    goto host_phy_pll_success_local; // Use goto for success path

host_phy_pll_fail_local: // Local label for this function's failure path
    printf("  usb_host_phy_pll_start: FAILED.\n");

host_phy_pll_success_local: // Label for success path
    printf("<<< usb_host_phy_pll_start() finished.\n");
}
// <<< END: Full usb_host_phy_pll_start function >>>

__attribute__((section(".startup"), noinline, noreturn))
static void ResetHandler2(void)
{
	unsigned int i;
	__asm__ volatile("dsb":::"memory");
	asm volatile("nop"); asm volatile("nop"); asm volatile("nop"); asm volatile("nop");
	startup_early_hook();
	PMU_MISC0_SET = 1<<3;
	for(volatile int k=0; k<16; k++) asm volatile("nop");

	memory_copy(&_stext, &_stextload, &_etext);
	memory_copy(&_sdata, &_sdataload, &_edata);
	memory_clear(&_sbss, &_ebss);

	SCB_CPACR = 0x00F00000;
	for (i=0; i < NVIC_NUM_INTERRUPTS + 16; i++) _VectorsRam[i] = &unused_interrupt_vector;
	for (i=0; i < NVIC_NUM_INTERRUPTS; i++) NVIC_SET_PRIORITY(i, 128);
	SCB_VTOR = (uint32_t)_VectorsRam;
	reset_PFD();
	SCB_SHCSR |= SCB_SHCSR_MEMFAULTENA | SCB_SHCSR_BUSFAULTENA | SCB_SHCSR_USGFAULTENA;
	CCM_CSCMR1 = (CCM_CSCMR1 & ~CCM_CSCMR1_PERCLK_PODF(0x3F)) | CCM_CSCMR1_PERCLK_CLK_SEL;
	CCM_CSCDR1 = (CCM_CSCDR1 & ~CCM_CSCDR1_UART_CLK_PODF(0x3F)) | CCM_CSCDR1_UART_CLK_SEL;
#if defined(__IMXRT1062__)
	IOMUXC_GPR_GPR26 = 0xFFFFFFFF; IOMUXC_GPR_GPR27 = 0xFFFFFFFF;
	IOMUXC_GPR_GPR28 = 0xFFFFFFFF; IOMUXC_GPR_GPR29 = 0xFFFFFFFF;
#endif
	printf_debug_init();
	printf("\n***********IMXRT Startup (Serial4 Logs)**********\n");
	printf("Test message from startup.c: %d %d %d\n", 1, -1234567, 3);

	configure_cache();
	configure_systick();

	usb_pll_start();
	usb_host_phy_pll_start(); // Your working version

	reset_PFD();
#ifdef F_CPU
	set_arm_clock(F_CPU);
#endif
	CCM_CCGR1 |= CCM_CCGR1_PIT(CCM_CCGR_ON); PIT_MCR = 0;
	PIT_TCTRL0 = 0; PIT_TCTRL1 = 0; PIT_TCTRL2 = 0; PIT_TCTRL3 = 0;
	if (!(SNVS_LPCR & SNVS_LPCR_SRTC_ENV)) { SNVS_LPSRTCLR = 1546300800u << 15; SNVS_LPSRTCMR = 1546300800u >> 17; SNVS_LPCR |= SNVS_LPCR_SRTC_ENV; }
	SNVS_HPCR |= SNVS_HPCR_RTC_EN | SNVS_HPCR_HP_TS;
#ifdef ARDUINO_TEENSY41
	configure_external_ram();
#endif
	analog_init(); pwm_init(); tempmon_init();
	startup_middle_hook();

    printf("Before USB device init delays (Log to Serial4)\n");
	#if !defined(TEENSY_INIT_USB_DELAY_BEFORE)
        #define TEENSY_INIT_USB_DELAY_BEFORE 20
	#endif
	#if !defined(TEENSY_INIT_USB_DELAY_AFTER)
        #define TEENSY_INIT_USB_DELAY_AFTER 280
	#endif
	while (millis() < TEENSY_INIT_USB_DELAY_BEFORE) ;
	while (millis() < TEENSY_INIT_USB_DELAY_AFTER + TEENSY_INIT_USB_DELAY_BEFORE) ;

    printf("Before C++ ctors (Log to Serial4)\n");
	startup_debug_reset();

	__libc_init_array(); // C++ CONSTRUCTORS RUN HERE
    printf("After C++ ctors (Log to Serial4)\n");

    printf("Calling startup_late_hook() (aliased to C++ startup_host_init_and_spoof)...\n");
	startup_late_hook(); // This will execute the C++ function
	printf("Returned from startup_late_hook().\n");

	usb_init(); // Initialize USB Device stack (standard)
    printf("After usb_init() (Log to Serial4)\n");

	main();
	while (1) asm("WFI");
}

// ARM SysTick is used for most Ardiuno timing functions, delay(), millis(),
// micros().  SysTick can run from either the ARM core clock, or from an
// "external" clock.  NXP documents it as "24 MHz XTALOSC can be the external
// clock source of SYSTICK" (RT1052 ref manual, rev 1, page 411).  However,
// NXP actually hid an undocumented divide-by-240 circuit in the hardware, so
// the external clock is really 100 kHz.  We use this clock rather than the
// ARM clock, to allow SysTick to maintain correct timing even when we change
// the ARM clock to run at different speeds.
#define SYSTICK_EXT_FREQ 100000

extern volatile uint32_t systick_cycle_count;
static void configure_systick(void)
{
	_VectorsRam[14] = pendablesrvreq_isr;
	_VectorsRam[15] = systick_isr;
	SYST_RVR = (SYSTICK_EXT_FREQ / 1000) - 1;
	SYST_CVR = 0;
	SYST_CSR = SYST_CSR_TICKINT | SYST_CSR_ENABLE;
	SCB_SHPR3 = 0x20200000;  // Systick, pendablesrvreq_isr = priority 32;
	ARM_DEMCR |= ARM_DEMCR_TRCENA;
	ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA; // turn on cycle counter
	systick_cycle_count = ARM_DWT_CYCCNT; // compiled 0, corrected w/1st systick
}


// concise defines for SCB_MPU_RASR and SCB_MPU_RBAR, ARM DDI0403E, pg 696
#define NOEXEC		SCB_MPU_RASR_XN
#define READONLY	SCB_MPU_RASR_AP(7)
#define READWRITE	SCB_MPU_RASR_AP(3)
#define NOACCESS	SCB_MPU_RASR_AP(0)
#define MEM_CACHE_WT	SCB_MPU_RASR_TEX(0) | SCB_MPU_RASR_C
#define MEM_CACHE_WB	SCB_MPU_RASR_TEX(0) | SCB_MPU_RASR_C | SCB_MPU_RASR_B
#define MEM_CACHE_WBWA	SCB_MPU_RASR_TEX(1) | SCB_MPU_RASR_C | SCB_MPU_RASR_B
#define MEM_NOCACHE	SCB_MPU_RASR_TEX(1)
#define DEV_NOCACHE	SCB_MPU_RASR_TEX(2)
#define SIZE_32B	(SCB_MPU_RASR_SIZE(4) | SCB_MPU_RASR_ENABLE)
#define SIZE_64B	(SCB_MPU_RASR_SIZE(5) | SCB_MPU_RASR_ENABLE)
#define SIZE_128B	(SCB_MPU_RASR_SIZE(6) | SCB_MPU_RASR_ENABLE)
#define SIZE_256B	(SCB_MPU_RASR_SIZE(7) | SCB_MPU_RASR_ENABLE)
#define SIZE_512B	(SCB_MPU_RASR_SIZE(8) | SCB_MPU_RASR_ENABLE)
#define SIZE_1K		(SCB_MPU_RASR_SIZE(9) | SCB_MPU_RASR_ENABLE)
#define SIZE_2K		(SCB_MPU_RASR_SIZE(10) | SCB_MPU_RASR_ENABLE)
#define SIZE_4K		(SCB_MPU_RASR_SIZE(11) | SCB_MPU_RASR_ENABLE)
#define SIZE_8K		(SCB_MPU_RASR_SIZE(12) | SCB_MPU_RASR_ENABLE)
#define SIZE_16K	(SCB_MPU_RASR_SIZE(13) | SCB_MPU_RASR_ENABLE)
#define SIZE_32K	(SCB_MPU_RASR_SIZE(14) | SCB_MPU_RASR_ENABLE)
#define SIZE_64K	(SCB_MPU_RASR_SIZE(15) | SCB_MPU_RASR_ENABLE)
#define SIZE_128K	(SCB_MPU_RASR_SIZE(16) | SCB_MPU_RASR_ENABLE)
#define SIZE_256K	(SCB_MPU_RASR_SIZE(17) | SCB_MPU_RASR_ENABLE)
#define SIZE_512K	(SCB_MPU_RASR_SIZE(18) | SCB_MPU_RASR_ENABLE)
#define SIZE_1M		(SCB_MPU_RASR_SIZE(19) | SCB_MPU_RASR_ENABLE)
#define SIZE_2M		(SCB_MPU_RASR_SIZE(20) | SCB_MPU_RASR_ENABLE)
#define SIZE_4M		(SCB_MPU_RASR_SIZE(21) | SCB_MPU_RASR_ENABLE)
#define SIZE_8M		(SCB_MPU_RASR_SIZE(22) | SCB_MPU_RASR_ENABLE)
#define SIZE_16M	(SCB_MPU_RASR_SIZE(23) | SCB_MPU_RASR_ENABLE)
#define SIZE_32M	(SCB_MPU_RASR_SIZE(24) | SCB_MPU_RASR_ENABLE)
#define SIZE_64M	(SCB_MPU_RASR_SIZE(25) | SCB_MPU_RASR_ENABLE)
#define SIZE_128M	(SCB_MPU_RASR_SIZE(26) | SCB_MPU_RASR_ENABLE)
#define SIZE_256M	(SCB_MPU_RASR_SIZE(27) | SCB_MPU_RASR_ENABLE)
#define SIZE_512M	(SCB_MPU_RASR_SIZE(28) | SCB_MPU_RASR_ENABLE)
#define SIZE_1G		(SCB_MPU_RASR_SIZE(29) | SCB_MPU_RASR_ENABLE)
#define SIZE_2G		(SCB_MPU_RASR_SIZE(30) | SCB_MPU_RASR_ENABLE)
#define SIZE_4G		(SCB_MPU_RASR_SIZE(31) | SCB_MPU_RASR_ENABLE)
#define REGION(n)	(SCB_MPU_RBAR_REGION(n) | SCB_MPU_RBAR_VALID)

FLASHMEM void configure_cache(void)
{
	//printf("MPU_TYPE = %08lX\n", SCB_MPU_TYPE);
	//printf("CCR = %08lX\n", SCB_CCR);

	// TODO: check if caches already active - skip?

	SCB_MPU_CTRL = 0; // turn off MPU

	uint32_t i = 0;
	SCB_MPU_RBAR = 0x00000000 | REGION(i++); //https://developer.arm.com/docs/146793866/10/why-does-the-cortex-m7-initiate-axim-read-accesses-to-memory-addresses-that-do-not-fall-under-a-defined-mpu-region
	SCB_MPU_RASR = SCB_MPU_RASR_TEX(0) | NOACCESS | NOEXEC | SIZE_4G;
	
	SCB_MPU_RBAR = 0x00000000 | REGION(i++); // ITCM
	SCB_MPU_RASR = MEM_NOCACHE | READONLY | SIZE_512K;

	// TODO: trap regions should be created last, because the hardware gives
	//  priority to the higher number ones.
	SCB_MPU_RBAR = 0x00000000 | REGION(i++); // trap NULL pointer deref
	SCB_MPU_RASR =  DEV_NOCACHE | NOACCESS | SIZE_32B;

	SCB_MPU_RBAR = 0x00200000 | REGION(i++); // Boot ROM
	SCB_MPU_RASR = MEM_CACHE_WT | READONLY | SIZE_128K;

	SCB_MPU_RBAR = 0x20000000 | REGION(i++); // DTCM
	SCB_MPU_RASR = MEM_NOCACHE | READWRITE | NOEXEC | SIZE_512K;
	
	SCB_MPU_RBAR = ((uint32_t)&_ebss) | REGION(i++); // trap stack overflow
	SCB_MPU_RASR = SCB_MPU_RASR_TEX(0) | NOACCESS | NOEXEC | SIZE_32B;

	SCB_MPU_RBAR = 0x20200000 | REGION(i++); // RAM (AXI bus)
	SCB_MPU_RASR = MEM_CACHE_WBWA | READWRITE | NOEXEC | SIZE_1M;

	SCB_MPU_RBAR = 0x40000000 | REGION(i++); // Peripherals
	SCB_MPU_RASR = DEV_NOCACHE | READWRITE | NOEXEC | SIZE_64M;

	SCB_MPU_RBAR = 0x60000000 | REGION(i++); // QSPI Flash
	SCB_MPU_RASR = MEM_CACHE_WBWA | READONLY | SIZE_16M;

	SCB_MPU_RBAR = 0x70000000 | REGION(i++); // FlexSPI2
	SCB_MPU_RASR = MEM_CACHE_WBWA | READWRITE | NOEXEC | SIZE_16M;

	SCB_MPU_RBAR = 0x80000000 | REGION(i++); // SEMC: SDRAM, NAND, SRAM, etc
	SCB_MPU_RASR = MEM_CACHE_WBWA | READWRITE | NOEXEC | SIZE_1G;
	// hardware: https://forum.pjrc.com/index.php?threads/73898/#post-334041
	// software: https://github.com/mjs513/SDRAM_t4

	asm("nop"); // allow a few cycles for bus writes before enable MPU
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	SCB_MPU_CTRL = SCB_MPU_CTRL_ENABLE;

	// cache enable, ARM DDI0403E, pg 628
	asm("dsb");
	asm("isb");
	SCB_CACHE_ICIALLU = 0;

	asm("dsb");
	asm("isb");
	SCB_CCR |= (SCB_CCR_IC | SCB_CCR_DC);
}

#ifdef ARDUINO_TEENSY41

#define LUT0(opcode, pads, operand) (FLEXSPI_LUT_INSTRUCTION((opcode), (pads), (operand)))
#define LUT1(opcode, pads, operand) (FLEXSPI_LUT_INSTRUCTION((opcode), (pads), (operand)) << 16)
#define CMD_SDR         FLEXSPI_LUT_OPCODE_CMD_SDR
#define ADDR_SDR        FLEXSPI_LUT_OPCODE_RADDR_SDR
#define READ_SDR        FLEXSPI_LUT_OPCODE_READ_SDR
#define WRITE_SDR       FLEXSPI_LUT_OPCODE_WRITE_SDR
#define DUMMY_SDR       FLEXSPI_LUT_OPCODE_DUMMY_SDR
#define PINS1           FLEXSPI_LUT_NUM_PADS_1
#define PINS4           FLEXSPI_LUT_NUM_PADS_4

FLASHMEM static void flexspi2_command(uint32_t index, uint32_t addr)
{
	FLEXSPI2_IPCR0 = addr;
	FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(index);
	FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
	while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)); // wait
	FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

FLASHMEM static uint32_t flexspi2_psram_id(uint32_t addr)
{
	FLEXSPI2_IPCR0 = addr;
	FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(3) | FLEXSPI_IPCR1_IDATSZ(4);
	FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
	while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)); // wait
	uint32_t id = FLEXSPI2_RFDR0;
	FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE | FLEXSPI_INTR_IPRXWA;
	return id & 0xFFFF;
}

FLASHMEM void configure_external_ram()
{
	// initialize pins
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_22 = 0x1B0F9; // 100K pullup, strong drive, max speed, hyst
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_23 = 0x110F9; // keeper, strong drive, max speed, hyst
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_24 = 0x1B0F9; // 100K pullup, strong drive, max speed, hyst
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_25 = 0x100F9; // strong drive, max speed, hyst
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_26 = 0x170F9; // 47K pullup, strong drive, max speed, hyst
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_27 = 0x170F9; // 47K pullup, strong drive, max speed, hyst
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_28 = 0x170F9; // 47K pullup, strong drive, max speed, hyst
	IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_29 = 0x170F9; // 47K pullup, strong drive, max speed, hyst

	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_22 = 8 | 0x10; // ALT1 = FLEXSPI2_A_SS1_B (Flash)
	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_23 = 8 | 0x10; // ALT1 = FLEXSPI2_A_DQS
	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_24 = 8 | 0x10; // ALT1 = FLEXSPI2_A_SS0_B (RAM)
	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_25 = 8 | 0x10; // ALT1 = FLEXSPI2_A_SCLK
	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_26 = 8 | 0x10; // ALT1 = FLEXSPI2_A_DATA0
	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_27 = 8 | 0x10; // ALT1 = FLEXSPI2_A_DATA1
	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_28 = 8 | 0x10; // ALT1 = FLEXSPI2_A_DATA2
	IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_29 = 8 | 0x10; // ALT1 = FLEXSPI2_A_DATA3

	IOMUXC_FLEXSPI2_IPP_IND_DQS_FA_SELECT_INPUT = 1; // GPIO_EMC_23 for Mode: ALT8, pg 986
	IOMUXC_FLEXSPI2_IPP_IND_IO_FA_BIT0_SELECT_INPUT = 1; // GPIO_EMC_26 for Mode: ALT8
	IOMUXC_FLEXSPI2_IPP_IND_IO_FA_BIT1_SELECT_INPUT = 1; // GPIO_EMC_27 for Mode: ALT8
	IOMUXC_FLEXSPI2_IPP_IND_IO_FA_BIT2_SELECT_INPUT = 1; // GPIO_EMC_28 for Mode: ALT8
	IOMUXC_FLEXSPI2_IPP_IND_IO_FA_BIT3_SELECT_INPUT = 1; // GPIO_EMC_29 for Mode: ALT8
	IOMUXC_FLEXSPI2_IPP_IND_SCK_FA_SELECT_INPUT = 1; // GPIO_EMC_25 for Mode: ALT8

	// turn on clock  (TODO: increase clock speed later, slow & cautious for first release)
	CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK))
		| CCM_CBCMR_FLEXSPI2_PODF(5) | CCM_CBCMR_FLEXSPI2_CLK_SEL(3); // 88 MHz
	CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);

	FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
	FLEXSPI2_MCR0 = (FLEXSPI2_MCR0 & ~(FLEXSPI_MCR0_AHBGRANTWAIT_MASK
		 | FLEXSPI_MCR0_IPGRANTWAIT_MASK | FLEXSPI_MCR0_SCKFREERUNEN
		 | FLEXSPI_MCR0_COMBINATIONEN | FLEXSPI_MCR0_DOZEEN
		 | FLEXSPI_MCR0_HSEN | FLEXSPI_MCR0_ATDFEN | FLEXSPI_MCR0_ARDFEN
		 | FLEXSPI_MCR0_RXCLKSRC_MASK | FLEXSPI_MCR0_SWRESET))
		| FLEXSPI_MCR0_AHBGRANTWAIT(0xFF) | FLEXSPI_MCR0_IPGRANTWAIT(0xFF)
		| FLEXSPI_MCR0_RXCLKSRC(1) | FLEXSPI_MCR0_MDIS;
	FLEXSPI2_MCR1 = FLEXSPI_MCR1_SEQWAIT(0xFFFF) | FLEXSPI_MCR1_AHBBUSWAIT(0xFFFF);
	FLEXSPI2_MCR2 = (FLEXSPI_MCR2 & ~(FLEXSPI_MCR2_RESUMEWAIT_MASK
		 | FLEXSPI_MCR2_SCKBDIFFOPT | FLEXSPI_MCR2_SAMEDEVICEEN
		 | FLEXSPI_MCR2_CLRLEARNPHASE | FLEXSPI_MCR2_CLRAHBBUFOPT))
		| FLEXSPI_MCR2_RESUMEWAIT(0x20) /*| FLEXSPI_MCR2_SAMEDEVICEEN*/;

	FLEXSPI2_AHBCR = FLEXSPI2_AHBCR & ~(FLEXSPI_AHBCR_READADDROPT | FLEXSPI_AHBCR_PREFETCHEN
		| FLEXSPI_AHBCR_BUFFERABLEEN | FLEXSPI_AHBCR_CACHABLEEN);
	uint32_t mask = (FLEXSPI_AHBRXBUFCR0_PREFETCHEN | FLEXSPI_AHBRXBUFCR0_PRIORITY_MASK
		| FLEXSPI_AHBRXBUFCR0_MSTRID_MASK | FLEXSPI_AHBRXBUFCR0_BUFSZ_MASK);
	FLEXSPI2_AHBRXBUF0CR0 = (FLEXSPI2_AHBRXBUF0CR0 & ~mask)
		| FLEXSPI_AHBRXBUFCR0_PREFETCHEN | FLEXSPI_AHBRXBUFCR0_BUFSZ(64);
	FLEXSPI2_AHBRXBUF1CR0 = (FLEXSPI2_AHBRXBUF0CR0 & ~mask)
		| FLEXSPI_AHBRXBUFCR0_PREFETCHEN | FLEXSPI_AHBRXBUFCR0_BUFSZ(64);
	FLEXSPI2_AHBRXBUF2CR0 = mask;
	FLEXSPI2_AHBRXBUF3CR0 = mask;

	// RX watermark = one 64 bit line
	FLEXSPI2_IPRXFCR = (FLEXSPI_IPRXFCR & 0xFFFFFFC0) | FLEXSPI_IPRXFCR_CLRIPRXF;
	// TX watermark = one 64 bit line
	FLEXSPI2_IPTXFCR = (FLEXSPI_IPTXFCR & 0xFFFFFFC0) | FLEXSPI_IPTXFCR_CLRIPTXF;

	FLEXSPI2_INTEN = 0;
	FLEXSPI2_FLSHA1CR0 = 0x2000; // 8 MByte
	FLEXSPI2_FLSHA1CR1 = FLEXSPI_FLSHCR1_CSINTERVAL(2)
		| FLEXSPI_FLSHCR1_TCSH(3) | FLEXSPI_FLSHCR1_TCSS(3);
	FLEXSPI2_FLSHA1CR2 = FLEXSPI_FLSHCR2_AWRSEQID(6) | FLEXSPI_FLSHCR2_AWRSEQNUM(0)
		| FLEXSPI_FLSHCR2_ARDSEQID(5) | FLEXSPI_FLSHCR2_ARDSEQNUM(0);

	FLEXSPI2_FLSHA2CR0 = 0x2000; // 8 MByte
	FLEXSPI2_FLSHA2CR1 = FLEXSPI_FLSHCR1_CSINTERVAL(2)
		| FLEXSPI_FLSHCR1_TCSH(3) | FLEXSPI_FLSHCR1_TCSS(3);
	FLEXSPI2_FLSHA2CR2 = FLEXSPI_FLSHCR2_AWRSEQID(6) | FLEXSPI_FLSHCR2_AWRSEQNUM(0)
		| FLEXSPI_FLSHCR2_ARDSEQID(5) | FLEXSPI_FLSHCR2_ARDSEQNUM(0);

	FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;

	FLEXSPI2_LUTKEY = FLEXSPI_LUTKEY_VALUE;
	FLEXSPI2_LUTCR = FLEXSPI_LUTCR_UNLOCK;
	volatile uint32_t *luttable = &FLEXSPI2_LUT0;
	for (int i=0; i < 64; i++) luttable[i] = 0;
	FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
	while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) ; // wait

	FLEXSPI2_LUTKEY = FLEXSPI_LUTKEY_VALUE;
	FLEXSPI2_LUTCR = FLEXSPI_LUTCR_UNLOCK;

	// cmd index 0 = exit QPI mode
	FLEXSPI2_LUT0 = LUT0(CMD_SDR, PINS4, 0xF5);
	// cmd index 1 = reset enable
	FLEXSPI2_LUT4 = LUT0(CMD_SDR, PINS1, 0x66);
	// cmd index 2 = reset
	FLEXSPI2_LUT8 = LUT0(CMD_SDR, PINS1, 0x99);
	// cmd index 3 = read ID bytes
	FLEXSPI2_LUT12 = LUT0(CMD_SDR, PINS1, 0x9F) | LUT1(DUMMY_SDR, PINS1, 24);
	FLEXSPI2_LUT13 = LUT0(READ_SDR, PINS1, 1);
	// cmd index 4 = enter QPI mode
	FLEXSPI2_LUT16 = LUT0(CMD_SDR, PINS1, 0x35);
	// cmd index 5 = read QPI
	FLEXSPI2_LUT20 = LUT0(CMD_SDR, PINS4, 0xEB) | LUT1(ADDR_SDR, PINS4, 24);
	FLEXSPI2_LUT21 = LUT0(DUMMY_SDR, PINS4, 6) | LUT1(READ_SDR, PINS4, 1);
	// cmd index 6 = write QPI
	FLEXSPI2_LUT24 = LUT0(CMD_SDR, PINS4, 0x38) | LUT1(ADDR_SDR, PINS4, 24);
	FLEXSPI2_LUT25 = LUT0(WRITE_SDR, PINS4, 1);

	// look for the first PSRAM chip
	flexspi2_command(0, 0); // exit quad mode
	flexspi2_command(1, 0); // reset enable
	flexspi2_command(2, 0); // reset (is this really necessary?)
	if (flexspi2_psram_id(0) == 0x5D0D) {
		// first PSRAM chip is present, look for a second PSRAM chip
		flexspi2_command(4, 0);
		flexspi2_command(0, 0x800000); // exit quad mode
		flexspi2_command(1, 0x800000); // reset enable
		flexspi2_command(2, 0x800000); // reset (is this really necessary?)
		if (flexspi2_psram_id(0x800000) == 0x5D0D) {
			flexspi2_command(4, 0x800000);
			// Two PSRAM chips are present, 16 MByte
			external_psram_size = 16;
		} else {
			// One PSRAM chip is present, 8 MByte
			external_psram_size = 8;
		}
		// TODO: zero uninitialized EXTMEM variables
		// TODO: copy from flash to initialize EXTMEM variables
		sm_set_pool(&extmem_smalloc_pool, &_extram_end,
			external_psram_size * 0x100000 -
			((uint32_t)&_extram_end - (uint32_t)&_extram_start),
			1, NULL);
	} else {
		// No PSRAM
		memset(&extmem_smalloc_pool, 0, sizeof(extmem_smalloc_pool));
	}
}

#endif // ARDUINO_TEENSY41


FLASHMEM void usb_pll_start()
{
	while (1) {
		uint32_t n = CCM_ANALOG_PLL_USB1; // pg 759
		printf("CCM_ANALOG_PLL_USB1=%08lX\n", n);
		if (n & CCM_ANALOG_PLL_USB1_DIV_SELECT) {
			printf("  ERROR, 528 MHz mode!\n"); // never supposed to use this mode!
			CCM_ANALOG_PLL_USB1_CLR = 0xC000;			// bypass 24 MHz
			CCM_ANALOG_PLL_USB1_SET = CCM_ANALOG_PLL_USB1_BYPASS;	// bypass
			CCM_ANALOG_PLL_USB1_CLR = CCM_ANALOG_PLL_USB1_POWER |	// power down
				CCM_ANALOG_PLL_USB1_DIV_SELECT |		// use 480 MHz
				CCM_ANALOG_PLL_USB1_ENABLE |			// disable
				CCM_ANALOG_PLL_USB1_EN_USB_CLKS;		// disable usb
			continue;
		}
		if (!(n & CCM_ANALOG_PLL_USB1_ENABLE)) {
			printf("  enable PLL\n");
			// TODO: should this be done so early, or later??
			CCM_ANALOG_PLL_USB1_SET = CCM_ANALOG_PLL_USB1_ENABLE;
			continue;
		}
		if (!(n & CCM_ANALOG_PLL_USB1_POWER)) {
			printf("  power up PLL\n");
			CCM_ANALOG_PLL_USB1_SET = CCM_ANALOG_PLL_USB1_POWER;
			continue;
		}
		if (!(n & CCM_ANALOG_PLL_USB1_LOCK)) {
			printf("  wait for lock\n");
			continue;
		}
		if (n & CCM_ANALOG_PLL_USB1_BYPASS) {
			printf("  turn off bypass\n");
			CCM_ANALOG_PLL_USB1_CLR = CCM_ANALOG_PLL_USB1_BYPASS;
			continue;
		}
		if (!(n & CCM_ANALOG_PLL_USB1_EN_USB_CLKS)) {
			printf("  enable USB clocks\n");
			CCM_ANALOG_PLL_USB1_SET = CCM_ANALOG_PLL_USB1_EN_USB_CLKS;
			continue;
		}
		return; // everything is as it should be  :-)
	}
}

FLASHMEM void reset_PFD()
{	
	//Reset PLL2 PFDs, set default frequencies:
	CCM_ANALOG_PFD_528_SET = (1 << 31) | (1 << 23) | (1 << 15) | (1 << 7);
	CCM_ANALOG_PFD_528 = 0x2018101B; // PFD0:352, PFD1:594, PFD2:396, PFD3:297 MHz 	
	//PLL3:
	CCM_ANALOG_PFD_480_SET = (1 << 31) | (1 << 23) | (1 << 15) | (1 << 7);	
	CCM_ANALOG_PFD_480 = 0x13110D0C; // PFD0:720, PFD1:664, PFD2:508, PFD3:454 MHz
}

extern void usb_isr(void);

// Stack frame
//  xPSR
//  ReturnAddress
//  LR (R14) - typically FFFFFFF9 for IRQ or Exception
//  R12
//  R3
//  R2
//  R1
//  R0
// Code from :: https://community.nxp.com/thread/389002


__attribute__((naked))
void unused_interrupt_vector(void)
{
	uint32_t i, ipsr, crc, count;
	const uint32_t *stack;
	struct arm_fault_info_struct *info;
	const uint32_t *p, *end;

	// disallow any nested interrupts
	__disable_irq();
	// store crash report info
	asm volatile("mrs %0, ipsr\n" : "=r" (ipsr) :: "memory");
	info = (struct arm_fault_info_struct *)0x2027FF80;
	info->ipsr = ipsr;
	asm volatile("tst lr, #4\nite eq\nmrseq %0, msp\nmrsne %0, psp\n" : "=r" (stack) :: "memory");
	info->cfsr = SCB_CFSR;
	info->hfsr = SCB_HFSR;
	info->mmfar = SCB_MMFAR;
	info->bfar = SCB_BFAR;
	info->ret = stack[6];
	info->xpsr = stack[7];
	info->temp = tempmonGetTemp();
	info->time = rtc_get();
	info->len = sizeof(*info) / 4;
	// add CRC to crash report
	crc = 0xFFFFFFFF;
	p = (uint32_t *)info;
	end = p + (sizeof(*info) / 4 - 1);
	while (p < end) {
		crc ^= *p++;
		for (i=0; i < 32; i++) crc = (crc >> 1) ^ (crc & 1)*0xEDB88320;
	}
	info->crc = crc;
	arm_dcache_flush_delete(info, sizeof(*info));

	// LED blink can show fault mode - by default we don't mess with pin 13
	//IOMUXC_SW_MUX_CTL_PAD_GPIO_B0_03 = 5; // pin 13
	//IOMUXC_SW_PAD_CTL_PAD_GPIO_B0_03 = IOMUXC_PAD_DSE(7);
	//GPIO7_GDIR |= (1 << 3);

	// reinitialize PIT timer and CPU clock
	CCM_CCGR1 |= CCM_CCGR1_PIT(CCM_CCGR_ON);
	PIT_MCR = PIT_MCR_MDIS;
	CCM_CSCMR1 = (CCM_CSCMR1 & ~CCM_CSCMR1_PERCLK_PODF(0x3F)) | CCM_CSCMR1_PERCLK_CLK_SEL;
  	if (F_CPU_ACTUAL > 198000000) set_arm_clock(198000000);
	PIT_MCR = 0;
	PIT_TCTRL0 = 0;
	PIT_LDVAL0 = 2400000; // 2400000 = 100ms
	PIT_TCTRL0 = PIT_TCTRL_TEN;
	// disable all NVIC interrupts, as usb_isr() might use __enable_irq()
	NVIC_ICER0 = 0xFFFFFFFF;
	NVIC_ICER1 = 0xFFFFFFFF;
	NVIC_ICER2 = 0xFFFFFFFF;
	NVIC_ICER3 = 0xFFFFFFFF;
	NVIC_ICER4 = 0xFFFFFFFF;

	// keep USB running, so any unsent Serial.print() actually arrives in
	// the Arduino Serial Monitor, and we remain responsive to Upload
	// without requiring manual press of Teensy's pushbutton
	count = 0;
	while (1) {
		if (PIT_TFLG0) {
			//GPIO7_DR_TOGGLE = (1 << 3); // blink LED
			PIT_TFLG0 = 1;
			if (++count >= 80) break;  // reboot after 8 seconds
		}
		usb_isr();
		// TODO: should other data flush / cleanup tasks be done here?
		//   Transmit Serial1 - Serial8 data
		//   Complete writes to SD card
		//   Flush/sync LittleFS
	}
	// turn off USB
	USB1_USBCMD = USB_USBCMD_RST;
	USBPHY1_CTRL_SET = USBPHY_CTRL_SFTRST;
	while (PIT_TFLG0 == 0) /* wait 0.1 second for PC to know USB unplugged */
	// reboot
	SRC_GPR5 = 0x0BAD00F1;
	SCB_AIRCR = 0x05FA0004;
	while (1) ;
}

__attribute__((section(".startup"), noinline))
static void memory_copy(uint32_t *dest, const uint32_t *src, uint32_t *dest_end)
{
#if 0
	if (dest == src) return;
	do {
		*dest++ = *src++;
	} while (dest < dest_end);
#else
	asm volatile(
	"	cmp	%[src], %[dest]		\n"
	"	beq.n	2f			\n"
	"1:	ldr.w	r3, [%[src]], #4	\n"
	"	str.w	r3, [%[dest]], #4	\n"
	"	cmp	%[end], %[dest]		\n"
	"	bhi.n	1b			\n"
	"2:					\n"
	: [dest] "+r" (dest), [src] "+r" (src) : [end] "r" (dest_end) : "r3", "memory");
#endif
}

__attribute__((section(".startup"), noinline))
static void memory_clear(uint32_t *dest, uint32_t *dest_end)
{
#if 0
	while (dest < dest_end) {
		*dest++ = 0;
	}
#else
	asm volatile(
	"	ldr	r3, =0			\n"
	"1:	str.w	r3, [%[dest]], #4	\n"
	"	cmp	%[end], %[dest]		\n"
	"	bhi.n	1b			\n"
	: [dest] "+r" (dest) : [end] "r" (dest_end) : "r3", "memory");
#endif
}



// syscall functions need to be in the same C file as the entry point "ResetVector"
// otherwise the linker will discard them in some cases.

#include <errno.h>

// from the linker script
extern unsigned long _heap_start;
extern unsigned long _heap_end;

char *__brkval = (char *)&_heap_start;

__attribute__((weak))
void * _sbrk(int incr)
{
        char *prev = __brkval;
        if (incr != 0) {
                if (prev + incr > (char *)&_heap_end) {
                        errno = ENOMEM;
                        return (void *)-1;
                }
                __brkval = prev + incr;
        }
        return prev;
}

__attribute__((weak))
int _read(int file __attribute__((unused)), char *ptr __attribute__((unused)), int len __attribute__((unused)))
{
	return 0;
}

__attribute__((weak))
int _close(int fd __attribute__((unused)))
{
	return -1;
}

#include <sys/stat.h>

__attribute__((weak))
int _fstat(int fd __attribute__((unused)), struct stat *st)
{
	st->st_mode = S_IFCHR;
	return 0;
}

__attribute__((weak))
int _isatty(int fd __attribute__((unused)))
{
	return 1;
}

__attribute__((weak))
int _lseek(int fd __attribute__((unused)), long long offset __attribute__((unused)), int whence __attribute__((unused)))
{
	return -1;
}

__attribute__((weak))
void _exit(int status __attribute__((unused)))
{
	while (1) asm ("WFI");
}

__attribute__((weak))
void __cxa_pure_virtual()
{
	while (1) asm ("WFI");
}

__attribute__((weak))
int __cxa_guard_acquire (char *g)
{
	return !(*g);
}

__attribute__((weak))
void __cxa_guard_release(char *g)
{
	*g = 1;
}

__attribute__((weak))
void abort(void)
{
	while (1) asm ("WFI");
}
