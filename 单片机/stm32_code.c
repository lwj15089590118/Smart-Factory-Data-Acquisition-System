/* ============================================================================
 * Smart-Factory-Data-Acquisition-System
 * 文件名   : stm32_code.c
 * 描述     : STM32F103C8T6 多参数采集节点主程序
 * 功能     : 1) DHT11 温湿度采集
 *            2) ACS712 电流采集(ADC1_IN1) + 分压电阻电压采集(ADC1_IN4)
 *            3) OLED(SSD1306) 本地显示
 *            4) 三按键(MENU/UP/DOWN)阈值设置, 阈值Flash掉电保存
 *            5) 蜂鸣器+继电器声光报警(3次确认+迟滞, 防误报)
 *            6) ESP8266(AT固件) MQTT 上传 JSON 数据, 断线 URC 检测 + 自动重连
 *            7) 下行命令: 订阅 cmd 主题, 支持 set_threshold/mute/reboot 并回执
 *            8) 独立看门狗(约2s) + DHT11 时序超时, 防总线/链路异常挂死
 * 开发环境 : Keil MDK 5.36 + STM32F10x_StdPeriph_Driver V3.5
 * 时钟     : HSE 8MHz -> SYSCLK 72MHz, ADCCLK = 12MHz
 * 作者     : SmartFactory Project   日期: 2026-08
 * ==========================================================================*/
#include "stm32f10x.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------- 用户配置区 ------------------------- */
#define WIFI_SSID       "Factory_WiFi"          /* 现场 WiFi 名称        */
#define WIFI_PASS       "factory@2026"          /* 现场 WiFi 密码        */
#define BROKER_IP       "192.168.1.100"         /* MQTT Broker 地址      */
#define BROKER_PORT     1883
#define MQTT_USER       "sfda"
#define MQTT_PASS       "sfda123"
#define DEVICE_ID       "node1"                 /* 设备编号, 决定主题    */
#define FW_VERSION      "v1.2.0"

/* ------------------------- 引脚定义 ------------------------- */
/* PA0  DHT11 DATA          PA1  ADC1_IN1 ACS712电流
 * PA4  ADC1_IN4 电压分压   PA2/PA3  USART2 -> ESP8266
 * PA9/PA10 USART1 调试串口 PB6/PB7  软件I2C -> OLED
 * PB0  蜂鸣器(高电平响)    PA8  继电器(高电平吸合)
 * PB10 KEY_MENU  PB11 KEY_UP  PB12 KEY_DOWN(低电平按下) */
#define DHT11_PORT      GPIOA
#define DHT11_PIN       GPIO_Pin_0
#define DHT11_LOW()     GPIO_ResetBits(DHT11_PORT, DHT11_PIN)
#define DHT11_HIGH()    GPIO_SetBits(DHT11_PORT, DHT11_PIN)
#define DHT11_READ()    GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN)

#define BUZZER_PORT     GPIOB
#define BUZZER_PIN      GPIO_Pin_0
#define BUZZER_ON()     GPIO_SetBits(BUZZER_PORT, BUZZ_PIN_ALIAS)
#define BUZZER_OFF()    GPIO_ResetBits(BUZZER_PORT, BUZZ_PIN_ALIAS)
#define BUZZ_PIN_ALIAS  BUZZER_PIN

#define RELAY_PORT      GPIOA
#define RELAY_PIN       GPIO_Pin_8
#define RELAY_ON()      GPIO_SetBits(RELAY_PORT, RELAY_PIN)
#define RELAY_OFF()     GPIO_ResetBits(RELAY_PORT, RELAY_PIN)

#define KEY_MENU        0
#define KEY_UP          1
#define KEY_DOWN        2
#define KEY_PORT(k)     GPIOB
#define KEY_PIN(k)      ((k)==KEY_MENU ? GPIO_Pin_10 : ((k)==KEY_UP ? GPIO_Pin_11 : GPIO_Pin_12))

/* OLED 软件 I2C */
#define OLED_SCL_H()    GPIO_SetBits(GPIOB, GPIO_Pin_6)
#define OLED_SCL_L()    GPIO_ResetBits(GPIOB, GPIO_Pin_6)
#define OLED_SDA_H()    GPIO_SetBits(GPIOB, GPIO_Pin_7)
#define OLED_SDA_L()    GPIO_ResetBits(GPIOB, GPIO_Pin_7)
#define OLED_SDA_READ() GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_7)
#define OLED_ADDR       0x78                    /* SSD1306 8位地址 */

/* ------------------------- 数据结构 ------------------------- */
typedef struct {
    float   temp;                              /* 温度 degC            */
    float   humi;                              /* 湿度 %RH             */
    float   current;                           /* 电流 A               */
    float   voltage;                           /* 电压 V               */
    uint8_t alarm;                             /* bit0温度 bit1电流 bit2电压 */
} SensorData_t;

typedef struct {
    float temp_max;                            /* 温度上限      默认40.0 */
    float curr_max;                            /* 电流上限      默认10.0 */
    float volt_max;                            /* 电压上限      默认30.0 */
    float volt_min;                            /* 电压下限      默认20.0 */
} Threshold_t;

/* ------------------------- 全局变量 ------------------------- */
static SensorData_t  g_sensor       = {0};
static Threshold_t   g_th           = {40.0f, 10.0f, 30.0f, 20.0f};
static uint8_t       g_menuMode     = 0;        /* 0运行 1温度上限 2电流上限 3电压上限 4电压下限 */
static uint8_t       g_alarmCnt[3]  = {0};      /* 越限连续确认计数   */
static uint8_t       g_mqttOk       = 0;        /* MQTT 连接状态      */
static uint32_t      g_uptime       = 0;        /* 上电秒数           */
static float         g_iZeroVolt    = 2.5f;     /* 电流通道零点(V), 上电自校准 */
#define ADC_FILT_DEPTH   8                      /* 滑动平均窗口深度(与原设计一致) */
#define ADC_FILT_CH_NUM  2                      /* 参与滤波通道数: 电流/电压各一组 */
static uint16_t g_adcWin[ADC_FILT_CH_NUM][ADC_FILT_DEPTH] = {{0}}; /* 按通道隔离的滑动窗口 */
/* 写索引 uint16_t(第四轮修补, 原 uint8_t): 仅用于 & (DEPTH-1) 取写槽位,
   65536 次采样才回绕, 且 65536 是 8 的整数倍, 回绕前后槽位序列连续、窗口不丢;
   是否播种由独立的 g_winSeeded 标志决定, 与索引值解耦 —— 消除 256 回绕重播种 */
static uint16_t g_winIdx[ADC_FILT_CH_NUM]       = {0};  /* 每通道独立写索引 */
static uint8_t  g_winSeeded[ADC_FILT_CH_NUM]    = {0};  /* 每通道首次播种标志 */
static volatile uint32_t g_msTicks  = 0;        /* 毫秒时基(SysTick中断维护) */

#define VREF            3.300f                  /* ADC 基准电压        */
#define ACS_SENS        0.100f                  /* ACS712-20A 灵敏度 100mV/A */
#define V_DIV_RATIO     22.277f                 /* (100k+4.7k)/4.7k    */
#define FLASH_TH_ADDR   ((uint32_t)0x0800FC00)  /* 最后一页存阈值      */
#define TH_MAGIC        0x5A3C

/* 中文字库不在本文件, ASCII 6x8 字库见 oledfont.h */
extern const unsigned char ASCII6x8[][6];

/* ============================================================
 *                        SysTick 延时与时基
 * 1ms 中断维护 g_msTicks; DelayUs 用计数器现值忙等(<1ms 短延时)
 * ============================================================ */
void SysTick_Handler(void)
{
    g_msTicks++;                                /* 上电毫秒计数 */
}

static void Delay_Init(void)
{
    SysTick->CTRL = 0;
    SysTick->LOAD = 72000 - 1;                  /* 1ms @72MHz */
    SysTick->VAL  = 0;
    SysTick->CTRL = 7;                          /* 处理器时钟+使能+开中断 */
}

static void DelayUs(uint32_t us)
{
    uint32_t start = SysTick->VAL;
    uint32_t ticks = us * 72;                   /* 72 cycles/us */
    uint32_t delta, now;
    do {
        now   = SysTick->VAL;
        delta = (start >= now) ? (start - now)
                               : (start + (SysTick->LOAD + 1) - now);
    } while (delta < ticks);                    /* 仅适用于 us<1000 的短延时 */
}

static void DelayMs(uint32_t ms)
{
    uint32_t start = g_msTicks;
    while ((g_msTicks - start) < ms);
}

/* ============================================================
 *                     OLED 软件位带 I2C
 * ============================================================ */
static void I2C_Delay(void) { volatile uint8_t i = 30; while (i--); }

static void I2C_Start(void)
{
    OLED_SDA_H(); OLED_SCL_H(); I2C_Delay();
    OLED_SDA_L(); I2C_Delay();
    OLED_SCL_L(); I2C_Delay();
}

static void I2C_Stop(void)
{
    OLED_SDA_L(); OLED_SCL_H(); I2C_Delay();
    OLED_SDA_H(); I2C_Delay();
}

static uint8_t I2C_WaitAck(void)
{
    uint8_t ack;
    OLED_SDA_H(); I2C_Delay();
    OLED_SCL_H(); I2C_Delay();
    ack = OLED_SDA_READ();                      /* 1=NACK 0=ACK */
    OLED_SCL_L(); I2C_Delay();
    return ack;
}

static void I2C_SendByte(uint8_t byte)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        if (byte & 0x80) OLED_SDA_H(); else OLED_SDA_L();
        I2C_Delay();
        OLED_SCL_H(); I2C_Delay();
        OLED_SCL_L(); I2C_Delay();
        byte <<= 1;
    }
}

static void OLED_WriteCmd(uint8_t cmd)
{
    I2C_Start();
    I2C_SendByte(OLED_ADDR); I2C_WaitAck();
    I2C_SendByte(0x00);     I2C_WaitAck();      /* Control: Co=0 D/C#=0 */
    I2C_SendByte(cmd);      I2C_WaitAck();
    I2C_Stop();
}

static void OLED_WriteDat(uint8_t dat)
{
    I2C_Start();
    I2C_SendByte(OLED_ADDR); I2C_WaitAck();
    I2C_SendByte(0x40);     I2C_WaitAck();      /* Control: D/C#=1 */
    I2C_SendByte(dat);      I2C_WaitAck();
    I2C_Stop();
}

static void OLED_SetPos(uint8_t page, uint8_t col)
{
    OLED_WriteCmd(0xB0 + page);
    OLED_WriteCmd(0x10 | (col >> 4));
    OLED_WriteCmd(0x00 | (col & 0x0F));
}

static void OLED_GPIO_Init(void)
{
    GPIO_InitTypeDef gi;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    gi.GPIO_Pin   = GPIO_Pin_6 | GPIO_Pin_7;
    gi.GPIO_Mode  = GPIO_Mode_Out_OD;           /* 软件 I2C: 开漏输出, 外部 4.7k 上拉 */
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gi);
    OLED_SCL_H(); OLED_SDA_H();                 /* 总线空闲态 */
}

static void OLED_Init(void)
{
    const uint8_t initCmd[] = {0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,
                               0x8D,0x14,0x20,0x02,0xA1,0xC8,0xDA,0x12,
                               0x81,0xCF,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,
                               0xAF};
    uint8_t i;
    DelayMs(100);                               /* 上电复位等待 */
    for (i = 0; i < sizeof(initCmd); i++) OLED_WriteCmd(initCmd[i]);
}

static void OLED_Clear(void)
{
    uint8_t p, c;
    for (p = 0; p < 8; p++) {
        OLED_SetPos(p, 0);
        for (c = 0; c < 128; c++) OLED_WriteDat(0x00);
    }
}

static void OLED_ShowChar(uint8_t page, uint8_t col, char ch)
{
    uint8_t i, wid = (ch - ' ') * 6;
    OLED_SetPos(page, col);
    for (i = 0; i < 6; i++) OLED_WriteDat(ASCII6x8[wid + i]);
}

static void OLED_ShowString(uint8_t page, uint8_t col, const char *str)
{
    while (*str) { OLED_ShowChar(page, col, *str++); col += 6; }
}

static void OLED_ShowFloat(uint8_t page, uint8_t col, float val, uint8_t dec)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.*f", dec, val);
    OLED_ShowString(page, col, buf);
}

/* ============================================================
 *                     串口1(调试) / 串口2(ESP8266)
 * ============================================================ */
static void USART1_Init(void)
{
    GPIO_InitTypeDef  gi;
    USART_InitTypeDef ui;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1, ENABLE);
    gi.GPIO_Pin   = GPIO_Pin_9; gi.GPIO_Mode = GPIO_Mode_AF_PP;       GPIO_Init(GPIOA, &gi);
    gi.GPIO_Pin   = GPIO_Pin_10; gi.GPIO_Mode = GPIO_Mode_IN_FLOATING; GPIO_Init(GPIOA, &gi);
    ui.USART_BaudRate            = 115200;
    ui.USART_WordLength          = USART_WordLength_8b;
    ui.USART_StopBits            = USART_StopBits_1;
    ui.USART_Parity              = USART_Parity_No;
    ui.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    ui.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART1, &ui);
    USART_Cmd(USART1, ENABLE);
}

static void USART2_Init(void)
{
    GPIO_InitTypeDef  gi;
    USART_InitTypeDef ui;
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gi.GPIO_Pin   = GPIO_Pin_2; gi.GPIO_Mode = GPIO_Mode_AF_PP;        GPIO_Init(GPIOA, &gi);
    gi.GPIO_Pin   = GPIO_Pin_3; gi.GPIO_Mode = GPIO_Mode_IN_FLOATING;  GPIO_Init(GPIOA, &gi);
    ui.USART_BaudRate            = 115200;
    ui.USART_WordLength          = USART_WordLength_8b;
    ui.USART_StopBits            = USART_StopBits_1;
    ui.USART_Parity              = USART_Parity_No;
    ui.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    ui.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &ui);
    USART_Cmd(USART2, ENABLE);
    {
        /* 使能接收中断: 命令下行经 ESP8266 URC 异步到达 */
        NVIC_InitTypeDef ni;
        USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
        ni.NVIC_IRQChannel                   = USART2_IRQn;
        ni.NVIC_IRQChannelPreemptionPriority = 1;
        ni.NVIC_IRQChannelSubPriority        = 1;
        ni.NVIC_IRQChannelCmd                = ENABLE;
        NVIC_Init(&ni);
    }
}

int fputc(int ch, FILE *f)                      /* printf 重定向到串口1 */
{
    USART_SendData(USART1, (uint8_t)ch);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    return ch;
}

static void UART2_SendString(const char *s)
{
    while (*s) {
        USART_SendData(USART2, (uint8_t)*s++);
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
    }
}

/* ---- USART2 接收环形缓冲: 收 ESP8266 URC(+MQTTSUBRECV 等), 主循环轮询解析 ---- */
static volatile char     g_rxBuf[512];
static volatile uint16_t g_rxHead = 0, g_rxTail = 0;
/* 行拼接缓冲(协议解析状态机的半帧状态): ESP_RxPoll 逐字节拼行,
   放文件作用域以便重连回退路径 ESP_RxFlush 连同环形缓冲一起复位 */
static char     g_lineBuf[256];
static uint16_t g_lineLen = 0;

/* 清接收缓冲 + 半帧状态, 供连接回退/重连路径(ESP_MQTT_Init)在 AT+RST 前调用:
   ESP8266 复位后上一会话的残留字节(半帧 URC/未完成的应答)全部作废, 若不清空,
   会与新会话的启动横幅拼接成假行, 污染下一轮解析(第四轮修补项)。
   清理层次: 串口环形缓冲(读写指针归零)与协议解析状态机(半行长度清零)两层同清 ——
   环缓冲内尚未拼成行的字节就是状态机的半帧输入, 二者耦合, 只清一层仍残留半帧。
   双指针复位放在短暂关闭 USART2 中断的临界区内, 避免与 ISR 写 g_rxHead 竞争 */
static void ESP_RxFlush(void)
{
    NVIC_DisableIRQ(USART2_IRQn);
    g_rxHead = 0;
    g_rxTail = 0;
    NVIC_EnableIRQ(USART2_IRQn);
    g_lineLen = 0;                              /* 丢弃拼接中的半帧(仅主循环访问) */
}

void USART2_IRQHandler(void)
{
    uint16_t next;
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE)) {          /* 溢出清标志防中断风暴 */
        (void)USART_ReceiveData(USART2);
    }
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET) {
        next = (uint16_t)((g_rxHead + 1) % sizeof(g_rxBuf));
        if (next != g_rxTail) {
            g_rxBuf[g_rxHead] = (char)USART_ReceiveData(USART2);
            g_rxHead = next;
        } else {
            (void)USART_ReceiveData(USART2);                    /* 缓冲满, 丢弃 */
        }
    }
}

/* ============================================================
 *          独立看门狗 IWDG(约 2s, LSI 40kHz)
 * 用寄存器直写, 不依赖工程是否提供 stm32f10x_iwdg.h;
 * 喂狗点: 主循环末尾(确认处) + 所有 AT 长等待内部(防止重连期间误复位)
 * ============================================================ */
#define IWDG_FEED()     (IWDG->KR = 0xAAAA)     /* 重载计数(喂狗) */

static void IWDG_Init(void)
{
    uint32_t t = 1000000u;                      /* 有界等待, 防 LSI 异常卡死 */
    IWDG->KR  = 0xCCCC;                         /* 启动 IWDG(硬件自动开 LSI) */
    IWDG->KR  = 0x5555;                         /* 解除 PR/RLR 写保护 */
    IWDG->PR  = 0x06;                           /* 分频 64: 40kHz/64 = 625Hz */
    IWDG->RLR = 1250;                           /* 1250/625 = 2s(LSI 有容差) */
    while (((IWDG->SR & 3u) != 0u) && --t);     /* 等 PVU/RVU 同步完成(有界) */
    IWDG_FEED();
}

/* 在 [start, head) 未消费接收区查找 needle(只读不消费, 数据留给 ESP_RxPoll
   统一拼行, 避免吞掉 +MQTTSUBRECV 命令 URC), 找到返回 1 */
static uint8_t ESP_RxScan(const char *needle, uint16_t start)
{
    uint16_t idx = start;
    uint16_t i = 0, len = (uint16_t)strlen(needle);
    while (idx != g_rxHead) {
        char c = g_rxBuf[idx];
        idx = (uint16_t)((idx + 1) % sizeof(g_rxBuf));
        if (c == needle[i]) {
            if (++i == len) return 1;
        } else {
            i = (c == needle[0]) ? 1 : 0;       /* 失配回退到可能的重复首字符 */
        }
    }
    return 0;
}

/* 阻塞等待 needle 出现(期间喂狗), 超时返回 0 */
static uint8_t ESP_RxWaitToken(const char *needle, uint16_t start, uint32_t timeoutMs)
{
    uint32_t start_ms = g_msTicks;
    while ((g_msTicks - start_ms) < timeoutMs) {
        if (ESP_RxScan(needle, start)) return 1;
        IWDG_FEED();
    }
    return 0;
}

/* 发送 AT 指令并校验应答: 收到 OK 返回 1, ERROR/FAIL/超时返回 0
   (应答只认本命令发出之后的字节: base 之前的旧数据不参与匹配,
    且应答数据不在此消费, 统一留给 ESP_RxPoll 拼行) */
static uint8_t ESP_SendAT(const char *cmd, uint32_t waitMs)
{
    char buf[128];
    uint16_t base = g_rxHead;
    uint32_t start = g_msTicks;
    snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    UART2_SendString(buf);
    printf("[AT] %s\r\n", cmd);
    while ((g_msTicks - start) < waitMs) {
        if (ESP_RxScan("\r\nOK\r\n", base)) return 1;
        if (ESP_RxScan("\r\nERROR\r\n", base) ||
            ESP_RxScan("\r\nFAIL\r\n", base)) return 0;
        IWDG_FEED();
    }
    return 0;                                   /* 超时按失败处理, 交上层重试 */
}

/* 发布节点在线状态到 factory/<id>/status, retain=1(协议 §3.3) */
static void MQTT_PublishStatus(const char *state)
{
    char cmd[192];
    if (!g_mqttOk) return;
    /* AT 命令字符串内的双引号须转义为 \" */
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTPUB=0,\"factory/" DEVICE_ID "/status\","
             "\"{\\\"deviceId\\\":\\\"" DEVICE_ID "\\\",\\\"state\\\":\\\"%s\\\","
             "\\\"fw\\\":\\\"" FW_VERSION "\\\"}\",1,1", state);
    ESP_SendAT(cmd, 300);
}

static void ESP_MQTT_Init(void)
{
    char cmd[192];
    uint8_t retry;
    uint16_t base;
    for (retry = 0; retry < 3; retry++) {
        ESP_RxFlush();                          /* 回退路径先清接收缓冲/半帧状态(第四轮修补):
                                                   上一会话或上一轮失败尝试的残留字节随
                                                   ESP 复位作废, 不污染本轮解析 */
        ESP_SendAT("AT+RST", 3000);
        ESP_SendAT("AT+CWMODE=1", 500);
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASS);
        if (!ESP_SendAT(cmd, 8000)) continue;   /* WiFi 入网失败, 直接下一轮重试 */
        ESP_SendAT("AT+MQTTUSERCFG=0,1,\"" DEVICE_ID "\",\"" MQTT_USER "\",\"" MQTT_PASS "\",0,0,\"\"", 1000);
        /* keepalive=30s + 遗嘱 LWT: 异常掉线由 Broker 代发 retained offline(协议 §1/§3.3) */
        snprintf(cmd, sizeof(cmd),
                 "AT+MQTTCONNCFG=0,30,1,\"factory/" DEVICE_ID "/status\","
                 "\"{\\\"deviceId\\\":\\\"" DEVICE_ID "\\\",\\\"state\\\":\\\"offline\\\"}\",1,1");
        ESP_SendAT(cmd, 1000);
        base = g_rxHead;                        /* 应答基准: +MQTTCONNECTED URC 可能紧随 OK 到达 */
        /* ESP-AT v2.x 文档: AT+MQTTCONN=<LinkID>,<"host">,<port>,<reconnect>
           第4参是 reconnect(0/1) 标志而非 keepalive(复审 P2-N3);
           keepalive=30s 已由上方 MQTTCONNCFG 第2参正确配置。
           此处取 0(不依赖 ESP-AT 内部自动重连), 断线重连统一由主循环 30s 状态机负责 */
        snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%d,0", BROKER_IP, BROKER_PORT);
        if (!ESP_SendAT(cmd, 4000)) continue;               /* 命令被拒 */
        if (!ESP_RxWaitToken("+MQTTCONNECTED", base, 5000)) continue;  /* 未连上 Broker */
        g_mqttOk = 1;
        /* 订阅下行命令主题(协议 §2/§3.4) */
        snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=0,\"factory/" DEVICE_ID "/cmd\",1");
        ESP_SendAT(cmd, 1000);
        MQTT_PublishStatus("online");
        printf("[NET] MQTT connected, retry=%d\r\n", retry);
        return;
    }
    g_mqttOk = 0;
    printf("[NET] MQTT connect FAILED, will retry in main loop\r\n");
}

/* 数据/报警上行: JSON 含大量双引号且约 115~140 字节, 超过 AT+MQTTPUB 的 64 字节
   参数上限且转义易错, 改走 AT+MQTTPUBRAW 裸数据模式:
   发命令 -> 等 '>' 提示符 -> 发定长裸数据(引号无需转义) -> 等 +MQTTPUB:OK/FAIL
   注: ESP-AT v2.x 的 MQTTPUBRAW 按声明长度收满即发布, 数据末尾不需要 0x1A
   (0x1A 是 TCP 透传模式的结束符; 若计入长度会混入 payload 破坏 JSON) */
#define MQTT_PUB_MAX_LEN 192                    /* 上行 payload 长度上限(与调用方缓冲一致) */
static uint8_t MQTT_Publish(const char *topic, const char *payload)
{
    char cmd[96];
    uint16_t base;
    uint32_t len, start;
    if (!g_mqttOk) return 0;
    len = (uint32_t)strlen(payload);
    if (len == 0 || len > MQTT_PUB_MAX_LEN) return 0;   /* 长度显式检查, 防缓冲区溢出 */
    base = g_rxHead;                            /* 应答基准: '>' 紧随 OK, 两次等待须同源 */
    snprintf(cmd, sizeof(cmd), "AT+MQTTPUBRAW=0,\"%s\",%lu,1,0",
             topic, (unsigned long)len);
    UART2_SendString(cmd);
    UART2_SendString("\r\n");
    printf("[AT] %s\r\n", cmd);
    start = g_msTicks;
    while ((g_msTicks - start) < 1000u) {       /* 等命令应答 OK */
        if (ESP_RxScan("\r\nOK\r\n", base)) break;
        if (ESP_RxScan("\r\nERROR\r\n", base) ||
            ESP_RxScan("\r\nFAIL\r\n", base)) {
            g_mqttOk = 0;                       /* 命令被拒也标记重连, 与 '>' 超时/
                                                   +MQTTPUB:FAIL 失败路径对齐(复审 P2-N1),
                                                   消除 URC 丢失时的静默失败窗口 */
            printf("[NET] MQTTPUBRAW rejected, mark for reconnect\r\n");
            return 0;
        }
        IWDG_FEED();
    }
    /* '>' 提示符紧随 OK(间隔约 1 个字节时间 87µs@115200), 必须阻塞等待:
       单次 ESP_RxScan 会在 OK 末字节入环形缓冲后几十µs内退出, 大概率漏检 '>'
       导致每次发布误判失败而整轮重连(复审 P1-N1);
       ESP_RxWaitToken 内部循环扫描并持续喂狗, 500ms 超时覆盖串口时序抖动 */
    if (!ESP_RxWaitToken(">", base, 500)) {     /* 阻塞等 '>' 提示符进入数据模式 */
        g_mqttOk = 0;                           /* 视为链路异常, 交主循环 30s 重连 */
        printf("[NET] MQTTPUBRAW no '>' prompt\r\n");
        return 0;
    }
    UART2_SendString(payload);
    if (!ESP_RxWaitToken("+MQTTPUB:OK", base, 3000)) {
        g_mqttOk = 0;
        printf("[NET] publish FAIL, mark for reconnect\r\n");
        return 0;
    }
    return 1;
}

/* ============================================================
 *                        ADC 采集与滤波
 * ============================================================ */
static void ADC1_Init(void)
{
    GPIO_InitTypeDef gi;
    ADC_InitTypeDef  ai;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);           /* 72/6=12MHz */
    gi.GPIO_Pin  = GPIO_Pin_1 | GPIO_Pin_4;
    gi.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &gi);
    ADC_DeInit(ADC1);
    ai.ADC_Mode               = ADC_Mode_Independent;
    ai.ADC_ScanConvMode       = DISABLE;
    ai.ADC_ContinuousConvMode = DISABLE;
    ai.ADC_ExternalTrigConv   = ADC_ExternalTrigConv_None;
    ai.ADC_DataAlign          = ADC_DataAlign_Right;
    ai.ADC_NbrOfChannel       = 1;
    ADC_Init(ADC1, &ai);
    ADC_Cmd(ADC1, ENABLE);
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1));
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1));
}

static uint16_t ADC_ReadCh(uint8_t ch)
{
    ADC_RegularChannelConfig(ADC1, ch, 1, ADC_SampleTime_239Cycles5);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC));
    return ADC_GetConversionValue(ADC1);
}

/* 通道号 -> 滤波窗口槽位: 电流(ADC_Channel_1)与电压(ADC_Channel_4)各自独立一组窗口,
   避免两路交替采样共用同一窗口导致读数互相串扰 */
static uint8_t ADC_FiltSlot(uint8_t ch)
{
    return (uint8_t)((ch == ADC_Channel_4) ? 1 : 0);
}

/* 采集5次取中值, 再做8点滑动平均 —— 解决现场数据跳变(见调试记录Day4) */
static uint16_t ADC_ReadFiltered(uint8_t ch)
{
    uint16_t s[5];
    uint8_t  i, j, slot;
    uint32_t sum = 0;
    slot = ADC_FiltSlot(ch);
    for (i = 0; i < 5; i++) { s[i] = ADC_ReadCh(ch); DelayUs(200); }
    for (i = 0; i < 4; i++)                     /* 冒泡排序取中值 */
        for (j = 0; j < 4 - i; j++)
            if (s[j] > s[j+1]) { uint16_t t = s[j]; s[j] = s[j+1]; s[j+1] = t; }
    g_adcWin[slot][g_winIdx[slot] & (ADC_FILT_DEPTH - 1)] = s[2];
    if (!g_winSeeded[slot]) {                   /* 首次采样播种整窗, 避免开机曲线凹陷。
                                                   原以 uint8_t idx==0 判定, 每通道 256 次
                                                   采样(2s 周期约 8.5min)回绕到 0 会整窗
                                                   重播种, 滤波记忆周期性清零(复审 P2-N2);
                                                   改为独立标志仅上电播种一次, 稳态下输出
                                                   与原实现逐次等价, 仅消除周期性失忆 */
        uint8_t n;
        for (n = 0; n < ADC_FILT_DEPTH; n++) g_adcWin[slot][n] = s[2];
        g_winSeeded[slot] = 1;
    }
    g_winIdx[slot]++;
    for (i = 0; i < ADC_FILT_DEPTH; i++) sum += g_adcWin[slot][i];
    return (uint16_t)(sum >> 3);
}

static void Current_ZeroCal(void)               /* 上电空载零点校准 */
{
    uint32_t sum = 0;
    uint8_t  i;
    printf("[CAL] current zero calibrating, keep load OFF...\r\n");
    for (i = 0; i < 64; i++) sum += ADC_ReadCh(ADC_Channel_1);
    g_iZeroVolt = sum / 64.0f * VREF / 4095.0f;   /* 浮点均值, 避免整除截断误差 */
    printf("[CAL] zero point = %.3f V\r\n", g_iZeroVolt);
}

/* ============================================================
 *                           DHT11
 * ============================================================ */
static void DHT11_ModeOut(void)
{
    GPIO_InitTypeDef gi;
    gi.GPIO_Pin  = DHT11_PIN;
    gi.GPIO_Mode = GPIO_Mode_Out_OD;            /* 开漏+上拉, 可直接读 */
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(DHT11_PORT, &gi);
}

static void DHT11_Init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    DHT11_ModeOut();
    DHT11_HIGH();
}

/* 等待数据线变为指定电平(RESET/SET), 基于 SysTick 计数约 200us 超时:
   返回 0=等到, 1=超时。防止数据线受扰钳死导致 while 死循环挂死主循环 */
static uint8_t DHT11_Wait(uint8_t level)
{
    uint32_t start = SysTick->VAL;
    uint32_t ticks = 200 * 72;                  /* 200us @72MHz */
    uint32_t delta, now;
    do {
        if (DHT11_READ() == level) return 0;
        now   = SysTick->VAL;
        delta = (start >= now) ? (start - now)
                               : (start + (SysTick->LOAD + 1) - now);
    } while (delta < ticks);
    return 1;
}

static uint8_t DHT11_ReadByte(uint8_t *out)     /* 返回 0 成功, 1 等待超时 */
{
    uint8_t i, byte = 0;
    for (i = 0; i < 8; i++) {
        if (DHT11_Wait(SET)) return 1;          /* 等 50us 低电平结束(线变高) */
        DelayUs(40);                            /* 40us 后判断电平 */
        byte <<= 1;
        if (DHT11_READ() != RESET) {
            byte |= 1;
            if (DHT11_Wait(RESET)) return 1;    /* 等高电平结束 */
        }
    }
    *out = byte;
    return 0;
}

static uint8_t DHT11_Read(float *temp, float *humi)
{
    uint8_t buf[5], i;
    DHT11_LOW();  DelayMs(20);                  /* 起始信号 >18ms */
    DHT11_HIGH(); DelayUs(30);
    if (DHT11_READ() != RESET) return 1;        /* 无应答 */
    if (DHT11_Wait(SET)) return 3;              /* 83us 低, 超时 */
    if (DHT11_Wait(RESET)) return 3;            /* 87us 高, 超时 */
    for (i = 0; i < 5; i++)
        if (DHT11_ReadByte(&buf[i])) return 3;  /* 位等待超时 */
    if ((uint8_t)(buf[0]+buf[1]+buf[2]+buf[3]) != buf[4]) return 2;
    *humi = buf[0] + buf[1] * 0.1f;
    *temp = buf[2] + buf[3] * 0.1f;
    return 0;
}

/* ============================================================
 *                    按键 / 蜂鸣器 / 继电器
 * ============================================================ */
static void KEY_BEEP_Init(void)
{
    GPIO_InitTypeDef gi;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);
    gi.GPIO_Mode  = GPIO_Mode_IPU;              /* 内部上拉, 按下接地 */
    gi.GPIO_Speed = GPIO_Speed_10MHz;
    gi.GPIO_Pin   = GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
    GPIO_Init(GPIOB, &gi);
    gi.GPIO_Mode = GPIO_Mode_Out_PP;
    gi.GPIO_Pin  = BUZZER_PIN;  GPIO_Init(BUZZER_PORT, &gi);
    gi.GPIO_Pin  = RELAY_PIN;   GPIO_Init(RELAY_PORT,  &gi);
    BUZZER_OFF();
    RELAY_OFF();
}

/* 20ms 消抖扫描, 返回 0xFF 表示无键, 长按(>1s)返回 KEY|0x80 */
static uint8_t KEY_Scan(void)
{
    static uint8_t  lastKey = 0xFF, stable = 0xFF, holdCnt = 0;
    static uint32_t lastScan = 0;
    uint8_t k, cur = 0xFF, i;
    if ((g_msTicks - lastScan) < 20) return 0xFF;   /* 20ms 节流 */
    lastScan = g_msTicks;
    for (i = 0; i < 3; i++)
        if (GPIO_ReadInputDataBit(KEY_PORT(i), KEY_PIN(i)) == RESET) { cur = i; break; }
    if (cur != lastKey) { lastKey = cur; holdCnt = 0; return 0xFF; } /* 抖动期 */
    if (cur == 0xFF) { stable = 0xFF; holdCnt = 0; return 0xFF; }
    if (cur != stable) { stable = cur; return cur; }        /* 短按 */
    holdCnt++;
    if (holdCnt == 50) return cur | 0x80;       /* 1s 长按 */
    return 0xFF;
}

static void Buzzer_Beep(uint16_t ms) { BUZZER_ON(); DelayMs(ms); BUZZER_OFF(); }

/* ============================================================
 *                     阈值 Flash 读写
 * ============================================================ */
static void TH_Save(void)
{
    uint16_t d[5] = {TH_MAGIC,
                     (uint16_t)(g_th.temp_max * 10),
                     (uint16_t)(g_th.curr_max * 10),
                     (uint16_t)(g_th.volt_max * 10),
                     (uint16_t)(g_th.volt_min * 10)};
    uint8_t i;
    FLASH_Unlock();
    FLASH_ErasePage(FLASH_TH_ADDR);
    for (i = 0; i < 5; i++)
        FLASH_ProgramHalfWord(FLASH_TH_ADDR + i * 2, d[i]);
    FLASH_Lock();
    printf("[TH] thresholds saved to flash\r\n");
}

static void TH_Load(void)
{
    uint16_t *p = (uint16_t *)FLASH_TH_ADDR;
    if (p[0] != TH_MAGIC) { printf("[TH] flash empty, use default\r\n"); return; }
    g_th.temp_max = p[1] / 10.0f;
    g_th.curr_max = p[2] / 10.0f;
    g_th.volt_max = p[3] / 10.0f;
    g_th.volt_min = p[4] / 10.0f;
    printf("[TH] loaded: T=%.1f I=%.1f U=%.1f/%.1f\r\n",
           g_th.temp_max, g_th.curr_max, g_th.volt_max, g_th.volt_min);
}

/* ============================================================
 *                  下行命令处理(协议 §3.4)
 * 看板经 factory/<id>/cmd 下发 JSON, ESP8266 以 +MQTTSUBRECV
 * URC 上抛; 此处做最小字符串解析: set_threshold/mute/reboot
 * ============================================================ */
#define CMD_NUM_MISSING  (-1000.0f)

/* 在 URC 行中提取 \"<key>\":<number> 的数值, 不存在返回 CMD_NUM_MISSING */
static float Cmd_ParseNum(const char *line, const char *key)
{
    char pat[32];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(line, pat);
    if (p == NULL) return CMD_NUM_MISSING;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return (float)strtod(p, NULL);
}

static long Cmd_ParseId(const char *line)
{
    const char *p = strstr(line, "\"id\"");
    if (p == NULL) return 0;
    p += 4;
    while (*p == ' ' || *p == ':') p++;
    return strtol(p, NULL, 10);
}

static void Cmd_Resp(long id, int result, const char *msg)
{
    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"id\":%ld,\"result\":%d,\"msg\":\"%s\"}", id, result, msg);
    MQTT_Publish("factory/" DEVICE_ID "/cmd_resp", payload);
}

static void Cmd_Handle(const char *line)
{
    long id = Cmd_ParseId(line);
    float v;

    if (strstr(line, "set_threshold")) {
        uint8_t hit = 0;
        v = Cmd_ParseNum(line, "temp_max");
        if (v > CMD_NUM_MISSING && v >= 10 && v <= 200) { g_th.temp_max = v; hit = 1; }
        v = Cmd_ParseNum(line, "curr_max");
        if (v > CMD_NUM_MISSING && v >= 1 && v <= 100) { g_th.curr_max = v; hit = 1; }
        v = Cmd_ParseNum(line, "volt_max");
        if (v > CMD_NUM_MISSING && v >= 5 && v <= 1000) { g_th.volt_max = v; hit = 1; }
        v = Cmd_ParseNum(line, "volt_min");
        if (v > CMD_NUM_MISSING && v >= 1 &&
            v <= 1000 && v < g_th.volt_max) { g_th.volt_min = v; hit = 1; }
        if (hit) {
            TH_Save();                          /* 与按键设置一致: 写 Flash 掉电保存 */
            Cmd_Resp(id, 0, "ok");
            printf("[CMD] set_threshold applied\r\n");
        } else {
            Cmd_Resp(id, 1, "param");
            printf("[CMD] set_threshold rejected\r\n");
        }
    } else if (strstr(line, "mute")) {          /* 蜂鸣器消音 */
        BUZZER_OFF();
        Cmd_Resp(id, 0, "ok");
        printf("[CMD] muted\r\n");
    } else if (strstr(line, "reboot")) {        /* 软复位 */
        Cmd_Resp(id, 0, "ok");
        printf("[CMD] rebooting...\r\n");
        DelayMs(200);                           /* 给 UART 发送留时间 */
        NVIC_SystemReset();
    } else {
        Cmd_Resp(id, 1, "unknown");
        printf("[CMD] unknown type\r\n");
    }
}

#define CMD_URC_PREFIX   "+MQTTSUBRECV:"
#define CMD_TOPIC_SUFFIX "/cmd"

/* 主循环轮询: 从环形缓冲拼行, 命中 cmd 主题的 URC 交命令处理 */
static void ESP_RxPoll(void)
{
    /* 行缓冲 g_lineBuf/g_lineLen 为文件作用域静态(见环形缓冲定义处):
       重连回退路径 ESP_RxFlush 需复位半帧状态(第四轮修补) */
    while (g_rxTail != g_rxHead) {
        char c = g_rxBuf[g_rxTail];
        g_rxTail = (uint16_t)((g_rxTail + 1) % sizeof(g_rxBuf));
        if (c == '\n') {
            g_lineBuf[g_lineLen] = '\0';
            /* 断线 URC: WiFi 掉网或 MQTT 断开, 清连接标志使主循环 30s 重连生效 */
            if (strstr(g_lineBuf, "WIFI DISCONNECT") != NULL ||
                strstr(g_lineBuf, "+MQTTDISCONNECTED") != NULL) {
                if (g_mqttOk) {
                    g_mqttOk = 0;
                    printf("[NET] disconnect URC, will reconnect\r\n");
                }
            }
            if (strstr(g_lineBuf, CMD_URC_PREFIX) != NULL &&
                strstr(g_lineBuf, "factory/" DEVICE_ID CMD_TOPIC_SUFFIX) != NULL) {
                Cmd_Handle(g_lineBuf);
            }
            g_lineLen = 0;
        } else if (c != '\r') {
            if (g_lineLen < sizeof(g_lineBuf) - 1) {
                g_lineBuf[g_lineLen++] = c;
            } else {
                g_lineLen = 0;                  /* 超长行丢弃重来 */
            }
        }
    }
}

/* ============================================================
 *                       报警判定(防误报)
 * 规则: 连续3个采样周期越限才报警; 恢复条件加入5%迟滞回差
 * ============================================================ */
static void Alarm_Check(void)
{
    const float hys = 0.05f;
    uint8_t prev = g_sensor.alarm;
    /* 温度 */
    if (g_sensor.temp > g_th.temp_max) { if (g_alarmCnt[0] < 3) g_alarmCnt[0]++; }
    else if (g_sensor.temp < g_th.temp_max * (1 - hys)) g_alarmCnt[0] = 0;
    /* 电流 */
    if (g_sensor.current > g_th.curr_max) { if (g_alarmCnt[1] < 3) g_alarmCnt[1]++; }
    else if (g_sensor.current < g_th.curr_max * (1 - hys)) g_alarmCnt[1] = 0;
    /* 电压 */
    if (g_sensor.voltage > g_th.volt_max || g_sensor.voltage < g_th.volt_min) {
        if (g_alarmCnt[2] < 3) g_alarmCnt[2]++;
    } else if (g_sensor.voltage < g_th.volt_max * (1 - hys) &&
               g_sensor.voltage > g_th.volt_min * (1 + hys)) g_alarmCnt[2] = 0;

    g_sensor.alarm = 0;
    if (g_alarmCnt[0] >= 3) g_sensor.alarm |= 1;
    if (g_alarmCnt[1] >= 3) g_sensor.alarm |= 2;
    if (g_alarmCnt[2] >= 3) g_sensor.alarm |= 4;

    /* 继电器联锁仅电流持续越限(bit1)时吸合(调试记录 Day11 整改),
       温度/电压报警只声光提示不切负载 */
    if (g_sensor.alarm & 2) RELAY_ON();
    else                    RELAY_OFF();

    if (g_sensor.alarm && !prev) {              /* 报警触发沿 */
        char payload[160];
        BUZZER_ON();                            /* 蜂鸣器对任意报警持续鸣响 */
        snprintf(payload, sizeof(payload),
                 "{\"deviceId\":\"" DEVICE_ID "\",\"source\":\"device\",\"alarm\":%d,"
                 "\"temp\":%.1f,\"current\":%.2f,\"voltage\":%.1f,\"uptime\":%lu}",
                 g_sensor.alarm, g_sensor.temp, g_sensor.current, g_sensor.voltage,
                 (unsigned long)g_uptime);
        MQTT_Publish("factory/" DEVICE_ID "/alarm", payload);
        printf("[ALM] ALARM! code=%d\r\n", g_sensor.alarm);
    } else if (!g_sensor.alarm && prev) {       /* 报警恢复沿 */
        BUZZER_OFF();
        printf("[ALM] alarm cleared\r\n");
    }
}

/* ============================================================
 *                       OLED 界面刷新
 * ============================================================ */
static void OLED_ShowMain(void)
{
    char line[24];
    OLED_ShowString(0,  0, "SmartFactory " FW_VERSION);
    snprintf(line, sizeof(line), "T:%.1fC H:%.0f%%",
             g_sensor.temp, g_sensor.humi);
    OLED_ShowString(2, 0, line);
    snprintf(line, sizeof(line), "I:%d.%02dA U:%d.%dV",
             (int)g_sensor.current, (int)(g_sensor.current * 100) % 100,
             (int)g_sensor.voltage, (int)(g_sensor.voltage * 10) % 10);
    OLED_ShowString(3, 0, line);
    snprintf(line, sizeof(line), "NET:%s RUN:%lus", g_mqttOk ? "OK" : "--", g_uptime);
    OLED_ShowString(5, 0, line);
    if (g_menuMode == 0) {
        OLED_ShowString(6, 0, g_sensor.alarm ? "** ALARM **" : "System Normal ");
    } else {                                    /* 阈值设置界面 */
        const char *names[5] = {"", "TempMax ", "CurrMax ", "VoltMax ", "VoltMin "};
        snprintf(line, sizeof(line), "SET %s=%.1f", names[g_menuMode],
                 g_menuMode == 1 ? g_th.temp_max : g_menuMode == 2 ? g_th.curr_max :
                 g_menuMode == 3 ? g_th.volt_max : g_th.volt_min);
        OLED_ShowString(6, 0, line);
    }
}

/* ============================================================
 *                     按键菜单状态机
 * ============================================================ */
static void Key_Handle(uint8_t key)
{
    if (key == 0xFF) return;
    if ((key & 0x7F) == KEY_MENU) {             /* MENU: 切换/保存 */
        if (g_menuMode >= 4) { g_menuMode = 0; TH_Save(); Buzzer_Beep(100); }
        else { g_menuMode++; }
    } else if (g_menuMode != 0) {               /* 上下键: 调整当前阈值 */
        float *p = (g_menuMode == 1) ? &g_th.temp_max :
                   (g_menuMode == 2) ? &g_th.curr_max :
                   (g_menuMode == 3) ? &g_th.volt_max : &g_th.volt_min;
        float step = (key & 0x80) ? 1.0f : 0.1f;   /* 长按大步进 */
        *p += ((key & 0x7F) == KEY_UP) ? step : -step;
        if (g_menuMode == 1 && *p < 10) *p = 10;
        if (g_menuMode == 2 && *p < 1)  *p = 1;
        if (g_menuMode == 3 && *p < 5)  *p = 5;
        if (g_menuMode == 4 && *p < 1)  *p = 1;
    } else if (((key & 0x7F) == KEY_UP) && (key & 0x80)) {  /* 运行态长按UP: 手动消音 */
        BUZZER_OFF();
    }
}

/* ============================================================
 *                        JSON 上传
 * ============================================================ */
static void UploadData(void)
{
    char payload[192];
    snprintf(payload, sizeof(payload),
        "{\"deviceId\":\"" DEVICE_ID "\",\"temp\":%.1f,\"humi\":%.1f,"
        "\"current\":%.2f,\"voltage\":%.1f,\"alarm\":%d,\"uptime\":%lu,"
        "\"fw\":\"" FW_VERSION "\"}",
        g_sensor.temp, g_sensor.humi, g_sensor.current,
        g_sensor.voltage, g_sensor.alarm, g_uptime);
    MQTT_Publish("factory/" DEVICE_ID "/data", payload);
    printf("[TX] %s\r\n", payload);             /* 串口1同步打印 */
}

/* ============================================================
 *                             main
 * ============================================================ */
int main(void)
{
    uint32_t lastSec = 0, lastUpload = 0, lastOled = 0, lastRetry = 0;
    uint16_t adcI, adcU;
    uint8_t  dhtErrCnt = 0;
    float    t, h;

    SystemInit();
    Delay_Init();                               /* 先建 1ms SysTick 时基 */
    USART1_Init();
    USART2_Init();
    KEY_BEEP_Init();
    DHT11_Init();
    ADC1_Init();
    OLED_GPIO_Init();
    OLED_Init();
    OLED_Clear();
    OLED_ShowString(2, 0, "Booting...");
    printf("\r\n===== SmartFactory Node " FW_VERSION " =====\r\n");
    Buzzer_Beep(50);                            /* 上电自检提示音 */
    TH_Load();
    Current_ZeroCal();
    IWDG_Init();                                /* 使能独立看门狗(约2s): AT长等待与主循环处喂狗 */
    ESP_MQTT_Init();
    OLED_Clear();

    while (1) {
        uint32_t nowSec = g_msTicks / 1000u;    /* 上电秒数 */
        g_uptime = nowSec;

        ESP_RxPoll();                           /* 处理下行命令 URC */

        if (nowSec != lastOled) { lastOled = nowSec; OLED_ShowMain(); }   /* 1s 刷屏 */

        if (nowSec - lastSec >= 2) {            /* 2s 采样 */
            lastSec = nowSec;
            adcI = ADC_ReadFiltered(ADC_Channel_1);
            adcU = ADC_ReadFiltered(ADC_Channel_4);
            g_sensor.current = (adcI * VREF / 4095.0f - g_iZeroVolt) / ACS_SENS;
            g_sensor.voltage = adcU * VREF / 4095.0f * V_DIV_RATIO;
            if (DHT11_Read(&t, &h) == 0) { dhtErrCnt = 0; g_sensor.temp = t; g_sensor.humi = h; }
            else if (++dhtErrCnt > 5) { g_sensor.temp = -99.9f; g_sensor.humi = -1; }  /* 传感器故障 */
            Alarm_Check();
        }

        if (g_mqttOk && (nowSec - lastUpload >= 5)) {                     /* 5s 上传 */
            lastUpload = nowSec;
            UploadData();
        }
        if (!g_mqttOk && (nowSec - lastRetry >= 30)) {                    /* 30s 重连 */
            lastRetry = nowSec;
            ESP_MQTT_Init();
        }

        Key_Handle(KEY_Scan());
        IWDG_FEED();                            /* 主循环确认处喂狗: 采样/报警/上传/按键均已完成 */
    }
}
