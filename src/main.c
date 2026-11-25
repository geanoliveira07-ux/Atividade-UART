#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>

#define DEVICE_ID           0x01    // ID esperado pelo receptor
#define PACKET_SIZE         6       // ID | R | G | B | ACAO | CHECK
#define RX_TIMEOUT_MS       100     // Timeout curto para detectar pacotes truncados

// Códigos de Ação
#define ACAO_LIGAR          0x01
#define ACAO_DESLIGAR       0x02
#define ACAO_TOGGLE         0x03
#define ACAO_PISCA_1S       0x04
#define ACAO_PISCA_2S       0x05

/* Usa a UART padrão do console (115200 8N1) */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

// ================= SINCRONIZAÇÃO E FILA UART ==================================================================== 
// Fila para armazenar bytes vindos da interrupção da UART
K_MSGQ_DEFINE(uart_rx_msgq, sizeof(uint8_t), 32, 1);
K_MUTEX_DEFINE(led_mutex);

// Estado compartilhado do LED
struct led_state_t {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t acao;
    bool ligado;
};

static struct led_state_t estado_led = {0, 0, 0, ACAO_DESLIGAR, false};

// ================= FUNÇÕES AUXILIARES ===========================================================================
static void aplicar_hardware_rgb(bool on, uint8_t r, uint8_t g, uint8_t b)
{
    // qualquer valor > 0 acende o canal correspondente
    gpio_pin_set_dt(&led_r, (on && r > 0) ? 1 : 0);
    gpio_pin_set_dt(&led_g, (on && g > 0) ? 1 : 0);
    gpio_pin_set_dt(&led_b, (on && b > 0) ? 1 : 0);
}

static uint8_t calcular_checksum_xor(const uint8_t *pkt) {		return pkt[0] ^ pkt[1] ^ pkt[2] ^ pkt[3] ^ pkt[4];		}

// Callback de Interrupção (ISR) da UART
static void uart_isr_callback(const struct device *dev, void *user_data)
{
    uint8_t byte_rx;

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        if (uart_fifo_read(dev, &byte_rx, 1) == 1) {
            // Coloca o byte na fila; se estiver cheia, descarta sem travar
            k_msgq_put(&uart_rx_msgq, &byte_rx, K_NO_WAIT);
        }
    }
}

// ================= THREAD 1: RECEPTOR UART ===================================================================== 
void thread_uart_rx(void *p1, void *p2, void *p3)
{
    uint8_t pkt[PACKET_SIZE];
    uint8_t byte_rx;

    printk("\n=== Receptor UART RGB Iniciado ===\n");
    printk("ID esperado: 0x%02X | Protocolo: 6 bytes (ID R G B ACAO CHECK)\n", DEVICE_ID);
    printk("Aguardando pacotes...\n\n");

    while (1) {
        /* Aguarda o 1º byte indefinidamente (não consome CPU) */
        k_msgq_get(&uart_rx_msgq, &pkt[0], K_FOREVER);

        /* Lê os 5 bytes restantes com timeout curto para detectar truncamento */
        bool truncado = false;
        for (int i = 1; i < PACKET_SIZE; i++) {
            if (k_msgq_get(&uart_rx_msgq, &byte_rx, K_MSEC(RX_TIMEOUT_MS)) != 0) {
                printk("[ERRO] Pacote truncado! Recebidos apenas %d de %d bytes (Timeout).\n",
                       i, PACKET_SIZE);
                truncado = true;
                break;
            }
            pkt[i] = byte_rx;
        }

        if (truncado) {
            continue;
        }

        /* Exibe o pacote bruto recebido */
        printk("[RX] Pacote: %02X %02X %02X %02X %02X %02X -> ",
               pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5]);

        /* 1. Validação do ID */
        if (pkt[0] != DEVICE_ID) {
            printk("[ERRO] ID invalido (Esperado: 0x%02X, Recebido: 0x%02X). Ignorado.\n",
                   DEVICE_ID, pkt[0]);
            continue;
        }

        /* 2. Validação do Checksum (XOR dos 5 primeiros bytes) */
        uint8_t check_calc = calcular_checksum_xor(pkt);
        if (pkt[5] != check_calc) {
            printk("[ERRO] Checksum incorreto (Esperado: 0x%02X, Recebido: 0x%02X). Ignorado.\n",
                   check_calc, pkt[5]);
            continue;
        }

        /* 3. Validação da Ação e Atualização do Estado */
        uint8_t acao = pkt[4];
        if (acao < ACAO_LIGAR || acao > ACAO_PISCA_2S) {
            printk("[ERRO] Acao desconhecida: 0x%02X. Ignorado.\n", acao);
            continue;
        }

        k_mutex_lock(&led_mutex, K_FOREVER);
        estado_led.r = pkt[1];
        estado_led.g = pkt[2];
        estado_led.b = pkt[3];
        estado_led.acao = acao;

        switch (acao) {
            case ACAO_LIGAR:
                estado_led.ligado = true;
                printk("[OK] LIGAR (R:%d G:%d B:%d)\n", pkt[1], pkt[2], pkt[3]);
                break;
            case ACAO_DESLIGAR:
                estado_led.ligado = false;
                printk("[OK] DESLIGAR\n");
                break;
            case ACAO_TOGGLE:
                estado_led.ligado = !estado_led.ligado;
                printk("[OK] TOGGLE -> Novo estado: %s (R:%d G:%d B:%d)\n",
                       estado_led.ligado ? "LIGADO" : "DESLIGADO", pkt[1], pkt[2], pkt[3]);
                break;
            case ACAO_PISCA_1S:
                estado_led.ligado = true;
                printk("[OK] PISCA 1s (R:%d G:%d B:%d)\n", pkt[1], pkt[2], pkt[3]);
                break;
            case ACAO_PISCA_2S:
                estado_led.ligado = true;
                printk("[OK] PISCA 2s (R:%d G:%d B:%d)\n", pkt[1], pkt[2], pkt[3]);
                break;
        }
        k_mutex_unlock(&led_mutex);
    }
}

// ================= THREAD 2: CONTROLE DO LED RGB ===============================================================
void thread_led_ctrl(void *p1, void *p2, void *p3)
{
    int64_t ultimo_toggle = k_uptime_get();

    while (1) {
        k_mutex_lock(&led_mutex, K_FOREVER);

        if (estado_led.acao == ACAO_PISCA_1S || estado_led.acao == ACAO_PISCA_2S) {
            int64_t intervalo = (estado_led.acao == ACAO_PISCA_1S) ? 1000 : 2000;
            int64_t agora = k_uptime_get();

            if ((agora - ultimo_toggle) >= intervalo) {
                estado_led.ligado = !estado_led.ligado;
                ultimo_toggle = agora;
            }
        } else {
            /* Mantém o temporizador zerado fora dos modos de pisca */
            ultimo_toggle = k_uptime_get();
        }

        aplicar_hardware_rgb(estado_led.ligado, estado_led.r, estado_led.g, estado_led.b);

        k_mutex_unlock(&led_mutex);

        /* Dorme 50ms para resposta rápida a novos comandos sem sobrecarregar a CPU */
        k_msleep(50);
    }
}

K_THREAD_DEFINE(uart_rx_tid, 1024, thread_uart_rx, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(led_ctrl_tid, 512, thread_led_ctrl, NULL, NULL, NULL, 6, 0, 0);

int main(void)
{
    if (!device_is_ready(uart_dev)) {		return -1;		}

    if (!gpio_is_ready_dt(&led_r) || !gpio_is_ready_dt(&led_g) || !gpio_is_ready_dt(&led_b)) {
        printk("[ERRO] Dispositivos GPIO dos LEDs nao estao prontos.\n");
        return -1;
    }

    gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);

    /* Configura e habilita a interrupção de recepção da UART */
    uart_irq_callback_user_data_set(uart_dev, uart_isr_callback, NULL);
    uart_irq_rx_enable(uart_dev);

    while (1) {		k_sleep(K_FOREVER);		}

    return 0;
}