/* ============================================================================
 * Smart-Factory-Data-Acquisition-System
 * 文件名   : stm32_code.c
 * 描述     : STM32F103C8T6 多参数采集节点主程序
 * 功能     : 1) DHT11 温湿度采集
 *            2) ACS712 电流采集(ADC1_IN1) + 分压电阻电压采集(ADC1_IN4)
 *            3) OLED(SSD1306) 本地显示
 *            4) 三按键(MENU/UP/DOWN)阈值设置, 阈值Flash掉电保存
 *            5) 蜂鸣器+继电器声光报警(3次确认+迟滞, 防误报)
 *            6) ESP8266(AT固件) MQTT 上传 JSON 数据, 断线自动重连
 *            7) 下行命令: 订阅 cmd 主题, 支持 set_threshold/mute/reboot 并回执
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
static uint16_t      g_adcWin[8]    = {0};      /* 滑动平均窗口       */
static uint8_t       g_winIdx       = 0;
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

/* 发送 AT 指令并粗等待应答(简化版: 延时法, 完整工程可用环形缓冲+匹配OK) */
static uint8_t ESP_SendAT(const char *cmd, uint32_t waitMs)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    UART2_SendString(buf);
    printf("[AT] %s\r\n", cmd);
    DelayMs(waitMs);
    return 1;
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
    for (retry = 0; retry < 3; retry++) {
        ESP_SendAT("AT+RST", 3000);
        ESP_SendAT("AT+CWMODE=1", 500);
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASS);
        if (ESP_SendAT(cmd, 8000)) {
            ESP_SendAT("AT+MQTTUSERCFG=0,1,\"" DEVICE_ID "\",\"" MQTT_USER "\",\"" MQTT_PASS "\",0,0,\"\"", 1000);
            /* keepalive=30s + 遗嘱 LWT: 异常掉线由 Broker 代发 retained offline(协议 §1/§3.3) */
            snprintf(cmd, sizeof(cmd),
                     "AT+MQTTCONNCFG=0,30,1,\"factory/" DEVICE_ID "/status\","
                     "\"{\\\"deviceId\\\":\\\"" DEVICE_ID "\\\",\\\"state\\\":\\\"offline\\\"}\",1,1");
            ESP_SendAT(cmd, 1000);
            snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%d,30", BROKER_IP, BROKER_PORT);
            ESP_SendAT(cmd, 4000);
            g_mqttOk = 1;
            /* 订阅下行命令主题(协议 §2/§3.4) */
            snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=0,\"factory/" DEVICE_ID "/cmd\",1");
            ESP_SendAT(cmd, 1000);
            MQTT_PublishStatus("online");
            printf("[NET] MQTT connected, retry=%d\r\n", retry);
            return;
        }
    }
    g_mqttOk = 0;
    printf("[NET] MQTT connect FAILED, will retry in main loop\r\n");
}

static void MQTT_Publish(const char *topic, const char *payload)
{
    char cmd[300];
    if (!g_mqttOk) return;
    snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=0,\"%s\",\"%s\",1,0", topic, payload);
    ESP_SendAT(cmd, 300);
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

/* 采集5次取中值, 再做8点滑动平均 —— 解决现场数据跳变(见调试记录Day4) */
static uint16_t ADC_ReadFiltered(uint8_t ch)
{
    uint16_t s[5];
    uint8_t  i, j;
    uint32_t sum = 0;
    for (i = 0; i < 5; i++) { s[i] = ADC_ReadCh(ch); DelayUs(200); }
    for (i = 0; i < 4; i++)                     /* 冒泡排序取中值 */
        for (j = 0; j < 4 - i; j++)
            if (s[j] > s[j+1]) { uint16_t t = s[j]; s[j] = s[j+1]; s[j+1] = t; }
    g_adcWin[g_winIdx & 7] = s[2];
    if (g_winIdx == 0) {                        /* 首次采样播种整窗, 避免开机曲线凹陷 */
        uint8_t n;
        for (n = 0; n < 8; n++) g_adcWin[n] = s[2];
    }
    g_winIdx++;
    for (i = 0; i < 8; i++) sum += g_adcWin[i];
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

static uint8_t DHT11_ReadByte(void)
{
    uint8_t i, byte = 0;
    for (i = 0; i < 8; i++) {
        while (DHT11_READ() == RESET);          /* 等 50us 低电平结束 */
        DelayUs(40);                            /* 40us 后判断电平 */
        byte <<= 1;
        if (DHT11_READ() != RESET) { byte |= 1; while (DHT11_READ() != RESET); }
    }
    return byte;
}

static uint8_t DHT11_Read(float *temp, float *humi)
{
    uint8_t buf[5];
    DHT11_LOW();  DelayMs(20);                  /* 起始信号 >18ms */
    DHT11_HIGH(); DelayUs(30);
    if (DHT11_READ() != RESET) return 1;        /* 无应答 */
    while (DHT11_READ() == RESET);              /* 83us 低 */
    while (DHT11_READ() != RESET);              /* 87us 高 */
    buf[0] = DHT11_ReadByte();                  /* 湿度整数 */
    buf[1] = DHT11_ReadByte();                  /* 湿度小数 */
    buf[2] = DHT11_ReadByte();                  /* 温度整数 */
    buf[3] = DHT11_ReadByte();                  /* 温度小数 */
    buf[4] = DHT11_ReadByte();                  /* 校验和 */
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
    static char     g_lineBuf[256];
    static uint16_t g_lineLen = 0;

    while (g_rxTail != g_rxHead) {
        char c = g_rxBuf[g_rxTail];
        g_rxTail = (uint16_t)((g_rxTail + 1) % sizeof(g_rxBuf));
        if (c == '\n') {
            g_lineBuf[g_lineLen] = '\0';
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
        char payload[128];
        BUZZER_ON();                            /* 蜂鸣器对任意报警持续鸣响 */
        snprintf(payload, sizeof(payload),
                 "{\"deviceId\":\"" DEVICE_ID "\",\"alarm\":%d,\"temp\":%.1f,\"current\":%.2f,\"voltage\":%.1f,\"uptime\":%lu}",
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
    }
}
