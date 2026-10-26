#ifndef TIMING_H_
#define TIMING_H_

#include "Perspective.h"

/* 平台二选一，不定义时按有没有 SYSTEM_CLOCK_250M 自动判断 */
#if !defined(PLATFORM_CH32) && !defined(PLATFORM_CYT4BB7)
    #ifdef SYSTEM_CLOCK_250M
        #define PLATFORM_CYT4BB7
    #else
        #define PLATFORM_CH32
    #endif
#endif

#define PERSPECTIVE_TEST_ROW        (RESULT_ROW)
#define PERSPECTIVE_TEST_COL        (RESULT_COL)
#define PERSPECTIVE_TEST_PIXELS     ((uint32)PERSPECTIVE_TEST_ROW * PERSPECTIVE_TEST_COL)

typedef struct
{
    uint32 tick;                    /* CH32 是 15 MHz 的 tick，4BB7 是周期 */
    uint32 time_ns;
    uint32 time_us;
    uint32 cycle;
    uint32 ns_per_pixel;
    uint32 cycle_per_pixel;
} perspective_time_result_struct;


#ifdef PLATFORM_CH32

/* CH32V307 的 SysTick（0xE000F000），64 位自由计数。
 * 用 HCLK/8 = 15 MHz：库里的 system_delay_ms/us 按这个算 CMP，切到 HCLK 会让库延时快 8 倍。
 * 每格 66.667 ns，1 格 = 8 个 CPU 周期。
 */
typedef struct
{
    volatile uint32 CTLR;
    volatile uint32 SR;
    volatile uint32 CNTL;
    volatile uint32 CNTH;
    volatile uint32 CMPLR;
    volatile uint32 CMPHR;
} cpu_time_stk_struct;

#define CPU_TIME_STK            ((cpu_time_stk_struct *)0xE000F000)

/* 配成 HCLK/8 自由计数模式并清零，main 里调一次 */
static inline void cpu_time_init (void)
{
    CPU_TIME_STK->CTLR = 0;
    CPU_TIME_STK->CTLR = (1u << 5);
    CPU_TIME_STK->SR  &= ~(1u << 0);
    CPU_TIME_STK->CTLR = (1u << 0);
}

/* 每次都从 0 开始，单次测量长度只受 64 位上限限制 */
static inline void cpu_time_start (void)
{
    CPU_TIME_STK->CTLR = 0;
    CPU_TIME_STK->CTLR = (1u << 5);
    CPU_TIME_STK->CTLR = (1u << 0);
}

/* 读数并停表。高 32 位读两次比较，避免跨进位时读到撕裂值；先读数再停表，停表开销不算进去 */
static inline uint64 cpu_time_stop (void)
{
    uint32 high_first  = 0;
    uint32 high_second = 0;
    uint32 low         = 0;
    uint64 tick        = 0;

    do
    {
        high_first  = CPU_TIME_STK->CNTH;
        low         = CPU_TIME_STK->CNTL;
        high_second = CPU_TIME_STK->CNTH;
    } while (high_first != high_second);

    tick = (((uint64)high_first) << 32) | (uint64)low;

    CPU_TIME_STK->CTLR &= ~(1u << 0);
    return tick;
}

/* tick 换算成纳秒，单次测量别超过约 4.29 秒 */
static inline uint32 cpu_time_tick_to_ns (uint64 tick)
{
    return (uint32)((tick * 1000ull) / 15ull);
}

/* tick 换算成 CPU 周期，1 个 tick = 8 个 HCLK 周期 */
static inline uint32 cpu_time_tick_to_cycle (uint64 tick)
{
    return (uint32)(tick * 8ull);
}

#else

/* CYT4BB7 的 DWT->CYCCNT，直接数 CPU 周期，不占外设和引脚。
 * 32 位，250 MHz 下约 17 秒溢出一次，测一次逆透视只要 1 ms 量级。
 */
static uint8 cpu_time_ready = 0;

/* 打开跟踪单元并启动周期计数器 */
static inline void cpu_time_init (void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
    cpu_time_ready = 1;
}

/* 清零重新计数 */
static inline void cpu_time_start (void)
{
    if (!cpu_time_ready)
    {
        cpu_time_init();
    }
    DWT->CYCCNT = 0;
}

/* 这里读到的就是周期数 */
static inline uint64 cpu_time_stop (void)
{
    return (uint64)DWT->CYCCNT;
}

/* 周期换算成纳秒 */
static inline uint32 cpu_time_tick_to_ns (uint64 tick)
{
    uint32 freq = (0 != SystemCoreClock) ? (uint32)SystemCoreClock : 250000000UL;
    return (uint32)((tick * 1000000000ull) / (uint64)freq);
}

/* 本来就是周期，原样返回 */
static inline uint32 cpu_time_tick_to_cycle (uint64 tick)
{
    return (uint32)tick;
}

#endif


/* 求和结果必须落地，否则优化一开编译器会把整个取像素循环删掉 */
static volatile uint32 perspective_test_sink = 0;

/* 遍历全图一次并计时，只包住 perspective_sum() 那一段 */
static inline void perspective_test_run (perspective_time_result_struct *result)
{
    uint32 sum = 0;

    if (NULL == result)
    {
        return;
    }

    cpu_time_start();
    sum = perspective_sum();
    result->tick = (uint32)cpu_time_stop();

    perspective_test_sink = sum;

    result->time_ns         = cpu_time_tick_to_ns(result->tick);
    result->time_us         = result->time_ns / 1000u;
    result->cycle           = cpu_time_tick_to_cycle(result->tick);
    result->ns_per_pixel    = result->time_ns / PERSPECTIVE_TEST_PIXELS;
    result->cycle_per_pixel = result->cycle / PERSPECTIVE_TEST_PIXELS;
}

/* 遍历 N 次取平均。走 PERSPECTIVE_PIXEL 宏，和 perspective_sum() 的显式外提路径不同，用来交叉验证 */
static inline void perspective_test_run_loop (uint32 loop, perspective_time_result_struct *result)
{
    uint32 sum = 0;
    uint32 total_pixel = 0;

    if ((NULL == result) || (0 == loop))
    {
        return;
    }

    total_pixel = PERSPECTIVE_TEST_PIXELS * loop;

    cpu_time_start();

    for (uint32 n = 0; n < loop; n ++)
    {
        for (uint16 row = 0; row < PERSPECTIVE_TEST_ROW; row ++)
        {
            for (uint16 col = 0; col < PERSPECTIVE_TEST_COL; col ++)
            {
                sum += PERSPECTIVE_PIXEL(row, col);
            }
        }
    }

    result->tick = (uint32)cpu_time_stop();
    perspective_test_sink = sum;

    result->time_ns         = cpu_time_tick_to_ns(result->tick);
    result->time_us         = result->time_ns / 1000u;
    result->cycle           = cpu_time_tick_to_cycle(result->tick);
    result->ns_per_pixel    = result->time_ns / total_pixel;
    result->cycle_per_pixel = result->cycle / total_pixel;
}

#endif /* TIMING_H_ */
