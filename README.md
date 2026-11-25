# Controle de LED RGB via UART — Zephyr RTOS (FRDM-KL25Z)

Implementação de um receptor UART orientado a eventos no **Zephyr RTOS** (via **PlatformIO**) para controle do LED RGB da placa **NXP FRDM-KL25Z**, utilizando um protocolo binário fixo de 6 bytes com validação de integridade e recuperação de falhas por *timeout*.

---

## 1. Especificação do Protocolo

A comunicação opera em **115200 bps, 8N1** (8 bits de dados, sem paridade de hardware, 1 stop bit). Cada comando segue o formato fixo de **6 bytes**:

| Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 |
| :---: | :---: | :---: | :---: | :---: | :---: |
| `ID` | `R` | `G` | `B` | `AÇÃO` | `CHECK` |

* **`ID`**: Identificador do dispositivo alvo (`0x01`).
* **`R`, `G`, `B`**: Estado digital de cada canal (`0x00` = desligado, `0x01` a `0xFF` = ligado).
* **`AÇÃO`**:
  * `0x01` — **LIGAR**: Acende o LED na cor informada.
  * `0x02` — **DESLIGAR**: Apaga o LED RGB.
  * `0x03` — **TOGGLE**: Inverte o estado atual do LED (ligado/desligado).
  * `0x04` — **PISCA 1s**: Alterna o LED a cada 1000 ms (não bloqueante).
  * `0x05` — **PISCA 2s**: Alterna o LED a cada 2000 ms (não bloqueante).
* **`CHECK`**: Checksum calculado pelo **XOR** dos 5 primeiros bytes:
  $$\text{CHECK} = \text{ID} \oplus R \oplus G \oplus B \oplus \text{AÇÃO}$$

---

## 2. Pré-condições para os Testes

Antes de iniciar a bateria de testes, verifique os itens abaixo:

1. **Hardware:** Placa `FRDM-KL25Z` conectada via USB (porta OpenSDA).
2. **Firmware:** Projeto compilado e gravado via PlatformIO (`Zephyr RTOS`).
3. **Configuração Serial:** `115200 bps`, `8N1`, sem controle de fluxo.
4. **Ferramenta de Envio:** Como o protocolo utiliza bytes brutos (HEX), utilize um terminal com suporte a envio hexadecimal (**RealTerm**, **HTerm**, **CuteCom**) ou o script Python disponibilizado na Seção 4.
5. **Estado Inicial:** Após o *reset* da placa, o LED RGB deve estar apagado e o terminal deve exibir a mensagem de inicialização do receptor.

---

## 3. Bateria de Testes

### 3.1. Testes Funcionais (Comandos e Cores)

| ID | Cenário | Pacote Enviado (HEX) | Comportamento Esperado (Hardware + Serial) | Status |
| :---: | :--- | :--- | :--- | :---: |
| **CT-01** | Ligar Vermelho | `01 FF 00 00 01 FF` | LED acende em **Vermelho** fixo. Terminal confirma `OK: LIGAR`. | [ ] |
| **CT-02** | Ligar Cor Composta (Ciano) | `01 00 FF FF 01 00` | Canais Verde e Azul acendem simultaneamente (**Ciano**). Terminal confirma `OK: LIGAR`. | [ ] |
| **CT-03** | Ligar Branco (Todos os canais) | `01 FF FF FF 01 FF` | Todos os 3 canais acendem (**Branco**). Terminal confirma `OK: LIGAR`. | [ ] |
| **CT-04** | Desligar LED | `01 00 00 00 02 03` | LED apaga completamente. Terminal confirma `OK: DESLIGAR`. | [ ] |
| **CT-05** | Toggle (Inversão de Estado) | `01 00 00 FF 03 FD` | Enviar 2 vezes seguidas: na 1ª o LED acende em **Azul**; na 2ª o LED apaga. Terminal exibe o novo estado a cada envio. | [ ] |
| **CT-06** | Piscar a cada 1 segundo | `01 00 FF 00 04 FA` | LED pisca em **Verde** alternando estado a cada ~1s. Terminal confirma `OK: PISCA 1s`. | [ ] |
| **CT-07** | Piscar a cada 2 segundos | `01 00 00 FF 05 FB` | LED pisca em **Azul** alternando estado a cada ~2s. Terminal confirma `OK: PISCA 2s`. | [ ] |

---

### 3.2. Testes de Robustez e Tratamento de Erros

Estes testes comprovam que o receptor rejeita dados corrompidos ou incompletos **sem travar o sistema** e sem perder o sincronismo dos próximos pacotes.

| ID | Cenário | Pacote Enviado (HEX) | Comportamento Esperado (Hardware + Serial) | Status |
| :---: | :--- | :--- | :--- | :---: |
| **CT-08** | ID Inválido (com checksum válido) | `02 FF 00 00 01 FC` | Estado do LED **não muda**. Terminal exibe erro de `ID invalido (0x02)`. | [ ] |
| **CT-09** | Checksum Incorreto | `01 FF 00 00 01 00` | Estado do LED **não muda**. Terminal exibe erro de `CHECK incorreto` (esperado `0xFF`, recebido `0x00`). | [ ] |
| **CT-10** | Ação Desconhecida | `01 FF 00 00 09 F7` | Estado do LED **não muda**. Terminal exibe erro de `Acao desconhecida (0x09)`. | [ ] |
| **CT-11** | Pacote Truncado e Recuperação (*Timeout*) | 1º: `01 FF 00` *(aguardar > 150 ms)*<br>2º: `01 00 FF 00 01 FF` | Os 3 bytes iniciais são descartados pelo *timeout* sem travar a UART. O 2º pacote é reconhecido normalmente e acende o LED **Verde**, provando que o buffer não ficou desalinhado. | [ ] |

---

### 3.3. Testes de Concorrência e Eficiência

Estes testes validam que a arquitetura baseada em interrupções, fila (`k_msgq`) e *workqueue* não bloqueia a CPU.

| ID | Cenário | Procedimento | Comportamento Esperado | Status |
| :---: | :--- | :--- | :--- | :---: |
| **CT-12** | Interrupção Imediata de Pisca | 1. Enviar **CT-07** (`01 00 00 FF 05 FB` — Pisca 2s).<br>2. Após 0,5s, enviar **CT-01** (`01 FF 00 00 01 FF` — Ligar Vermelho). | O pisca de 2s é cancelado **imediatamente** (sem aguardar o fim do ciclo de 2s) e o LED passa para Vermelho fixo. | [ ] |
| **CT-13** | Rajada de Pacotes (*Burst*) | Enviar 5 pacotes válidos consecutivos com intervalo de `10 ms` entre eles (via script Python). | Todos os 5 pacotes são enfileirados e processados em ordem no terminal serial, sem erros de *overrun* ou reinicialização da placa. | [ ] |

---

## 4. Script de Automação de Testes (`teste_uart_rgb.py`)

Para executar toda a bateria de forma reproduzível, feche o monitor serial do PlatformIO, instale a biblioteca `pyserial` (`pip install pyserial`) e execute o script abaixo ajustando a porta serial (`COMx` ou `/dev/ttyACMx`):

```python
import serial
import time

PORTA = "COM3"  # Altere para a porta da sua FRDM-KL25Z (ex: '/dev/ttyACM0' no Linux)
BAUDRATE = 115200

def ler_resposta(ser, espera=0.3):
    time.sleep(espera)
    while ser.in_waiting:
        linha = ser.readline().decode(errors="ignore").strip()
        if linha:
            print(f"  [PLACA] {linha}")

def executar_testes():
    ser = serial.Serial(PORTA, BAUDRATE, timeout=1)
    time.sleep(1)
    ser.reset_input_buffer()

    casos = [
        ("CT-01: Ligar Vermelho",           bytes([0x01, 0xFF, 0x00, 0x00, 0x01, 0xFF]), 1.0),
        ("CT-02: Ligar Ciano",              bytes([0x01, 0x00, 0xFF, 0xFF, 0x01, 0x00]), 1.0),
        ("CT-03: Ligar Branco",             bytes([0x01, 0xFF, 0xFF, 0xFF, 0x01, 0xFF]), 1.0),
        ("CT-04: Desligar",                 bytes([0x01, 0x00, 0x00, 0x00, 0x02, 0x03]), 1.0),
        ("CT-05a: Toggle (Liga Azul)",      bytes([0x01, 0x00, 0x00, 0xFF, 0x03, 0xFD]), 1.0),
        ("CT-05b: Toggle (Desliga)",        bytes([0x01, 0x00, 0x00, 0xFF, 0x03, 0xFD]), 1.0),
        ("CT-06: Piscar Verde 1s",          bytes([0x01, 0x00, 0xFF, 0x00, 0x04, 0xFA]), 3.5),
        ("CT-07: Piscar Azul 2s",           bytes([0x01, 0x00, 0x00, 0xFF, 0x05, 0xFB]), 0.6),
        ("CT-12: Interromper Pisca (Verm.)",bytes([0x01, 0xFF, 0x00, 0x00, 0x01, 0xFF]), 1.0),
        ("CT-08: Erro ID Invalido",         bytes([0x02, 0xFF, 0x00, 0x00, 0x01, 0xFC]), 0.5),
        ("CT-09: Erro Checksum",            bytes([0x01, 0xFF, 0x00, 0x00, 0x01, 0x00]), 0.5),
        ("CT-10: Erro Acao Desconhecida",   bytes([0x01, 0xFF, 0x00, 0x00, 0x09, 0xF7]), 0.5),
    ]

    for nome, pacote, pausa in casos:
        print(f"\n>>> Executando {nome} | TX: {pacote.hex(' ').upper()}")
        ser.write(pacote)
        ler_resposta(ser, espera=0.3)
        if pausa > 0.3:
            time.sleep(pausa - 0.3)

    # CT-11: Pacote Truncado + Recuperação
    print("\n>>> Executando CT-11: Pacote Truncado (3 bytes) + Recuperacao")
    ser.write(bytes([0x01, 0xFF, 0x00]))
    time.sleep(0.3)  # Aguarda estourar o timeout da placa
    ser.write(bytes([0x01, 0x00, 0xFF, 0x00, 0x01, 0xFF]))  # Ligar Verde
    ler_resposta(ser, espera=0.5)

    # CT-13: Rajada (Burst)
    print("\n>>> Executando CT-13: Rajada de 5 pacotes consecutivos")
    rajada = [
        bytes([0x01, 0xFF, 0x00, 0x00, 0x01, 0xFF]),
        bytes([0x01, 0x00, 0xFF, 0x00, 0x01, 0xFF]),
        bytes([0x01, 0x00, 0x00, 0xFF, 0x01, 0xFF]),
        bytes([0x01, 0xFF, 0xFF, 0x00, 0x01, 0x00]),
        bytes([0x01, 0x00, 0x00, 0x00, 0x02, 0x03]),
    ]
    for pkt in rajada:
        ser.write(pkt)
        time.sleep(0.01)
    ler_resposta(ser, espera=0.6)

    ser.close()
    print("\n=== Bateria de testes finalizada ===")

if __name__ == "__main__":
    executar_testes()