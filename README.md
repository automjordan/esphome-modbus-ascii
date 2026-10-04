# esphome-modbus-ascii

Componente ESPHome **Modbus ASCII master** e integrazione completa in Home Assistant
del recuperatore di calore **Giacomini KHR-V** (elettronica Innova/Riello **INN-FR-B40**,
pannello a muro **CNV**) — **mantenendo il tastierino a muro collegato e funzionante**.

La stessa elettronica equipaggia numerosi fancoil Innova (Airleaf), Riello e altri
marchi OEM: il componente e buona parte delle scoperte valgono anche per quelli.

> Risultato in due righe: la scheda parla **Modbus ASCII 9600 7E1** sul bus del pannello,
> risponde a FC03 all'indirizzo **1**, ed esegue i comandi solo tramite il **broadcast FC16
> sui registri 101-103** inviato dal pannello. Emulando quel broadcast si comanda la macchina
> da Home Assistant senza togliere il pannello.

---

## Indice

1. [Perché questo progetto](#perché-questo-progetto)
2. [Hardware](#hardware)
3. [Cablaggio](#cablaggio)
4. [Il protocollo, come funziona davvero](#il-protocollo-come-funziona-davvero)
5. [Mappa registri](#mappa-registri)
6. [Installazione](#installazione)
7. [Il componente `modbus_ascii`](#il-componente-modbus_ascii)
8. [Dashboard Home Assistant](#dashboard-home-assistant)
9. [Cose che NON funzionano (e perché)](#cose-che-non-funzionano-e-perché)
10. [Risoluzione problemi](#risoluzione-problemi)
11. [Documentazione del costruttore](#documentazione-del-costruttore)
12. [Crediti](#crediti)

---

## Perché questo progetto

Il manuale del kit Modbus del costruttore descrive un collegamento **RTU** tramite una
scheda bridge e un jumper, e avverte che attivandolo il pannello a muro smette di funzionare.
Nelle discussioni pubbliche chi ha il pannello a muro non è mai riuscito a leggere nulla.

La strada che funziona è un'altra: il bus **pannello ↔ scheda** è già Modbus — in **ASCII**,
non RTU — e la scheda risponde a chiunque le parli in quel formato. Non serve il kit bridge,
non serve il jumper, non serve rinunciare al tastierino.

## Hardware

| Componente | Note |
|---|---|
| **Waveshare ESP32-S3-RS485-CAN** | RS485 isolato, alimentazione 7-36 V, DIN rail. Pin: TX=GPIO17, RX=GPIO18, DE=GPIO21 |
| Alternativa | qualsiasi ESP32 + transceiver RS485 a 3,3 V (meglio isolato) |
| Giacomini KHR-V | taglie 200/300/400/500, versione sensibile (Y) o entalpica (X) |

L'alimentazione dell'ESP32 si può prendere dai morsetti `+` / `−` del bus del pannello
(tipicamente 12 V DC) — il morsetto 7-36 V della Waveshare li accetta direttamente.

> **Attenzione (Waveshare):** mai alimentare contemporaneamente da morsettiera e USB-C.

## Cablaggio

Il bus RS485 del pannello è **uno solo**: scheda `− B A +` ↔ pannello `FAN COIL + A B −`,
con i morsetti `PC A A B B −` del pannello che sono la prosecuzione dello stesso bus
(daisy-chain). Ci si collega in parallelo in un punto qualsiasi: il più comodo è il morsetto
`− B A +` sulla scheda dell'unità, o i `PC` dietro il pannello.

```
Scheda unità  − B A +          Pannello a muro
              │ │ │ │          FAN COIL  + A B −
              │ │ │ └──────────────────┘ │ │ │
              │ │ └────────────────────────┘ │ │
              │ └──────────────────────────────┘ │
              └──────────────────────────────────┘
                │ │
     ESP32  GND B A   (in parallelo; + non collegato ai dati)
```

- `A` → `A+`, `B` → `B−`, `−` → `GND` della morsettiera RS485.
- **Jumper RTU/ASCII della scheda: fuori** (configurazione di fabbrica, pannello funzionante).
- Terminazione 120 Ω: se ti innesti a metà bus lasciala disinserita.
- Se A e B risultano invertiti, nello YAML si compensa con `inverted: true` su `tx_pin`
  e `rx_pin`: i dati arrivano ugualmente. Il sintomo dell'inversione è spazzatura
  che cambia lunghezza al variare del baud rate e non decodifica mai.

## Il protocollo, come funziona davvero

Sul bus viaggiano tre cose, tutte in **Modbus ASCII, 9600 baud, 7 bit, parità pari, 1 stop**:

1. **Broadcast del pannello**, ogni 10 s e a ogni pressione di tasto:
   `:00 10 0065 0003 06 <101> <102> <103> LRC` — FC16 all'indirizzo 0, registri 101-103.
   È l'**unico canale di comando** che la scheda esegue.
2. **Handshake proprietario** ogni 30 s: `:77FF00000001` → risposta `:77FF02xxxx`.
   Indirizzo 0x77, funzione 0xFF. Da ignorare.
3. **La scheda come slave** all'indirizzo **1**: risponde a FC03 (lettura) e FC06 (scrittura)
   **un registro alla volta**. Con `quantità > 1` restituisce dati disallineati.

### Registri del broadcast (101-103)

| Reg | Contenuto |
|---|---|
| 101 | byte alto costante (`0x50` osservato); **nibble basso = velocità**: `0` Automatico, `2` Minima (🌙), `1` Nominale (🌀), `3` Massima (🌀🌀), `4` Booster (85 %, non sul tastierino) |
| 102 | setpoint × 10 |
| 103 | temperatura del pannello × 10 |

Le velocità corrispondono alla tabella MVV della scheda: 2→MVV5, 1→MVV4, 0→MVV3 (auto), 3→MVV2, 4→MVV1.

### Il registro 224 e perché le scritture dirette non comandano

Il registro **224 (CFG)** vale 64 "controllo da supervisione esterna" o 66 "controllo dal CNV".
Con il pannello collegato la scheda lo riporta a **66 entro un secondo** da qualsiasi scrittura.
Di conseguenza FC06 su PRG (201) o setpoint (231) **viene accettato e memorizzato ma non eseguito**:
la macchina segue il broadcast del pannello. Le scritture dirette funzionano solo con il
pannello scollegato (ed è quello che fanno gli utenti senza pannello nel thread indomus).

La soluzione adottata: **emulare il broadcast**. Dopo ogni frame del pannello il componente
ne invia uno identico con il nibble di velocità e il setpoint desiderati, lasciando invariati
il byte alto del 101 e il 103. La scheda esegue l'ultimo broadcast ricevuto. Il pannello
continua a funzionare, ma il suo display mostra la *sua* modalità, non quella reale.

## Mappa registri

Letture FC03 all'indirizzo 1, un registro per richiesta. Tabella dal manuale del kit
(cod. 0180415 rev.02) con le note emerse sul campo.

| Reg | Nome | Note | Scala |
|---|---|---|---|
| 0 | T1 | temperatura aria | ×0,1 °C, signed |
| 1 | T2 | sonda H2 (fancoil) / seconda sonda aria | ×0,1 |
| 2 | T3 | sonda H4 / terza sonda aria | ×0,1 |
| 8 | SP | setpoint **attivo** (= reg 102 del pannello) | ×0,1 |
| 15 | MOT_SET | velocità motore impostata | % |
| 104 | STAT | bit0 raffr., bit1 risc., bit4 antigelo, bit5 allarme, bit7 standby | flag |
| 105 | ALR_STAT | bit0 com, bit1 sonda aria, bit2 H4, bit4 H2, bit7 motore, bit8 GRID, bit10 filtro (tabella Riello) | flag |
| 198 | RELEASE | 2 = scheda, 6 = display | |
| 199 | ID | **1115** = scheda, 1090 = display | |
| 200 | ADR | indirizzo slave (1) | |
| 201 | PRG | bit0-2 modo, bit4 lock, bit7 standby — **solo memoria con pannello collegato** | flag |
| 202/203 | SPL/SPH | limiti setpoint (16,0 / 28,0) | ×0,1 |
| 210-215 | MVV5..MVV1, MVVP | tabella velocità % (30/40/50/75/85/95 osservati) | % |
| 221/222 | ACL / ACL_TIM | manutenzione: soglia ore / contatore (222 è R/W: scrivere 0 = reset) | h |
| 224 | CFG | 64 supervisione / 66 CNV (vedi sopra) | |
| 231 | SET_BG | setpoint — solo memoria con pannello | ×0,1 |
| 233 | Man | 0 auto / 3 inverno / 5 estate | |
| 242-244 | OS1-3 | offset sonde | ×0,1 K |
| 247 | WEB | flag restrizioni tastiera (vedi manuale) | flag |

## Installazione

1. Nello YAML del dispositivo:

   ```yaml
   external_components:
     - source:
         type: git
         url: https://github.com/automjordan/esphome-modbus-ascii
         ref: main
   ```

2. Copia `example/vmc-khrv.yaml`, adatta WiFi e pin, flasha.
3. Nei log cerca `Modbus ASCII master: Indirizzo slave: 1` e i sensori che si popolano.
   Se ricevi solo spazzatura, prova prima `inverted: true` su RX e TX.
4. Verifica: `ID prodotto` deve leggere 1115.

## Il componente `modbus_ascii`

```yaml
modbus_ascii:
  id: vmc
  uart_id: bus485          # 9600 7E1
  address: 1
  flow_control_pin: GPIO21
  update_interval: 15s
  command_throttle: 150ms
  response_timeout: 500ms

sensor:
  - platform: modbus_ascii
    modbus_ascii_id: vmc
    register: 0
    signed: true           # opzionale
    passive: true          # opzionale: non interrogato, solo ascolto del bus
```

API disponibili nelle lambda:

| Metodo | Uso |
|---|---|
| `write_register(reg, value)` | FC06, accodato con priorità |
| `read_registers(reg, qty)` | FC03 accodato (usare qty=1 su questa scheda) |
| `probe(addr, reg)` | FC03 verso un indirizzo arbitrario, risultato nel log |
| `get_register(reg, out)` | ultimo valore letto (per read-modify-write) |
| `set_override(bool)` / `get_override()` | emulazione del pannello on/off |
| `set_desired_mode(code)` | nibble di velocità per il broadcast emulato |
| `set_desired_setpoint(v)` | setpoint ×10 per il broadcast emulato |
| `send_panel_frame(force)` | invia subito il broadcast emulato |

Il componente ascolta sempre il bus: cattura i broadcast altrui (FC16/FC06) e le risposte
alle letture di altri master, e li pubblica sui sensori marcati `passive`.

## Dashboard Home Assistant

In `example/dashboard-vmc.yaml` c'è una card Mushroom con popup browser_mod: comandi,
stato, allarmi, scheduler (HACS *Scheduler component* + *Scheduler card*) e cronologia.

Logica di convivenza con il tastierino, tutta nel firmware (`example/vmc-khrv.yaml`):

- un comando da HA (velocità o setpoint) **accende** il controllo HA;
- un tasto premuto sul tastierino **spegne** il controllo HA e il pannello riprende.

### Contatore filtri

Il contatore ore della scheda (reg 222) resta a zero finché `ACL` (reg 221) vale 0.
L'esempio conta nel firmware le ore con motore acceso (una scrittura in flash all'ora),
espone `Ore filtri`, una soglia configurabile (`Intervallo cambio filtri`, default 8760 h),
l'allarme `Cambio filtri` con notifica persistente in HA e il bottone `Filtri sostituiti`.
Serve abilitare "Consenti al dispositivo di eseguire azioni di Home Assistant" nelle
opzioni dell'integrazione ESPHome. In alternativa, scrivendo 8760 nel reg 221 è la scheda
stessa a contare e a segnalare "Filtri sporchi" anche sul tastierino.

## Cose che NON funzionano (e perché)

| Tentativo | Esito |
|---|---|
| Modbus RTU con jumper su RTU | il pannello muore; la bridge non ha risposto a nessun indirizzo 1-8 / 9600 8N1 (dip A-D non documentati) |
| Collegarsi ai morsetti `PC` del pannello come porta separata | sono lo stesso bus del FAN COIL |
| Il pannello come slave Modbus (opzione 2 del manuale) | nessuna risposta FC03 su nessun indirizzo 2-247 |
| Scrivere 224=64 per prendere il controllo | la scheda rimette 66 entro ~1 s |
| FC03 con `qty > 1` | dati disallineati, usare sempre qty=1 |
| `modbus_controller` nativo di ESPHome | parla solo RTU |

## Risoluzione problemi

- **Solo `Timeout`, mai un byte** → polarità A/B, oppure stai parlando RTU a un bus ASCII.
- **Spazzatura che si dimezza dimezzando il baud** → A e B invertiti, non il baud sbagliato.
- **Letture incoerenti (SPL > SPH, MVV > 100)** → letture multiple, forza qty=1.
- **Le scritture "funzionano" ma la macchina non cambia** → registro 224 a 66, usa l'emulazione.
- **Timeout sporadici ogni 30 s** → collisione con l'handshake del pannello, innocuo.

## Documentazione del costruttore

In `docs/manuali-costruttore/` ci sono i due PDF usati: il manuale KHR-V (01/2026) e il
manuale del kit Modbus (cod. 0180415 rev.02). **Entrambi riportano un divieto di
riproduzione**: sono qui per uso personale e la cartella è esclusa dal repository
tramite `.gitignore`. Il manuale KHR-V è scaricabile da giacomini.com; il manuale Modbus
va richiesto al rivenditore.

## Crediti

- Discussione indomus [#2464](https://github.com/indomus/forum/discussions/2464): mappa
  registri Riello, esperienze con e senza pannello, conferma che con il pannello la via RTU
  è chiusa.
- [jjdejong/esphome-airleaf](https://github.com/jjdejong/esphome-airleaf) e
  [lorenzo93/homeassistant-innova](https://github.com/lorenzo93/homeassistant-innova):
  stessa elettronica via RTU senza pannello.
- Documentazione Waveshare e [Sleeper85/esphome-yambms](https://github.com/Sleeper85/esphome-yambms)
  per la piedinatura della ESP32-S3-RS485-CAN.

Licenza MIT. Usatelo, miglioratelo, e dormite.
