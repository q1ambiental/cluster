#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "driver/adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "ssd1306.h"
#include <string.h>
#include "driver/twai.h"
#include "esp_timer.h"

#define BUZZER_pino 23
#define bttn_R 32
#define bttn_Cruise 33
#define bttn_VPIloto 18
#define bttn_mute 19 
#define ENCODER_CLK 25
#define ENCODER_DT  26
#define ENCODER_SW  27
#define LED1 5
#define LED2 4
#define LED3 2
#define LED4 14
#define LED5 12
#define LED6 13


#define CAN_ID_ECM             0x100
#define CAN_ID_BCM             0x200
#define CAN_ID_CRUISE_TX       0x300  // Cruise enviado pelo Cluster
#define BUZZER_PULSE_MS        200
#define CAN_TIMEOUT_US         500000
#define ADC_AMOSTRAS           16


/*QueueHandle_t filaCombustivel;
QueueHandle_t filaIluminacao;*/
QueueHandle_t filaBttn;
QueueHandle_t filaCAN;

typedef struct {
    int bttn;
} eventBttn;

typedef struct {
    float velocidade;
    bool setaEsquerda;
    bool setaDireita;
    float temperatura;
} dadosCAN;

const char *menus[] = {
    "Menu 1: Conducao",
    "Menu 2: Carroceria",
    "Menu 3: Diagnostico OBD-II"
};

SSD1306_t Oled;
volatile int menuAtual = 0;
volatile float nivelCombustivelAtual = 0;
volatile float nivelIluminacaoAtual = 0;
volatile bool pcmOnline = false;
volatile bool bcmOnline = false;
volatile bool Mute = false;
int64_t ultimoPCM = 0;
int64_t ultimoBCM = 0;
const int barra[6] = {LED1, LED2, LED3, LED4, LED5, LED6};

void iniciarSistema(void);

void task_oled(void *pvParameter);
void task_encoder(void *pvParameterEncoder);
void task_bttn(void *pvParameterBttn);
void task_potenciometroIluminacao(void *pvParameterIlu);
void task_potenciometroCombustivel(void *pvParameterComb);
void task_can_rx(void *pvParameter);
void task_can_timeout(void *pvParameter);
void task_buzzer(void *pvParameter);
void atualizarBarra(float velocidade);

static void solicitarBuzzer(uint32_t duracao_ms);
static void pararBuzzer(void);
static uint8_t lerSinalIntel(const uint8_t *dados, uint8_t inicio, uint8_t tamanho);
static void oled_renderizar(const char linhas[8][17]);
static int lerADCMedio(adc1_channel_t canal);
static float filtrarADC(float valor, float *estado);

void app_main(void){
    iniciarSistema();
    xTaskCreate(&task_potenciometroCombustivel, "task_potComb", 2048, NULL, 4, NULL);
    xTaskCreate(&task_potenciometroIluminacao, "task_potIlu", 2048, NULL, 3, NULL);
    xTaskCreate(&task_encoder, "task_encoder", 4096, NULL, 5, NULL);
    xTaskCreate(&task_oled, "task_oled", 4096, NULL, 6, NULL);
    xTaskCreate(&task_bttn, "task_bttn", 2048, NULL, 7, NULL);
    xTaskCreate(&task_can_rx, "task_can_rx", 2048, NULL, 9, NULL);
    xTaskCreate(&task_can_timeout, "CAN Timeout", 2048, NULL, 10, NULL);
    xTaskCreate(&task_buzzer, "task_buzzer", 2048, NULL, 8, NULL);
}
volatile int64_t buzzerAte = 0;
portMUX_TYPE buzzerMux = portMUX_INITIALIZER_UNLOCKED;

void atualizarBarra(float velocidade){
    int ledsAcesos = (int)(velocidade / 25.0f);
    if (ledsAcesos < 0)
        ledsAcesos = 0;
    if (ledsAcesos > 6)
        ledsAcesos = 6;
    for(int i = 0; i < 6; i++){
        gpio_set_level(barra[i], i < ledsAcesos);
    }
}

static void solicitarBuzzer(uint32_t duracao_ms)
{
    if (Mute) {
        return;
    }

    int64_t novoFim = esp_timer_get_time() + ((int64_t)duracao_ms * 1000);
    portENTER_CRITICAL(&buzzerMux);
    if (novoFim > buzzerAte) {
        buzzerAte = novoFim;
    }
    portEXIT_CRITICAL(&buzzerMux);
}

static void pararBuzzer(void)
{
    portENTER_CRITICAL(&buzzerMux);
    buzzerAte = 0;
    portEXIT_CRITICAL(&buzzerMux);
}

static uint8_t lerSinalIntel(const uint8_t *dados, uint8_t inicio, uint8_t tamanho)
{
    uint8_t byte = inicio / 8;
    uint8_t deslocamento = inicio % 8;
    uint16_t mascara = (uint16_t)((1U << tamanho) - 1U);
    return (uint8_t)((dados[byte] >> deslocamento) & mascara);
}

static int lerADCMedio(adc1_channel_t canal)
{
    int soma = 0;
    for (int i = 0; i < ADC_AMOSTRAS; i++) {
        soma += adc1_get_raw(canal);
    }
    return soma / ADC_AMOSTRAS;
}

static float filtrarADC(float valor, float *estado)
{
    if (*estado < 0.0f) {
        *estado = valor;
    } else {
        *estado = (*estado * 0.75f) + (valor * 0.25f);
    }
    return *estado;
}

void task_buzzer(void *pvParameter)
{
    bool estadoAnterior = false;

    while (1) {
        int64_t agora = esp_timer_get_time();
        int64_t fim;
        bool mutado = Mute;

        portENTER_CRITICAL(&buzzerMux);
        fim = buzzerAte;
        portEXIT_CRITICAL(&buzzerMux);

        bool deveLigar = !mutado && agora < fim;
        if (deveLigar != estadoAnterior) {
            ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0,
                          deveLigar ? 512 : 0);
            ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0);
            estadoAnterior = deveLigar;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void enviarCruiseCAN(int velocidade, bool ativo)
{
    twai_message_t msg = {
        .identifier = CAN_ID_CRUISE_TX,
        .data_length_code = 2,
        .flags = 0
    };
    msg.data[0] = (uint8_t)velocidade;
    msg.data[1] = ativo ? 0x01 : 0x00;
    esp_err_t erro = twai_transmit(&msg, pdMS_TO_TICKS(100));
    if (erro != ESP_OK) {
        printf("Erro ao transmitir cruise: %s\n", esp_err_to_name(erro));
    }
}

static void oled_renderizar(const char linhas[8][17])
{
    static char linhasAnteriores[8][17];
    static bool inicializado = false;
    bool mudou = !inicializado;
    char linhasNormalizadas[8][17];

    for (int page = 0; page < 8; page++) {
        memset(linhasNormalizadas[page], ' ', sizeof(linhasNormalizadas[page]) - 1);
        linhasNormalizadas[page][sizeof(linhasNormalizadas[page]) - 1] = '\0';

        size_t tamanho = strlen(linhas[page]);
        if (tamanho > sizeof(linhasNormalizadas[page]) - 1) {
            tamanho = sizeof(linhasNormalizadas[page]) - 1;
        }
        memcpy(linhasNormalizadas[page], linhas[page], tamanho);

        if (!inicializado && strncmp(linhasNormalizadas[page], linhasAnteriores[page],
                                     sizeof(linhasNormalizadas[page])) != 0) {
            mudou = true;
        }
        if (inicializado && strncmp(linhasNormalizadas[page], linhasAnteriores[page],
                                    sizeof(linhasNormalizadas[page])) != 0) {
            mudou = true;
        }
    }

    if (!mudou) {
        return;
    }

    for (int page = 0; page < 8; page++) {
        ssd1306_set_text(&Oled, page, linhasNormalizadas[page], 16, false);
        memcpy(linhasAnteriores[page], linhasNormalizadas[page],
               sizeof(linhasNormalizadas[page]));
    }

    ssd1306_show_buffer(&Oled);
    inicializado = true;
}

void task_can_timeout(void *pvParameter)
{
    while (1){
        int64_t agora = esp_timer_get_time();
        if (pcmOnline && (agora - ultimoPCM) > CAN_TIMEOUT_US){
            pcmOnline = false;
            printf("TIMEOUT: PCM desconectada!\n");
        }
        if (bcmOnline && (agora - ultimoBCM) > CAN_TIMEOUT_US){
            bcmOnline = false;
            printf("TIMEOUT: BCM desconectada!\n");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void task_can_rx(void *pvParameter)
{
    twai_message_t rx_msg;
    dadosCAN dados = {
        .velocidade = 0,
        .setaEsquerda = false,
        .setaDireita = false,
        .temperatura = 0
    };
    while (1) {
        if (twai_receive(&rx_msg, portMAX_DELAY) == ESP_OK) {
            solicitarBuzzer(BUZZER_PULSE_MS);

            switch (rx_msg.identifier) {
                case CAN_ID_BCM: // BCM: Seta_E_D = bits 5..6 do byte 0
                    if (rx_msg.data_length_code >= 1) {
                        ultimoBCM = esp_timer_get_time();
                        bcmOnline = true;
                        uint8_t seta = lerSinalIntel(rx_msg.data, 5, 2);
                        dados.setaEsquerda = seta == 1 || seta == 3;
                        dados.setaDireita = seta == 2 || seta == 3;
                    }
                    break;
                case CAN_ID_ECM: // ECM/PCM: Temp_Motor byte 0, Velocidade byte 1
                    if (rx_msg.data_length_code >= 2) {
                        ultimoPCM = esp_timer_get_time();
                        pcmOnline = true;
                        dados.temperatura = lerSinalIntel(rx_msg.data, 0, 8);
                        dados.velocidade = lerSinalIntel(rx_msg.data, 8, 8);
                        atualizarBarra(dados.velocidade);
                    }
                    break;
            }

            printf("CAN RX ID=0x%03lx DLC=%d\n",
                   (unsigned long)rx_msg.identifier,
                   rx_msg.data_length_code);
            xQueueOverwrite(filaCAN, &dados);
        }
    }
}

void task_oled(void *pvParameter) {
    eventBttn evento;
    dadosCAN dados = {
        .velocidade = 0,
        .setaEsquerda = false,
        .setaDireita = false,
        .temperatura = 0
    };
    char mensagem[24] = "";
    int64_t mensagemAte = 0;
    bool cruiseAtivo = false;
    int velocidadePiloto = 50;

    while(1) {
        if (xQueueReceive(filaCAN, &dados, 0)) {
        }
        if (xQueueReceive(filaBttn, &evento, pdMS_TO_TICKS(20))) {
            switch(evento.bttn) {
                case 1: strcpy(mensagem, "! Trip Reset"); break;
                case 2:
                    strcpy(mensagem, "! Cruise Control");
                    cruiseAtivo = !cruiseAtivo;
                    enviarCruiseCAN(velocidadePiloto, cruiseAtivo);
                    break;
                case 3: 
                    if (cruiseAtivo) {
                        velocidadePiloto += 5;
                        if(velocidadePiloto >180) {
                            velocidadePiloto = 180;
                        }
                        enviarCruiseCAN(velocidadePiloto, cruiseAtivo);
                        strcpy(mensagem, "Velocidade Cruise");
                    } else {
                        strcpy(mensagem, "Cruise desligado");
                    }
                    break;
                case 6:
                    if (cruiseAtivo) {
                        velocidadePiloto -= 5;
                        if (velocidadePiloto < 0){
                            velocidadePiloto = 0;
                        }
                        enviarCruiseCAN(velocidadePiloto, cruiseAtivo);
                    }
                    break;
                case 4: 
                    strcpy(mensagem, "! Mute Alarme");
                    break;
            case 5: strcpy(mensagem, "Consumo medio zerado!"); break;
            }
            mensagemAte = esp_timer_get_time() + 1000000;
        }

        char linhas[8][17] = {{0}};
        if (esp_timer_get_time() < mensagemAte) {
            snprintf(linhas[3], sizeof(linhas[3]), "%.16s", mensagem);
        } else {
            snprintf(linhas[0], sizeof(linhas[0]), "%s", menus[menuAtual]);

            if (menuAtual == 0) {
                if (Mute) {
                    snprintf(linhas[7], sizeof(linhas[7]), "Mute");
                }
                if (cruiseAtivo) {
                    snprintf(linhas[2], sizeof(linhas[2]), "Cruise");
                    snprintf(linhas[3], sizeof(linhas[3]), "Velocidade: %d", velocidadePiloto);
                }
                snprintf(linhas[4], sizeof(linhas[4]), "Combustivel: %.0f%%", nivelCombustivelAtual);
                snprintf(linhas[5], sizeof(linhas[5]), "Brilho: %.0f%%", nivelIluminacaoAtual);
            }
            if (menuAtual == 1) {
                snprintf(linhas[2], sizeof(linhas[2]), "Velocidade: %.0f", dados.velocidade);
                if (dados.setaEsquerda) {
                    snprintf(linhas[4], sizeof(linhas[4]), "<- SETA");
                }
                if (dados.setaDireita) {
                    snprintf(linhas[5], sizeof(linhas[5]), "SETA ->");
                }
            }
            if (menuAtual == 2) {
               snprintf(linhas[2], sizeof(linhas[2]), "Temp: %.1f C", dados.temperatura); 
            }
        }

        oled_renderizar(linhas);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void task_encoder(void *pvParameterEncoder) {
    int lastStateCLK = gpio_get_level(ENCODER_CLK);
    int lastStateSW = 1;
    eventBttn evento;

    while(1) {
        int currentStateCLK = gpio_get_level(ENCODER_CLK);
        if (currentStateCLK != lastStateCLK) {
            if (gpio_get_level(ENCODER_DT) != currentStateCLK) {
                menuAtual++;
            } else {
                menuAtual--;
            }
            if (menuAtual > 2) menuAtual = 0;
            if (menuAtual < 0) menuAtual = 2;
            printf("Menu atual: %s\n", menus[menuAtual]);
        }
        lastStateCLK = currentStateCLK;

        int currentStateSW = gpio_get_level(ENCODER_SW);
        if (lastStateSW == 1 && currentStateSW == 0) {
            evento.bttn = 5;
            xQueueSend(filaBttn, &evento, 0);
        }
        lastStateSW = currentStateSW;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void task_bttn(void *pvParameterBttn) {
    eventBttn evento; 
    int last_R = 1;
    int last_Cruise = 1;
    int last_VPIloto = 1;
    int last_mute = 1;
    int tempoSegurando = 0;

    while(1) {
        int current_R = gpio_get_level(bttn_R);
        int current_Cruise = gpio_get_level(bttn_Cruise);
        int current_VPIloto = gpio_get_level(bttn_VPIloto);
        int current_mute = gpio_get_level(bttn_mute);

        if (last_R == 1 && current_R == 0) {
            evento.bttn = 1;
            xQueueSend(filaBttn, &evento, 0);
        }
        if (last_Cruise == 1 && current_Cruise == 0) {
            evento.bttn = 2;
            xQueueSend(filaBttn, &evento, 0);
        }
        if (last_VPIloto == 1 && current_VPIloto == 0) {
            evento.bttn = 3;
            xQueueSend(filaBttn, &evento, 0);
            tempoSegurando = 0;
        }
        if (current_VPIloto == 0) {
            tempoSegurando++;
            if (tempoSegurando >= 50) {
                evento.bttn = 6;
                xQueueSend(filaBttn, &evento, 0);
                tempoSegurando = 0;
            }
        } else {
            tempoSegurando = 0;
        }
        if (last_mute == 1 && current_mute == 0) {
            Mute = !Mute;
            if (Mute) {
                pararBuzzer();
            }
            evento.bttn = 4;
            xQueueSend(filaBttn, &evento, 0);
        }

        last_R = current_R;
        last_Cruise = current_Cruise;
        last_VPIloto = current_VPIloto;
        last_mute = current_mute;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void task_potenciometroIluminacao(void *pvParameterIlu) {
    float valorFiltrado = -1.0f;

    while(1) {
        int valorPIluminacao = lerADCMedio(ADC1_CHANNEL_7);
        float nivelIluminacao = (valorPIluminacao / 4095.0f) * 100.0f;
        nivelIluminacao = filtrarADC(nivelIluminacao, &valorFiltrado);
        nivelIluminacaoAtual = (float)((int)(nivelIluminacao + 0.5f));
        /* xQueueOverwrite(filaIluminacao, &nivelIlumincao); */
        printf("Nivel Iluminacao: %.0f%%\n", nivelIluminacaoAtual);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void task_potenciometroCombustivel(void *pvParameterComb) {
    float valorFiltrado = -1.0f;
    bool combustivelBaixo = false;

    while (1) {
        int valorPCombustivel = lerADCMedio(ADC1_CHANNEL_6);
        float nivelCombustivel = (valorPCombustivel / 4095.0f) * 100.0f;
        nivelCombustivel = filtrarADC(nivelCombustivel, &valorFiltrado);
        nivelCombustivelAtual = (float)((int)(nivelCombustivel + 0.5f));
        /* xQueueOverwrite(filaCombustivel, &nivelCombustivel); */
        printf("Nivel Combustivel: %.0f%%\n", nivelCombustivelAtual);

        if (!combustivelBaixo && nivelCombustivel < 10.0f) {
            combustivelBaixo = true;
        } else if (combustivelBaixo && nivelCombustivel > 11.0f) {
            combustivelBaixo = false;
        }

        if (combustivelBaixo) {
            solicitarBuzzer(BUZZER_PULSE_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    } 
}

void iniciarSistema (void){
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11); // GPIO35

    ledc_timer_config_t timer = {
        .speed_mode       = LEDC_HIGH_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_10_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 440,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num       = BUZZER_pino,
        .speed_mode     = LEDC_HIGH_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&channel);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL<<bttn_R) | (1ULL<<bttn_Cruise) | (1ULL<<bttn_VPIloto) | (1ULL<<bttn_mute),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    gpio_config_t encoder_conf = {
    .pin_bit_mask = (1ULL<<ENCODER_CLK) | (1ULL<<ENCODER_DT) | (1ULL<<ENCODER_SW),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&encoder_conf);

    gpio_config_t led_conf = {
    .pin_bit_mask = (1ULL << LED1) | (1ULL << LED2) | (1ULL << LED3) | (1ULL << LED4) | (1ULL << LED5) | (1ULL << LED6),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);
    atualizarBarra(0);

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
            GPIO_NUM_17,   // TX
            GPIO_NUM_16,   // RX
            TWAI_MODE_NORMAL
        );
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_driver_install(&g_config, &t_config, &f_config);
    twai_start();

    i2c_master_init(&Oled, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
    ssd1306_init(&Oled, 128, 64);
    ssd1306_clear_screen(&Oled, false);
    ssd1306_contrast(&Oled, 0xff);
    ssd1306_display_text(&Oled, 0, "Sistema CAN", 11, false);
    ssd1306_display_text(&Oled, 2, menus[0], strlen(menus[0]), false);
    ssd1306_display_text(&Oled, 4, "OLED iniciado", 13, false);

    /* filaCombustivel = xQueueCreate(1, sizeof(float));
    filaIluminacao = xQueueCreate(1, sizeof(float)); */
    filaCAN = xQueueCreate(1, sizeof(dadosCAN));
    filaBttn = xQueueCreate(10, sizeof(eventBttn));
    if (/* filaCombustivel == NULL ||  */filaCAN == NULL || filaBttn == NULL) {
        printf("Erro ao criar fila!\n");
    }
    
}
