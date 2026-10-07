# Cluster/IHM — Grupo 3

O arquivo `codigotxt.txt` contém o firmware completo em C. No projeto ESP-IDF,
copie seu conteúdo para `main/cluster.c`. O driver SSD1306 e a fonte estão
incluídos: não é necessário instalar uma biblioteca OLED externa.

Compilação validada: **ESP-IDF 5.5.5, ESP32 clássico**. A tag oficial `v6.2`
não estava disponível na consulta; compatibilidade com ela não foi validada.
As APIs usadas são ADC oneshot, I²C master e TWAI on-chip modernos.

## Comandos da IHM

| Controle | Toque curto | Pressão de 1 segundo |
|---|---|---|
| Trip, GPIO32 | Alterna Trip A/B | Zera o trip selecionado |
| Cruise, GPIO33 | Liga/desliga cruise, com PCM online e TX sem falha | Mesmo comando, uma vez |
| Ajuste, GPIO18 | Aumenta 5 km/h | Diminui 5 km/h, sem aumento inicial |
| Mute, GPIO19 | Alterna silêncio | Mesmo comando, uma vez |
| Botão do encoder, GPIO27 | Zera consumo médio estimado | Entra/sai do ajuste de relógio |
| Giro do encoder | Alterna os três menus | — |

No ajuste de relógio, o giro altera minutos; o botão Cruise aumenta uma hora
com toque curto e diminui uma hora com pressão longa. O relógio começa em
00:00:00 ao ligar; o usuário precisa ajustá-lo. Não há RTC externo, sincronização
pela rede ou persistência de hora e métricas após desligamento.

## Ligações e parâmetros

- OLED SSD1306 128×64, endereço `0x3C`: SDA GPIO21, SCL GPIO22. Confira o endereço
  e use os pull-ups adequados do módulo/barramento.
- Encoder: CLK GPIO25, DT GPIO26, SW GPIO27. Botões ligados ao GND.
- Combustível: ADC GPIO34; brilho: ADC GPIO35. Entradas analógicas até 3,3 V.
- Buzzer **ativo**: GPIO23. Use um circuito de acionamento se o módulo exigir
  corrente além da capacidade do GPIO. Buzzer passivo precisa de PWM.
- Barra de LEDs: GPIO5, 4, 2, 14, 12, 13, com resistores apropriados.
  Confira os pinos de strapping GPIO2/5/12: a carga não deve impedir o boot.
- CAN: TX GPIO17, RX GPIO16, 500 kbit/s, via transceptor externo. Confirme níveis
  lógicos do TJA1050/módulo e terminação nas duas extremidades do barramento.
- Ajuste `TANK_L` (padrão 50 litros), `ADC_EMPTY` e `ADC_FULL` antes da demonstração.
  Os parâmetros padrão são hipóteses de simulação, não dados definidos pelo PDF.

## Perfil CAN experimental para o sistema do professor

O PDF especifica os identificadores, mas não o layout dos sinais. Foi mantido
o formato do código original para os sinais; o ID de transmissão foi alterado
para **0x310**, informado pelo usuário. Esse perfil é uma hipótese de teste,
não uma confirmação do DBC. Usa quadros clássicos e IDs de 11 bits:

| ID | Origem | Payload |
|---|---|---|
| `0x100` | PCM | Byte 0: temperatura em °C; byte 1: velocidade em km/h; ambos sem sinal |
| `0x200` | BCM | Byte 0, bit 5: seta esquerda; bit 6: seta direita |
| `0x310` | Cluster | Byte 0: velocidade definida; byte 1: cruise ativo (`0`/`1`) |

O cluster transmite `0x310` a cada 100 ms. PCM/BCM devem transmitir em intervalos
menores que o timeout de 500 ms, com margem. Ao perder PCM, o cluster desativa
cruise, invalida velocidade/temperatura e apaga a barra. Ao perder BCM, invalida
as setas. Ausência de sinal aparece no OLED desde a inicialização.

Exemplos: `0x100 [90,72]` representa 90 °C e 72 km/h; `0x200 [0x60]`
ativa as duas setas. Quadros RTR, estendidos e FD são ignorados.

O menu “Diagnóstico OBD-II” mostra temperatura e estado de comunicação.
Não implementa leitura de DTCs/serviços OBD-II: o PDF não fornece esse protocolo.

## Consumo e brilho

Trip A/B integram a velocidade CAN no tempo, somente enquanto o dado é válido.
O consumo médio é uma **estimativa** em km/l baseada na distância e na queda
do nível do tanque desde o reset. O visor usa `EST` e mostra `--` quando não
há queda suficiente para calcular. Aumento superior a 5 pontos percentuais
em relação ao menor nível observado reinicia a estimativa por reabastecimento.
Ruído, inclinação do tanque e movimentação do potenciômetro afetam o resultado;
um consumo real exige um sinal de combustível consumido/vazão do PCM.

O potenciômetro de brilho altera o contraste do OLED. O alarme de reserva
entra abaixo de 10% e sai acima de 11%. Mute silencia sons e mantém avisos visuais.

## Compilação neste ambiente

O projeto gerado fica fora do repositório, em `/workspace/cluster-build`.
Para instalar/atualizar dependências, copiar o TXT e compilar:

```sh
bash /workspace/cluster-install.sh
```

Para recompilar após editar o TXT:

```sh
export IDF_TOOLS_PATH=/workspace/.idf-tools
export IDF_PYTHON_CHECK_CONSTRAINTS=no
. /workspace/esp-idf-5.5.5/export.sh
cp /workspace/cluster/codigotxt.txt /workspace/cluster-build/main/cluster.c
idf.py -C /workspace/cluster-build build
```

O instalador usa a opção documentada `--no-constraints` e PyPI, pois o domínio
da lista opcional de versões da Espressif foi bloqueado neste ambiente.
TLS e checksums das ferramentas permanecem ativos. O SDK fica fixado na versão
5.5.5; as dependências Python respeitam seus requisitos, mas não a lista opcional.

Para montar um projeto em outra máquina, use na raiz:

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cluster)
```

E em `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "cluster.c" INCLUDE_DIRS "."
    REQUIRES esp_adc esp_driver_gpio esp_driver_i2c esp_driver_twai esp_timer)
```

Ative o ESP-IDF, execute `idf.py set-target esp32`, `idf.py build` e, na máquina
com a placa conectada, `idf.py -p PORTA flash monitor`.

## Validação

Os testes embarcados no TXT podem rodar no computador:

```sh
gcc -x c -std=c11 -Wall -Wextra -Werror -DCLUSTER_HOST_TEST codigotxt.txt -lm -o /tmp/cluster-test
/tmp/cluster-test
```

Eles exercitam distância/timeout CAN, cruise, trips, reset/reabastecimento da
estimativa de consumo, reserva, mute, debounce e comandos do relógio.
A compilação real com ESP-IDF 5.5.5 também passou. Execução em hardware ainda
não foi validada. Na bancada, confira inicialização do OLED, sentido do encoder,
contraste, reserva, todos os botões, frames entre os três módulos e desconexão
de PCM/BCM. Confirme que os LEDs e dados são invalidados após o timeout.

## Adaptação a possíveis mensagens

No início do TXT, o bloco **PERFIL CAN EDITAVEL** centraliza:

- `CLUSTER_TX_ID`: `0x310` (informado como ID usado na comunicação com o professor).
- `CLUSTER_SPEED_RX_ID` e `CLUSTER_BODY_RX_ID`: `0x100` e `0x200`, hipóteses anteriores.
- `CLUSTER_TX_DLC`: 2 bytes; `CLUSTER_CAN_BITRATE`: 500000 bit/s.
- `CLUSTER_TX_PERIOD_MS`: 100 ms.
- `sig_*`: bit inicial, largura de 1–16 bits, ordem Intel/Motorola,
  sinal numérico com/sem sinal, fator e offset.

Para cada sinal, a estrutura segue `{start, bits, motorola, signed_value, factor, offset}`.
Intel equivale a `@1` no DBC; Motorola equivale a `@0`, usando a numeração DBC
em que o bit inicial é o mais significativo do sinal. O valor físico é
`bruto * factor + offset`. A transmissão faz a transformação inversa,
arredondando ao inteiro representável. Campos que não cabem no DLC,
se sobrepõem no TX ou excedem sua representação são rejeitados.

O payload TX padrão é `[velocidade_cruise, ativo]`: por exemplo, 50 km/h com
cruise desligado transmite `32 00` em hexadecimal no ID `0x310`.
Isso pode precisar mudar quando houver uma descrição das mensagens esperadas.
O ID CAN identifica a mensagem: não funciona como endereço de destino.

Quadros padrão clássicos de outros IDs são registrados no terminal, com ID,
DLC e bytes em hexadecimal, no máximo uma mensagem desconhecida por segundo.
Eles não são interpretados como velocidade ou temperatura. Esse log ajuda a
identificar o tráfego, mas não permite descobrir com certeza escalas e significados.
Quadros estendidos, RTR e CAN FD não são suportados por este perfil.

Para testar os botões de cruise sem PCM/BCM, altere `CLUSTER_BENCH_MODE` para `1`
no bloco de configuração e recompile. Isso permite ligar cruise sem receber
velocidade; não cria um ACK CAN. Para uma transmissão reconhecida, é necessário
outro nó/uma interface CAN em modo ativo, com bitrate e terminação corretos.
Uma interface em modo somente escuta não fornece ACK. O código continua
indicando falha TX quando a transmissão não é reconhecida.

Os testes locais incluem vetores conhecidos Intel/Motorola de 16 bits,
sinal com fator/offset e valor negativo, campo que atravessa bytes, rejeição
por DLC insuficiente e montagem do payload TX. São testes do adaptador,
não evidência de compatibilidade com o DBC do professor.
