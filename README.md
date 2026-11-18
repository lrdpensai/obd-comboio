# obd-comboio

Gravação de OBD para uma viagem em comboio. Só leitura: nenhum comando escreve no carro.

- `index.html` — página para iPhone/Android (abrir no Bluefy no iPhone). Lê dongle ELM327 **BLE** e GPS.
- `firmware/` — ESP32 comum (esp32dev) que lê ELM327 por **Bluetooth clássico**, guarda na flash e envia
  para Azure Blob pelo Wi‑Fi (Starlink). Para enviar por https ele pausa o Bluetooth a cada 3 min
  (~5 s sem leitura), porque com o Bluetooth clássico ligado não sobra memória para o TLS.

## Firmware

```
cd firmware
python -m platformio run -e esp32dev -t upload --upload-port COM9
```

`firmware/include/secrets.h` (fora do git) define `AZ_ACCOUNT`, `AZ_SAS` (token só de escrita),
`DEVICE_LABEL` e `OBD_NAME`.

### No carro
1. Dongle na porta OBD; ESP no powerbank.
2. Wi‑Fi: as redes ficam em `secrets.h` (`WIFI_NETS`). O ESP conecta na de maior sinal, varre de novo
   quando o sinal cai abaixo de −75 dBm ou quando o envio falha (rede sem internet), e sem rede guarda tudo
   na flash até alguma voltar.
3. LED piscando devagar = procurando o dongle; piscando rápido = lendo.
4. Ao parar: desligar o carro, tirar o dongle (o pino 16 é alimentação permanente).

Linha gravada: `boot,ms,epoch_ms,tipo,nome,valor` (epoch 0 até o NTP sincronizar; reconstruir pela linha `meta,ntp`).

### Varredura de PIDs
- **Pelo PC** (dongle livre, celular e ESP desconectados): `python sweep.py <MAC> saida.json` em
  `obd-comboio-secrets` (mode 01 01..FF por resposta real + mode 09; so leitura).
- **Pelo ESP (Amarok):** `python -m platformio run -e amarok_sweep -t upload --upload-port COM9` grava a versao
  que, uma vez por gravacao, varre mode 01 (01..FF) e mode 09 logo apos conectar e sobe as linhas
  `sweep,01XX,<ms>;<resposta crua>` ao Azure; fecha com `meta,sweep_respostas` e `meta,sweep_ms`.
  Em qualquer versao, o comando remoto `sweep` em `cmd/<mac>.txt` pede outra varredura.
- **Pelo celular:** build de depuracao `.dev` do Drive Analyst (worktree `comboio-dev`), tela `SweepActivity`
  (mode 01 e mode 22).
Enquanto um aparelho esta conectado ao dongle, nenhum outro consegue usar o mesmo dongle.
