/*
 * ============================================================================
 *  HUD SPEEDOMETER
 *  STM32 Nucleo-L432KC
 * ============================================================================
 *
 *  Reads vehicle speed from the car's OBD-II CAN bus (ISO 15765-4, 500 kbps,
 *  11-bit IDs) and shows it in mph on a Lite-On LTD-4608JR 2-digit display,
 *  mirrored so it reads correctly reflected in the windshield.
 *
 *  ---------------------------------------------------------------------------
 *  WIRING SUMMARY (full detail in docs/BUILD_GUIDE.md)
 *  ---------------------------------------------------------------------------
 *   CAN transceiver (SN65HVD230):
 *     PA12 / D2   -> CTX          PA11 / D10  -> CRX
 *     3V3 -> 3V3, GND -> GND, CANH -> OBD pin 6, CANL -> OBD pin 14
 *     OBD pin 4 or 5 (GND) -> board GND   <-- required, even on USB power
 *
 *   Display segments, each through 150 ohm  (cathodes -> STM32 pins):
 *     disp 10 (a) -> PA0 | disp 8 (b) -> PA1 | disp 1 (c) -> PA3
 *     disp  5 (d) -> PA4 | disp 3 (e) -> PA5 | disp 6 (f) -> PA6
 *     disp  7 (g) -> PA7 | disp 2 (DP) unconnected
 *
 *   Digit drivers, 2x PNP (2N2907 / BC327 / 2N3906):
 *     emitter -> 3V3, collector -> disp pin 9 (left) / pin 4 (right)
 *     base -> 1k -> PB0 / D3 (left)  and  PB1 / D6 (right)
 *
 *   Onboard LED LD3 (PB3) blinks a fault code if startup fails before the
 *   display is running. See Error_Handler().
 *
 *  ---------------------------------------------------------------------------
 *  BUILD: VS Code + PlatformIO. Click Build, then Upload. Nothing else.
 * ============================================================================
 */

#include "stm32l4xx_hal.h"
#include <stdio.h>

/* ===========================================================================
 *  SETTINGS: the only things you should normally need to change
 * ========================================================================= */

/* 1 = mirror the digits for windshield reflection (final install)
 * 0 = normal orientation, for reading the display directly on the bench   */
#define HUD_MIRROR        1

/* 0 = common ANODE display + PNP digit drivers  <-- LTD-4608JR
 * 1 = common cathode display + NPN/N-MOSFET drivers                       */
#define COMMON_CATHODE    0

/* 1 = print status once a second to the USB serial port (PlatformIO's
 *     "Serial Monitor", 115200 baud). Very useful during bring-up.
 * 0 = silent.                                                             */
#define DEBUG_UART        1

/* 1 = BENCH SELF-TEST. Puts the CAN controller in silent loopback mode,
 *     where it receives its own transmissions without driving the bus,
 *     and feeds itself a synthetic ECU reply with a speed that sweeps
 *     0 -> 99 mph and repeats. Needs NO car, NO transceiver, NO OBD cable,
 *     just the board and the display. Exercises everything except the
 *     transceiver and the car's wiring.
 * 0 = normal operation.                                                   */
#define BENCH_TEST        0

#define REQUEST_MS        100    /* how often to ask the ECU for speed     */
#define DATA_TIMEOUT_MS   1000   /* show "--" if no reply within this long */
#define LAMP_TEST_MS      800    /* show "88" at power-up for this long    */

/* ===========================================================================
 *  Handles and shared state
 * ========================================================================= */

CAN_HandleTypeDef  hcan1;
TIM_HandleTypeDef  htim6;
#if DEBUG_UART
UART_HandleTypeDef huart2;
#endif

static volatile uint8_t  speed_mph  = 0;   /* latest converted speed       */
static volatile uint8_t  speed_kmh  = 0;   /* raw value, for debug output  */
static volatile uint8_t  have_data  = 0;   /* 0 -> display shows "--"      */
static volatile uint32_t last_rx_ms = 0;
static volatile uint32_t rx_count   = 0;
static volatile uint32_t tx_count   = 0;

/* Segment patterns the multiplex ISR paints. [0] = physical LEFT digit.   */
static volatile uint8_t  glyph[2] = {0, 0};

/* Millisecond counter driven by the TIM6 multiplex interrupt. Deliberately
 * independent of SysTick: if SysTick ever fails, the display and all of
 * main()'s timing still work, and we can SHOW that rather than freezing. */
static volatile uint32_t tim_ms = 0;

static uint8_t clock_source_is_lse = 0;    /* for the debug banner         */

/* 0 = healthy. Non-zero shows as "E<n>" on the display:
 *   2 CAN init   3 CAN filter   4 CAN start   9 SysTick dead              */
static volatile uint8_t  fault_code = 0;

static void Error_Handler(uint8_t blinks);

/* ===========================================================================
 *  7-segment font
 *  bit0=a bit1=b bit2=c bit3=d bit4=e bit5=f bit6=g
 * ========================================================================= */

#define GLYPH_BLANK  0x00
#define GLYPH_DASH   0x40
#define GLYPH_ALL    0x7F
#define GLYPH_E      0x79    /* a, d, e, f, g */

static const uint8_t FONT[10] = {
    0x3F, /* 0 */  0x06, /* 1 */  0x5B, /* 2 */  0x4F, /* 3 */  0x66, /* 4 */
    0x6D, /* 5 */  0x7D, /* 6 */  0x07, /* 7 */  0x7F, /* 8 */  0x6F  /* 9 */
};

#if HUD_MIRROR
/* Horizontal flip: a, d and g are left-right symmetric and stay put;
 * b<->f (upper pair) and c<->e (lower pair) trade places.                  */
static uint8_t flip(uint8_t s)
{
    uint8_t m = s & 0x49;                  /* keep a (bit0), d (3), g (6)  */
    if (s & (1u << 1)) m |= (1u << 5);     /* b -> f                       */
    if (s & (1u << 5)) m |= (1u << 1);     /* f -> b                       */
    if (s & (1u << 2)) m |= (1u << 4);     /* c -> e                       */
    if (s & (1u << 4)) m |= (1u << 2);     /* e -> c                       */
    return m;
}
#endif

/* ===========================================================================
 *  Display output
 * ========================================================================= */

static const uint16_t SEG_PIN[7] = {
    GPIO_PIN_0,  /* a */  GPIO_PIN_1,  /* b */  GPIO_PIN_3,  /* c */
    GPIO_PIN_4,  /* d */  GPIO_PIN_5,  /* e */  GPIO_PIN_6,  /* f */
    GPIO_PIN_7   /* g */
};
#define SEG_ALL_PINS (GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_3 | GPIO_PIN_4 | \
                      GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7)

#define DIG_L_PIN  GPIO_PIN_0     /* PB0 / D3 -> left  digit (disp pin 9)  */
#define DIG_R_PIN  GPIO_PIN_1     /* PB1 / D6 -> right digit (disp pin 4)  */
#define LED_PIN    GPIO_PIN_3     /* PB3      -> onboard LD3               */

#if COMMON_CATHODE
  #define SEG_ON   GPIO_PIN_SET     /* pin high sources into the segment   */
  #define SEG_OFF  GPIO_PIN_RESET
  #define DIG_ON   GPIO_PIN_SET     /* NPN / N-MOSFET: high = conducting   */
  #define DIG_OFF  GPIO_PIN_RESET
#else
  #define SEG_ON   GPIO_PIN_RESET   /* pin low sinks from the segment      */
  #define SEG_OFF  GPIO_PIN_SET
  #define DIG_ON   GPIO_PIN_RESET   /* PNP: base low = conducting          */
  #define DIG_OFF  GPIO_PIN_SET
#endif

/* Work out the two glyphs from the current state. Called from main().     */
static void render(void)
{
    uint8_t g_tens, g_ones, l, r;

    if (fault_code) {                       /* "E2", "E9", ...             */
        g_tens = GLYPH_E;
        g_ones = FONT[fault_code % 10u];
    } else if (!have_data) {
        g_tens = GLYPH_DASH;
        g_ones = GLYPH_DASH;
    } else {
        uint8_t s = speed_mph;
        if (s > 99u) s = 99u;
        g_tens = (s >= 10u) ? FONT[s / 10u] : GLYPH_BLANK;  /* no leading 0 */
        g_ones = FONT[s % 10u];
    }

#if HUD_MIRROR
    /* The reflection swaps left and right, so the tens digit goes on the
     * physical RIGHT in order to appear on the visual LEFT.               */
    l = flip(g_ones);
    r = flip(g_tens);
#else
    l = g_tens;
    r = g_ones;
#endif

    /* Two bytes must land together or a frame could show mismatched
     * digits. Cheaper and clearer than double-buffering.                  */
    __disable_irq();
    glyph[0] = l;
    glyph[1] = r;
    __enable_irq();
}

/* Paint one digit. TIM6 fires at 1 kHz, so each digit refreshes at 500 Hz,
 * far above the flicker threshold and above the frame rate of most phone
 * cameras too.                                                            */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    static uint8_t cur = 0;
    uint8_t g;
    int i;

    if (htim->Instance != TIM6) return;

    tim_ms++;              /* 1 kHz -> a millisecond timebase we control    */

    /* Blank both digits while the segment lines change, otherwise the
     * outgoing digit briefly shows the incoming digit's pattern (ghosting).
     */
    HAL_GPIO_WritePin(GPIOB, DIG_L_PIN, DIG_OFF);
    HAL_GPIO_WritePin(GPIOB, DIG_R_PIN, DIG_OFF);

    g = glyph[cur];
    for (i = 0; i < 7; i++) {
        HAL_GPIO_WritePin(GPIOA, SEG_PIN[i],
                          ((g >> i) & 1u) ? SEG_ON : SEG_OFF);
    }

    HAL_GPIO_WritePin(GPIOB, (cur == 0) ? DIG_L_PIN : DIG_R_PIN, DIG_ON);
    cur ^= 1u;
}

/* Busy-wait on the timer tick. Independent of SysTick and of the HAL, so
 * it works during early bring-up and can never hang if SysTick is broken. */
static void tim_delay(uint32_t ms)
{
    uint32_t start = tim_ms;
    while ((tim_ms - start) < ms) { __NOP(); }
}

/* ===========================================================================
 *  OBD-II over CAN
 * ========================================================================= */

/* Standard OBD-II request: 2 payload bytes, service 01, PID 0x0D (vehicle
 * speed). 0x7DF is the functional broadcast address: whichever ECU owns the
 * PID answers, so we don't need to know the car's ECU layout.             */
static void send_request(void)
{
    CAN_TxHeaderTypeDef tx;
    uint8_t  data[8] = {0x02, 0x01, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t mailbox;

    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0) return;

#if BENCH_TEST
    /* Impersonate the ECU: a well-formed response to PID 0x0D, with a
     * km/h value that ramps. In loopback this comes straight back in
     * through the normal receive path, filter and all.                   */
    {
        static uint8_t fake_kmh = 0;
        fake_kmh = (uint8_t)((fake_kmh + 1u) % 160u);   /* 0..159 km/h     */
        data[0] = 0x03; data[1] = 0x41; data[2] = 0x0D; data[3] = fake_kmh;
    }
    tx.StdId              = 0x7E8;
#else
    tx.StdId              = 0x7DF;
#endif
    tx.ExtId              = 0;
    tx.IDE                = CAN_ID_STD;
    tx.RTR                = CAN_RTR_DATA;
    tx.DLC                = 8;
    tx.TransmitGlobalTime = DISABLE;

    /* 29-bit (extended ID) cars, mostly heavy trucks and some pre-2008
     * vehicles: use tx.IDE = CAN_ID_EXT and tx.ExtId = 0x18DB33F1 here,
     * widen the RX filter in CAN_Init() to 0x18DAF100-0x18DAF1FF, and
     * match those IDs in HAL_CAN_RxFifo0MsgPendingCallback().            */

    if (HAL_CAN_AddTxMessage(&hcan1, &tx, data, &mailbox) == HAL_OK) {
        tx_count++;
    }
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef hdr;
    uint8_t data[8];

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &hdr, data) != HAL_OK) return;

    /* Expected positive response:  03 41 0D <km/h> ...                    */
    if (hdr.IDE == CAN_ID_STD && hdr.StdId >= 0x7E8u && hdr.StdId <= 0x7EFu &&
        hdr.DLC >= 4u && data[1] == 0x41u && data[2] == 0x0Du)
    {
        uint32_t kmh = data[3];
        speed_kmh  = (uint8_t)kmh;
        /* mph = km/h * 0.621371, rounded to nearest, integer-only         */
        speed_mph  = (uint8_t)((kmh * 621u + 500u) / 1000u);
        have_data  = 1;
        last_rx_ms = tim_ms;
        rx_count++;
    }
}

/* ===========================================================================
 *  Clock configuration
 *
 *  Preferred: MSI at 4 MHz auto-trimmed by the board's 32.768 kHz crystal
 *  (MSI PLL mode). That is better than 0.25 % accurate, comfortably inside
 *  CAN's tolerance budget of roughly 0.5 % at these bit timings.
 *
 *  If the crystal fails to start, fall back to HSI16 so the firmware always
 *  boots. HSI16 is marginal for CAN, so the debug banner reports which
 *  source is active. Either path ends at 80 MHz.
 * ========================================================================= */

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) {
        Error_Handler(1);
    }

    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();          /* LSE lives in the backup domain */

    /* Attempt 1: bring up the LSE crystal on its own */
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState       = RCC_LSE_ON;
    osc.PLL.PLLState   = RCC_PLL_NONE;   /* mandatory: don't touch the PLL */
    clock_source_is_lse = (HAL_RCC_OscConfig(&osc) == HAL_OK) ? 1u : 0u;

    /* Attempt 2: main oscillator + PLL to 80 MHz */
    osc = (RCC_OscInitTypeDef){0};
    if (clock_source_is_lse) {
        osc.OscillatorType      = RCC_OSCILLATORTYPE_MSI;
        osc.MSIState            = RCC_MSI_ON;
        osc.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
        osc.MSIClockRange       = RCC_MSIRANGE_6;      /* 4 MHz            */
        osc.PLL.PLLSource       = RCC_PLLSOURCE_MSI;
        osc.PLL.PLLN            = 40;                  /* 4 * 40 / 2 = 80  */
    } else {
        osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
        osc.HSIState            = RCC_HSI_ON;
        osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
        osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
        osc.PLL.PLLN            = 10;                  /* 16 * 10 / 2 = 80 */
    }
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLM     = 1;
    osc.PLL.PLLP     = RCC_PLLP_DIV7;
    osc.PLL.PLLQ     = RCC_PLLQ_DIV2;
    osc.PLL.PLLR     = RCC_PLLR_DIV2;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler(1);

    /* Lock MSI to the crystal. Must happen after MSI is running.          */
    if (clock_source_is_lse) HAL_RCCEx_EnableMSIPLLMode();

    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                         RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;    /* CAN and TIM6 both run at 80  */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) Error_Handler(1);
}

/* ===========================================================================
 *  Peripheral init
 * ========================================================================= */

static void GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Segment lines. Start with everything off so nothing flashes during
     * the rest of init.                                                   */
    HAL_GPIO_WritePin(GPIOA, SEG_ALL_PINS, SEG_OFF);
    g.Pin   = SEG_ALL_PINS;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);

    /* Digit drivers + status LED.                                         */
    HAL_GPIO_WritePin(GPIOB, DIG_L_PIN | DIG_R_PIN, DIG_OFF);
    HAL_GPIO_WritePin(GPIOB, LED_PIN, GPIO_PIN_RESET);
    g.Pin = DIG_L_PIN | DIG_R_PIN | LED_PIN;
    HAL_GPIO_Init(GPIOB, &g);
}

/* Returns 0 on success, or a fault code. Never fatal: a car with the
 * ignition off, or a bench with no transceiver, must not brick the HUD.  */
static uint8_t CAN_Init(void)
{
    CAN_FilterTypeDef f = {0};

    /* Bit timing: APB1 = 80 MHz, prescaler 10 -> 8 MHz time-quantum clock.
     * 1 (sync) + 13 (BS1) + 2 (BS2) = 16 tq per bit -> 500 kbit/s, with the
     * sample point at 14/16 = 87.5 % (the CiA / automotive standard).     */
    hcan1.Instance                  = CAN1;
    hcan1.Init.Prescaler            = 10;
#if BENCH_TEST
    hcan1.Init.Mode                 = CAN_MODE_SILENT_LOOPBACK;
#else
    hcan1.Init.Mode                 = CAN_MODE_NORMAL;
#endif
    hcan1.Init.SyncJumpWidth        = CAN_SJW_2TQ;   /* resync margin      */
    hcan1.Init.TimeSeg1             = CAN_BS1_13TQ;
    hcan1.Init.TimeSeg2             = CAN_BS2_2TQ;
    hcan1.Init.TimeTriggeredMode    = DISABLE;
    hcan1.Init.AutoBusOff           = ENABLE;   /* recover automatically   */
    hcan1.Init.AutoWakeUp           = DISABLE;
    hcan1.Init.AutoRetransmission   = DISABLE;  /* stale speed is useless; */
                                                /* drop it and ask again   */
    hcan1.Init.ReceiveFifoLocked    = DISABLE;
    hcan1.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan1) != HAL_OK) return 2;

    /* Accept 0x7E8-0x7EF (the ECU response range) and ignore the rest of
     * the bus, which on a running car is a firehose of other traffic.     */
    f.FilterBank           = 0;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh         = (uint16_t)(0x7E8u << 5);
    f.FilterIdLow          = 0x0000;
    f.FilterMaskIdHigh     = (uint16_t)(0x7F8u << 5);
    f.FilterMaskIdLow      = 0x0000;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    f.SlaveStartFilterBank = 14;

    if (HAL_CAN_ConfigFilter(&hcan1, &f) != HAL_OK)  return 3;
    if (HAL_CAN_Start(&hcan1) != HAL_OK)             return 4;
    if (HAL_CAN_ActivateNotification(&hcan1,
            CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)  return 4;
    return 0;
}

static void TIM6_Init(void)
{
    __HAL_RCC_TIM6_CLK_ENABLE();

    /* 80 MHz / 8000 = 10 kHz counter; reload every 10 -> 1 kHz interrupt. */
    htim6.Instance               = TIM6;
    htim6.Init.Prescaler         = 8000 - 1;
    htim6.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim6.Init.Period            = 10 - 1;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_Base_Init(&htim6) != HAL_OK) Error_Handler(5);

    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);

    if (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK) Error_Handler(5);
}

#if DEBUG_UART
static void UART2_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA2 -> ST-Link virtual COM port. Transmit only; we never read.      */
    g.Pin       = GPIO_PIN_2;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &g);

    huart2.Instance                    = USART2;
    huart2.Init.BaudRate               = 115200;
    huart2.Init.WordLength             = UART_WORDLENGTH_8B;
    huart2.Init.StopBits               = UART_STOPBITS_1;
    huart2.Init.Parity                 = UART_PARITY_NONE;
    huart2.Init.Mode                   = UART_MODE_TX;
    huart2.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart2.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    /* Non-fatal: a dead debug port shouldn't stop the speedometer.        */
    (void)HAL_UART_Init(&huart2);
}

static void dbg(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)n, 50);
}
#else
  #define dbg(s)  ((void)0)
#endif

/* CAN pin/clock/NVIC setup. The HAL calls this from inside HAL_CAN_Init(). */
void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
    GPIO_InitTypeDef g = {0};

    if (hcan->Instance != CAN1) return;

    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    g.Mode      = GPIO_MODE_AF_PP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF9_CAN1;

    g.Pin  = GPIO_PIN_12;                      /* PA12 = TX                */
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &g);

    /* PA11 = RX. The pull-up keeps the line recessive if the transceiver is
     * unplugged; without it a floating pin can read dominant and CAN never
     * leaves initialization mode.                                          */
    g.Pin  = GPIO_PIN_11;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &g);

    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
}

/* ===========================================================================
 *  Interrupt vectors
 *
 *  PlatformIO's stm32cube framework has no generated stm32l4xx_it.c, so the
 *  handlers this project needs are defined here. SysTick in particular is
 *  not optional: HAL_GetTick() and every HAL timeout depend on it.
 * ========================================================================= */

void SysTick_Handler(void)     { HAL_IncTick(); }
void TIM6_DAC_IRQHandler(void) { HAL_TIM_IRQHandler(&htim6); }
void CAN1_RX0_IRQHandler(void) { HAL_CAN_IRQHandler(&hcan1); }

/* ===========================================================================
 *  Fault indicator for failures before the display is running
 *
 *  Blinks the onboard green LED N times, pauses, repeats, so a board that
 *  won't start can still say why without a debugger attached:
 *      1 = clock configuration failed
 *      5 = timer init failed
 *  Faults after the display is up (CAN, SysTick) appear on the display as
 *  "E2", "E3", "E4" or "E9" instead.
 *
 *  Uses a crude busy-loop rather than HAL_Delay(), because error 1 means
 *  SysTick may not be running.
 * ========================================================================= */

static void Error_Handler(uint8_t blinks)
{
    volatile uint32_t d;
    uint8_t i;

    __disable_irq();

    /* LED may not be configured yet if we failed very early.              */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    {
        GPIO_InitTypeDef g = {0};
        g.Pin   = LED_PIN;
        g.Mode  = GPIO_MODE_OUTPUT_PP;
        g.Pull  = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOB, &g);
    }

    for (;;) {
        for (i = 0; i < blinks; i++) {
            HAL_GPIO_WritePin(GPIOB, LED_PIN, GPIO_PIN_SET);
            for (d = 0; d < 600000u; d++) { __NOP(); }
            HAL_GPIO_WritePin(GPIOB, LED_PIN, GPIO_PIN_RESET);
            for (d = 0; d < 600000u; d++) { __NOP(); }
        }
        for (d = 0; d < 4000000u; d++) { __NOP(); }
    }
}

/* ===========================================================================
 *  main
 * ========================================================================= */

int main(void)
{
    uint32_t next_request = 0;
    uint32_t next_retry   = 0;
    uint32_t systick_a;
#if DEBUG_UART
    uint32_t next_report  = 0;
    char     line[80];
#endif

    HAL_Init();
    SystemClock_Config();
    GPIO_Init();

    /* The timer comes up first: from here on the display is alive and can
     * report anything that goes wrong, instead of freezing mid-pattern.  */
    TIM6_Init();

    /* Power-up lamp test: every segment on, so a dead one is obvious.
     * Timed off tim_ms, NOT HAL_Delay, so it cannot hang.                 */
    glyph[0] = GLYPH_ALL;
    glyph[1] = GLYPH_ALL;
    tim_delay(LAMP_TEST_MS);

    /* --------------------------------------------------------------------
     * SysTick health check.
     * Every HAL function with a timeout (CAN init, UART transmit, HAL_Delay)
     * spins on HAL_GetTick(). If SysTick is not incrementing, those calls
     * never return. Check it here and report "E9" rather than hanging.
     * ------------------------------------------------------------------ */
    systick_a = HAL_GetTick();
    tim_delay(50);
    if (HAL_GetTick() == systick_a) {
        fault_code = 9;
        for (;;) { render(); }      /* display stays alive showing E9      */
    }

#if DEBUG_UART
    UART2_Init();
    dbg("\r\n=== HUD speedometer ===\r\n");
    dbg(clock_source_is_lse ? "clock: MSI trimmed by LSE (best)\r\n"
                            : "clock: HSI16 fallback (LSE did not start)\r\n");
#if BENCH_TEST
    dbg("MODE: BENCH SELF-TEST (silent loopback, no car needed)\r\n");
#endif
#endif

    fault_code = CAN_Init();
#if DEBUG_UART
    dbg(fault_code ? "CAN: init FAILED, will keep retrying\r\n"
                   : "CAN: up, waiting for ECU...\r\n");
#endif

    for (;;) {
        uint32_t now = tim_ms;

        if (fault_code) {
            /* Retry every 3 s. Plugging the transceiver in, or switching
             * the ignition on, recovers without a reset.                 */
            if ((int32_t)(now - next_retry) >= 0) {
                next_retry = now + 3000;
                fault_code = CAN_Init();
            }
        } else {
            if ((int32_t)(now - next_request) >= 0) {
                next_request = now + REQUEST_MS;
                send_request();
            }

            /* Bus asleep or cable pulled -> fall back to "--" rather than
             * freezing the last speed on screen. Signed difference, so a
             * reply that lands just after `now` was read can't look stale. */
            if (have_data &&
                (int32_t)(now - last_rx_ms) > (int32_t)DATA_TIMEOUT_MS) {
                have_data = 0;
            }
        }

        render();

#if DEBUG_UART
        if ((int32_t)(now - next_report) >= 0) {
            next_report = now + 1000;
            snprintf(line, sizeof line,
                     "tx=%lu rx=%lu  kmh=%u  mph=%u  %s\r\n",
                     (unsigned long)tx_count, (unsigned long)rx_count,
                     (unsigned)speed_kmh, (unsigned)speed_mph,
                     fault_code ? "CAN FAULT" : (have_data ? "OK" : "no data"));
            dbg(line);
        }
#endif

        tim_delay(5);
    }
}
