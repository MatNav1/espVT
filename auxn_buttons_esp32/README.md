# ESP32U AUX-N — przyciski zwierane do masy

Firmware dla klasycznego ESP32-WROOM-32U i transceivera CAN 3,3 V.
Przyciski są wejściami AUX-N Type 2 (chwilowymi).

## Połączenia

| ESP32 | Moduł CAN |
|---|---|
| 3V3 | 3.3V |
| GND | GND |
| GPIO21 | CAN RX |
| GPIO22 | CAN TX |

Każdy przycisk podłącz pomiędzy skonfigurowany GPIO i GND. Firmware używa
wewnętrznego podciągania `INPUT_PULLUP`, więc dodatkowy rezystor nie jest
potrzebny do testów.

## Zmiana liczby przycisków

Edytuj tylko `config/buttons.json`. Liczba pozycji w tablicy `buttons`
automatycznie staje się liczbą wejść AUX-N. Przykład dla sześciu przycisków:

```json
{
  "buttons": [
    { "pin": 32, "label": "B1" },
    { "pin": 33, "label": "B2" },
    { "pin": 25, "label": "B3" },
    { "pin": 26, "label": "B4" },
    { "pin": 27, "label": "B5" },
    { "pin": 14, "label": "B6" }
  ]
}
```

Dozwolone piny: GPIO4, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27, 32 i 33.
Generator odrzuci powtórzone oraz ryzykowne piny. Piny GPIO21 i GPIO22 są
zarezerwowane dla CAN.

Przed każdą kompilacją PlatformIO automatycznie generuje konfigurację oraz
pulę obiektów VT. Ręcznie można ją odtworzyć poleceniem:

```sh
python3 tools/generate_config.py
```

Kompilacja i wgrywanie:

```sh
pio run -t upload
pio device monitor
```

## Uwagi sprzętowe

- CAN pracuje z prędkością 250 kbit/s.
- Rezystor 120 ohm na module CAN zostaw tylko na końcu własnej magistrali
  testowej. Przy podłączeniu do istniejącego ISOBUS zazwyczaj trzeba go usunąć.
- Nie zasilaj ESP32 ani modułu CAN bezpośrednio z 12 V.
- Kod producenta `1407` jest wartością testową, nie do produktu komercyjnego.

