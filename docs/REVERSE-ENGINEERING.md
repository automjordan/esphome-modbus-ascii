# Come ci siamo arrivati

Diario ragionato del reverse engineering, perché il percorso è più utile del risultato
a chi si trova davanti la stessa scheda con un pannello diverso o un firmware diverso.

## 1. Il punto di partenza sbagliato

Il kit Modbus del costruttore (cod. 0180415) descrive: scheda bridge nello slot a 7 pin,
jumper su RTU, 9600 8N1, funzioni 03 e 06, registro 224 a 64. Il manuale KHR-V aggiunge
che con il selettore su RTU "il pannello di comando smetterà di rispondere".

Con quel setup, scansionando gli indirizzi 1-8 a 9600 8N1 su `−BA+` della scheda, nessuna
risposta. Invertendo A/B, nessuna risposta. Il thread indomus #2464 conferma: chi ha il
pannello a muro non legge nulla, chi non ce l'ha sì. Un partner Riello dichiara che serve
"il controller giusto". I dip A-D della bridge non sono documentati.

Conclusione provvisoria (sbagliata): il Modbus non è raggiungibile con il pannello.

## 2. Lo sniffer e la trappola della polarità

Ascoltando il bus pannello↔scheda con una UART in 8N1 a 9600 arrivavano burst di ~30 byte
ogni 10 s, mai decodificabili. A 19200 ~50 byte, a 4800 15, a 2400 7, a 1200 4: la
lunghezza si dimezzava dimezzando il baud. Sembrava un segnale non seriale.

Era invece **A/B invertiti**: con la polarità sbagliata la UART vede idle=0, non aggancia
gli start bit e produce spazzatura proporzionale alla durata del burst a qualunque velocità.
Il segnale rivelatore: byte isolati `00` e `FF` (break) tra un burst e l'altro.

Con `rx_pin: {inverted: true}` la prima frame leggibile:

```
3A 30 30 B1 30 30 30 36 35 30 30 30 33 30 36 35 30 B1 B1 30 30 44 B2 30 B1 30 36 B4 B8 8D 0A
```

I byte con bit 7 acceso (`B1`, `B2`, `B4`, `B8`, `8D`) sono il **bit di parità** letto come
dato: il formato è **7E1**. Tolto il bit 7: `:00100065000306501100D2010648` — Modbus ASCII,
FC16 broadcast, registri 101-103, LRC corretto.

## 3. Chi parla con chi

| Frame | Cadenza | Significato |
|---|---|---|
| `:0010 0065 0003 06 xxxx yyyy zzzz LRC` | 10 s + a ogni tasto | pannello → tutti: velocità, setpoint, temperatura |
| `:77FF00000001 89` / `:77FF02xxxx` | 30 s | handshake proprietario, indirizzo 0x77 funzione 0xFF |
| `:0103 00C7 0001 34` → `:0103 02 045B 9B` | su richiesta | la scheda risponde a FC03 all'indirizzo 1 con ID 1115 |

La sonda manuale all'indirizzo 1 è stata il momento in cui tutto si è sbloccato.
L'indirizzo 0x77 non risponde a FC03: è un canale privato.

## 4. Le letture multiple mentono

Con FC03 e `qty > 1` la scheda risponde ma i valori sono disallineati: SPL 28,0 e SPH 24,0,
MVV1 = 200 %, ID = 160. Con `qty = 1` tutto torna (ID 1115, SPL 16,0, SPH 28,0, catena
MVV 30/40/50/75/85/95). Il manuale lo diceva: "lettura e scrittura di **un singolo**
registro".

## 5. Le scritture che non comandano

FC06 su 201 (PRG) e 231 (setpoint): echo confermato, valore persistente in lettura, ma
motore e setpoint attivo (reg 8) non cambiano. Il registro 224 legge 66. Scrivendo 64,
la scheda rimette 66 entro un secondo — non al broadcast successivo, ma per conto suo,
finché sente il pannello. Il controllo è esclusivo del CNV.

Scansione ASCII degli indirizzi 2-247 sul registro 199: nessun altro slave. Il pannello
non è interrogabile (l'"opzione 2" del manuale non vale per questa coppia).

## 6. Il broadcast come canale di comando

Premendo i tasti del pannello si vede il nibble basso del registro 101 cambiare e il
motore seguire al ciclo dopo. Inviando noi lo stesso FC16 broadcast con il nibble scelto,
la scheda esegue: `0x5013` → 75 %, `0x5014` → 85 %, `0x5012` → 30 %. Il setpoint in 102
viene applicato immediatamente (reg 8 lo segue).

Il pannello ripete il suo broadcast ogni 10 s; il componente ri-emette il proprio subito
dopo. La scheda esegue l'ultimo ricevuto: per 9,9 s su 10 comanda Home Assistant. Il
display del pannello mostra la *sua* modalità, non quella reale — è il compromesso.

## 7. Mappa dei tasti

Premendo in sequenza AUTO, 🌙, 🌀, 🌀🌀 il registro 101 vale `0x5010`, `0x5012`, `0x5011`,
`0x5013`. Il codice `4` (`0x5014`, 85 % = MVV1) non ha un tasto: è il booster.

## 8. Dettagli che hanno fatto perdere tempo

- Il glitch `\x00` ricevuto ~25 ms dopo ogni trasmissione è il rilascio del DE, non una
  risposta.
- `uart.write` ritorna prima che il FIFO sia vuoto: un `delay` di 25 ms tagliava l'ultimo
  carattere. Nel componente si usa `flush()`.
- Il `flow_control_pin` della Waveshare è attivo alto: l'ipotesi "invertito" era sbagliata
  e ha reso sordo il ricevitore per un giro intero di prove.
- Le entità rinominate in ESPHome prendono un nuovo `entity_id` (prefisso area + friendly
  name); quelle vecchie restano con il prefisso corto. Controllare sempre gli id reali.
- ESPHome con `refresh: 1d` sugli `external_components` non vede i push: usare `0s` dopo
  ogni modifica al componente.
