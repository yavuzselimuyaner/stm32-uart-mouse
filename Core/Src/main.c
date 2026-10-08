/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* Tık butonları için durum bilgisi (debounce ve basma/bırakma anını yakalamak için) */
typedef struct {
  GPIO_TypeDef *port;   /* butonun portu (GPIOB, GPIOC...) */
  uint16_t      pin;    /* butonun pini */
  char          ad;     /* 'L' = sol tık, 'R' = sağ tık */
  uint8_t       durum;  /* kabul edilmiş son durum: 1 = bırakık, 0 = basılı */
  uint8_t       sayac;  /* yeni değer üst üste kaç kez okundu */
} Tik;

/* Halka tampon: kesme gelen karakterleri sona yazar, ana döngü baştan okur */
#define RB_BOYUT 64
typedef struct {
  uint8_t           veri[RB_BOYUT];
  volatile uint16_t yaz;   /* bir sonraki karakter nereye yazılacak (kesme ilerletir) */
  volatile uint16_t oku;   /* bir sonraki karakter nereden okunacak (ana döngü ilerletir) */
} HalkaTampon;

/* Menüden değiştirilebilen bir ayar */
typedef struct {
  const char *ad;      /* menüde görünen ad */
  int        *deger;   /* ayarın tutulduğu değişkenin adresi */
  int         min;     /* izin verilen en küçük değer */
  int         max;     /* izin verilen en büyük değer */
} Ayar;

/* Menünün iki durumu (durum makinesi) */
typedef enum { SECIM_BEKLE, DEGER_BEKLE } MenuDurum;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SATIR_BOYUT 32   /* bilgisayardan gelen bir satırın en fazla uzunluğu */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
volatile uint32_t tick10ms = 0;   /* TIM2 kesmesi her 10 ms'de 1 artırır */

/* Hız ayarları: artık sabit değil, menüden değiştirilebilen değişkenler */
int hiz_baslangic = 2;    /* yön butonuna basınca ilk hız: her 10 ms'de kaç piksel */
int hiz_max       = 12;   /* en yüksek hız */
int hizlanma_tick = 10;   /* basılı tuttukça her kaç tick'te (10 ms) hız 1 artsın */

static Ayar ayarlar[] = {
  { "En yuksek hiz",   &hiz_max,       1, 30 },
  { "Baslangic hizi",  &hiz_baslangic, 1, 10 },
  { "Hizlanma (tick)", &hizlanma_tick, 1, 50 },
};
#define AYAR_SAYISI ((int)(sizeof(ayarlar) / sizeof(ayarlar[0])))

HalkaTampon rx_tampon;    /* UART'tan gelen karakterler */
uint8_t     rx_bayt;      /* HAL'ın kesmeyle aldığı tek karakter buraya gelir */

static MenuDurum menu_durum  = SECIM_BEKLE;
static int       secili_ayar = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
int __io_putchar(int sayi)
{
  HAL_UART_Transmit(&huart2, (uint8_t *)&sayi, 1, HAL_MAX_DELAY);
  return sayi;
}

/* Buton basılı mı? Pull-up olduğu için basılıyken pin 0 okunur. */
static int basili(GPIO_TypeDef *port, uint16_t pin)
{
  return HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET;
}

/* Tık butonunu her 10 ms'de bir kontrol eder.
 * Yeni değer 2 kez üst üste (20 ms) okunursa gerçek bir değişim sayılır (debounce).
 * Değişim olunca bilgisayara "B L 1" (sol tık basıldı) gibi bir satır gönderir. */
static void tik_guncelle(Tik *t)
{
  uint8_t okunan = HAL_GPIO_ReadPin(t->port, t->pin);

  if (okunan == t->durum)
  {
    t->sayac = 0;          /* değişiklik yok */
    return;
  }
  if (++t->sayac >= 2)
  {
    t->durum = okunan;
    t->sayac = 0;
    printf("B %c %d\r\n", t->ad, (t->durum == GPIO_PIN_RESET) ? 1 : 0);
  }
}

/* ---------------- Halka tampon ---------------- */

/* Boşsa 1: yazma ve okuma göstergeleri aynı yerdeyse okunacak bir şey yoktur */
static int rb_bos_mu(HalkaTampon *rb)
{
  return rb->yaz == rb->oku;
}

/* Bir karakter ekler. Tampon doluysa karakteri atar ve 0 döndürür.
 * Bir hücre bilerek boş bırakılır; yoksa "dolu" ile "boş" aynı görünürdü. */
static int rb_yaz(HalkaTampon *rb, uint8_t c)
{
  uint16_t sonraki = (rb->yaz + 1) % RB_BOYUT;   /* sona gelince başa dön */
  if (sonraki == rb->oku)
  {
    return 0;                                     /* dolu */
  }
  rb->veri[rb->yaz] = c;
  rb->yaz = sonraki;
  return 1;
}

/* Bir karakter alır. Tampon boşsa 0 döndürür. */
static int rb_oku(HalkaTampon *rb, uint8_t *c)
{
  if (rb_bos_mu(rb))
  {
    return 0;
  }
  *c = rb->veri[rb->oku];
  rb->oku = (rb->oku + 1) % RB_BOYUT;
  return 1;
}

/* ---------------- Ayar menüsü (durum makinesi) ---------------- */

static void menu_goster(void)
{
  printf("\r\n=== Ayarlar ===\r\n");
  for (int i = 0; i < AYAR_SAYISI; i++)
  {
    printf("%d) %-16s (simdi: %d)\r\n", i + 1, ayarlar[i].ad, *ayarlar[i].deger);
  }
  printf("Secim:\r\n");
}

/* Metni sayıya çevirir. Sadece rakamlardan oluşuyorsa 1 döndürür. */
static int sayiya_cevir(const char *s, int *sonuc)
{
  int n = 0;
  if (*s == '\0') return 0;            /* boş satır sayı değildir */
  for (; *s != '\0'; s++)
  {
    if (*s < '0' || *s > '9') return 0;
    n = n * 10 + (*s - '0');
    if (n > 10000) return 0;           /* saçma büyük sayıları reddet */
  }
  *sonuc = n;
  return 1;
}

/* Tamamlanan bir satırı menünün o anki durumuna göre işler */
static void satir_isle(const char *satir)
{
  int n;

  if (menu_durum == SECIM_BEKLE)
  {
    if (sayiya_cevir(satir, &n) && n >= 1 && n <= AYAR_SAYISI)
    {
      secili_ayar = n - 1;
      menu_durum = DEGER_BEKLE;
      printf("Yeni deger (%d-%d):\r\n", ayarlar[secili_ayar].min, ayarlar[secili_ayar].max);
    }
    else
    {
      printf("Gecersiz secim.\r\n");
      menu_goster();
    }
  }
  else   /* DEGER_BEKLE */
  {
    Ayar *a = &ayarlar[secili_ayar];
    if (sayiya_cevir(satir, &n) && n >= a->min && n <= a->max)
    {
      *a->deger = n;
      printf("OK: %s = %d\r\n", a->ad, n);
    }
    else
    {
      printf("Gecersiz deger.\r\n");
    }
    menu_durum = SECIM_BEKLE;
    menu_goster();
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  HAL_TIM_Base_Start_IT(&htim2);   /* TIM2: her 10 ms'de kesme */
  printf("\r\n=== STM32 mouse (UART) ===\r\n");

  Tik sol = { BTN_CLICK_GPIO_Port, BTN_CLICK_Pin, 'L', GPIO_PIN_SET, 0 };
  Tik sag = { B1_GPIO_Port,        B1_Pin,        'R', GPIO_PIN_SET, 0 };

  uint32_t son_tick = 0;      /* en son işlenen tick */
  uint32_t basili_sure = 0;   /* yön butonları kaç tick'tir basılı (hızlanma için) */

  char satir[SATIR_BOYUT];    /* bilgisayardan gelen karakterlerle oluşan satır */
  int  satir_uzunluk = 0;

  HAL_UART_Receive_IT(&huart2, &rx_bayt, 1);   /* ilk karakteri kesmeyle almaya başla */
  menu_goster();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* ---- 0) Bilgisayardan gelen karakterleri tampondan al, satır tamamlanınca işle ---- */
    uint8_t c;
    while (rb_oku(&rx_tampon, &c))
    {
      if (c == '\r' || c == '\n')
      {
        if (satir_uzunluk > 0)
        {
          satir[satir_uzunluk] = '\0';
          satir_isle(satir);
          satir_uzunluk = 0;
        }
      }
      else if (satir_uzunluk < SATIR_BOYUT - 1)
      {
        satir[satir_uzunluk++] = (char)c;
      }
    }

    /* Her 10 ms'de bir (yeni tick gelince) butonlara bak */
    if (tick10ms != son_tick)
    {
      son_tick = tick10ms;

      /* ---- 1) Yön butonları: basılı olduğu sürece imleci kaydır ---- */
      int sag_b   = basili(BTN_RIGHT_GPIO_Port, BTN_RIGHT_Pin);
      int sol_b   = basili(BTN_LEFT_GPIO_Port,  BTN_LEFT_Pin);
      int yukari  = basili(BTN_UP_GPIO_Port,    BTN_UP_Pin);
      int asagi   = basili(BTN_DOWN_GPIO_Port,  BTN_DOWN_Pin);

      if (sag_b || sol_b || yukari || asagi)
      {
        basili_sure++;
        int hiz = hiz_baslangic + (int)basili_sure / hizlanma_tick;   /* basılı tuttukça hızlan */
        if (hiz > hiz_max) hiz = hiz_max;

        int dx = (sag_b - sol_b) * hiz;      /* sağ = +x, sol = -x */
        int dy = (asagi - yukari) * hiz;     /* ekranda y aşağı doğru artar */
        if (dx != 0 || dy != 0)
        {
          printf("M %d %d\r\n", dx, dy);
        }
      }
      else
      {
        basili_sure = 0;                     /* bırakınca hız başa dönsün */
      }

      /* ---- 2) Tık butonları: basma ve bırakma anını gönder ---- */
      tik_guncelle(&sol);
      tik_guncelle(&sag);

      /* ---- 3) LED: herhangi bir tık basılıyken yansın ---- */
      int tik_basili = (sol.durum == GPIO_PIN_RESET) || (sag.durum == GPIO_PIN_RESET);
      HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, tik_basili ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 8399;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 99;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : BTN_UP_Pin BTN_DOWN_Pin BTN_LEFT_Pin BTN_RIGHT_Pin */
  GPIO_InitStruct.Pin = BTN_UP_Pin|BTN_DOWN_Pin|BTN_LEFT_Pin|BTN_RIGHT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : BTN_CLICK_Pin */
  GPIO_InitStruct.Pin = BTN_CLICK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BTN_CLICK_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2)
  {
    tick10ms++;   /* 10 ms geçti */
  }
}

/* UART'tan bir karakter gelince HAL bu fonksiyonu çağırır (kesme içinde: kısa tut!) */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    rb_yaz(&rx_tampon, rx_bayt);                 /* karakteri tampona koy */
    HAL_UART_Receive_IT(&huart2, &rx_bayt, 1);   /* bir sonraki karakteri beklemeye başla */
  }
}

/* Taşma gibi bir UART hatasından sonra almayı yeniden başlat, yoksa alma durur */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    HAL_UART_Receive_IT(&huart2, &rx_bayt, 1);
  }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
