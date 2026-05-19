# ArduinoZoneDetect

Bibliothèque PlatformIO/Arduino permettant de déterminer le fuseau horaire (IANA timezone ID) à partir de coordonnées GPS (latitude/longitude en degrés), avec gestion de l'heure d'été/d'hiver (DST).

## Fonctionnalités

- Lecture du fichier binaire `timezone21.bin` ou `timezone16.bin` (format [ZoneDetect](https://github.com/BertoldVdb/ZoneDetect)) depuis une carte SD (ou tout autre `Stream` seekable)
- Retourne le timezone ID IANA (ex. `"Europe/Paris"`)
- Calcul de l'offset UTC courant incluant le DST via un fichier `dst_rules.bin` généré par un script Python
- Compatible Arduino et ESP32
- Faible empreinte mémoire : zéro allocation dynamique dans le chemin critique

## Fichiers requis sur la carte SD

| Fichier | Source |
|---|---|
| `timezone21.bin` ou `timezone16.bin` | Répertoire `database/out/` du dépôt [BertoldVdb/ZoneDetect](https://github.com/BertoldVdb/ZoneDetect/tree/master/database) (**utiliser `out/`, pas `out_v1/`**) |
| `dst_rules.bin` | Généré par `tools/generate_dst_bin.py` |

> **Important :** utilisez uniquement les fichiers du répertoire `out/` (version 0).
> Le répertoire `out_v1/` contient des fichiers compressés (version 1) qui ne sont pas pris en charge.

## Utilisation rapide

```cpp
#include <SD.h>
#include "ZoneDetect.h"
#include "DST.h"

File tzFile = SD.open("/timezone21.bin");
ZDFileReader<File> tzReader(tzFile);

ZoneDetect zd;
zd.begin(&tzReader);

char tzId[48];
zd.getTimezoneId(48.8584f, 2.2945f, tzId, sizeof(tzId));
// tzId == "Europe/Paris"

File dstFile = SD.open("/dst_rules.bin");
ZDFileReader<File> dstReader(dstFile);

DST dst;
dst.begin(&dstReader);

int32_t utcNow = 1711846800L;  // 2024-03-31 01:00:00 UTC
int32_t offset = dst.getUtcOffset(tzId, utcNow);  // +7200 (CEST = UTC+2)
int32_t local  = dst.toLocalTime(tzId, utcNow);   // local Unix timestamp
```

## API

### ZoneDetect

```cpp
bool begin(ZDReader *reader);
int  lookup(float lat, float lon, ZDMatch *out, uint8_t maxResults = 1);
bool getTimezoneId(float lat, float lon, char *tzId, size_t bufLen);
```

`ZDMatch` contient :
- `tzId[48]` – identifiant IANA (ex. `"America/New_York"`)
- `countryA2[3]` – code pays ISO-3166 (ex. `"US"`)

### DST

```cpp
bool    begin(ZDReader *reader);
bool    getPosixTz(const char *tzId, char *posixTz, size_t bufLen);
int32_t getUtcOffset(const char *tzId, int32_t unixTimestamp);
int32_t toLocalTime(const char *tzId, int32_t unixTimestamp);

// Utilisation sans fichier (chaîne POSIX TZ connue à l'avance)
static int32_t parsePosixTz(const char *posixTz, int32_t unixTimestamp);
```

### ZDReader / ZDFileReader

Abstraction seekable. Wrapper générique pour tout objet SD-like :

```cpp
ZDFileReader<File> reader(sdFile);   // File = SD.h File, SPIFFS File, etc.
```

Pour un source personnalisé, implémentez `ZDReader` :

```cpp
class MonReader : public ZDReader {
    bool     seek(uint32_t pos) override { ... }
    uint32_t position()         override { ... }
    size_t   readBytes(uint8_t *buf, size_t len) override { ... }
    uint32_t fileSize()         override { ... }
};
```

## Génération de dst_rules.bin

```bash
pip install pytz
cd tools
python3 generate_dst_bin.py -o dst_rules.bin
# Toutes les timezones (~600 entrées, ~40 Ko)

# Ou seulement les zones souhaitées :
python3 generate_dst_bin.py -z Europe/Paris America/New_York Pacific/Auckland -o dst_rules.bin
```

## Format de dst_rules.bin

```
4 octets  "DSTR"  (magic)
2 octets  uint16 LE  nombre d'entrées N
par entrée :
  uint8   longueur du tzId
  bytes   tzId (sans null)
  uint8   longueur de la chaîne POSIX TZ
  bytes   chaîne POSIX TZ (sans null)
```

## Précision

- `timezone21.bin` : précision ~100 m (recommandé)
- `timezone16.bin` : précision ~3 km (fichier plus petit, adapté aux microcontrôleurs avec moins de mémoire Flash/SD)

## Dépendances

Aucune dépendance externe. Nécessite uniquement la bibliothèque `SD` (incluse dans l'IDE Arduino / PlatformIO) pour la lecture de fichiers.

## Licence

MIT
