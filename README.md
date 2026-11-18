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
