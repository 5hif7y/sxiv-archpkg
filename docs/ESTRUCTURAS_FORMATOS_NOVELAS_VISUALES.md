# Especificación Técnica de Estructuras: Formatos de Novelas Visuales (GARbro)

## 1. Introducción

Las novelas visuales japonesas utilizan una amplia variedad de formatos de archivo propietarios o modificados para empaquetar fondos (*Backgrounds*), sprites de personajes (*Character CG*), eventos (*Event CG*), audio y scripts.

Este documento desglosa en detalle las estructuras binarias de cabecera, tabla de contenidos, procedimientos de Loader y Destructor correspondientes a los motores más prominentes documentados en el código fuente de **GARbro**.

---

## 2. KiriKiri / TVP (Formato XP3)

El motor KiriKiri (y su variante KiriKiri Z / TVP) es uno de los más populares del género (utilizado en *Fate/stay night*, *Tsukihime Remake*, *G Senjou no Maou*, títulos de *Yuzusoft*, etc.).

### 2.1 Cabecera y Detección

- **Firma Mágica (11 bytes):**
  ```hex
  58 50 33 0D 0A 20 0A 1A 8B 67 01
  ('X', 'P', '3', 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01)
  ```
- **Offset del Índice (TOC Offset):**
  - Ubicado en el byte `0x0B` (offset 11): Entero sin signo de 64 bits en Little Endian (`uint64_t`).
  - Apunta a la ubicación física de la cabecera de la tabla de contenidos.

### 2.2 Estructura del Índice (TOC Header)

Al desplazarse al `TOC Offset`:
1. `uint8_t header_type`:
   - `0x00`: Índice no comprimido. A continuación se leen 8 bytes con el tamaño total del índice (`uint64_t header_size`), seguido inmediatamente por el flujo de datos.
   - `0x01`: Índice comprimido con ZLIB. Le siguen 8 bytes de tamaño comprimido (`uint64_t packed_size`) y 8 bytes de tamaño descomprimido (`uint64_t unpacked_size`). El flujo ZLIB comienza en el offset actual + 17.

### 2.3 Chunks del Índice XP3

El índice deserializado es una secuencia continua de bloques (*chunks*). Cada bloque posee:
- `uint32_t chunk_magic` (4 bytes ASCII).
- `int64_t chunk_size` (8 bytes con el tamaño del contenido del chunk).

Los chunks principales son:
- **`File` (`0x656C6946`):** Representa un archivo archivado. Contiene sub-chunks en su interior:
  - **`info` (`0x6F666E69`):** Metadatos:
    - `uint32_t flags`: Bandera de cifrado.
    - `uint64_t uncompressed_size`: Tamaño real.
    - `uint64_t compressed_size`: Tamaño en disco.
    - `uint16_t name_length`: Cantidad de caracteres en UTF-16LE.
    - `uint16_t name[name_length]`: Cadena de caracteres en UTF-16LE.
  - **`segm` (`0x6D676573`):** Segmentos de datos (un archivo puede estar dividido en varios segmentos):
    - `uint32_t is_compressed`: 1 si está comprimido con ZLIB, 0 si está plano.
    - `uint64_t segment_offset`: Posición absoluta de inicio de datos.
    - `uint64_t segment_unpacked_size`.
    - `uint64_t segment_packed_size`.
  - **`adlr` (`0x726C6461`):** Checksum Adler-32 de 32 bits.

### 2.4 Loader y Destructor de XP3 en C

```c
typedef struct xp3_entry {
    char *filename_utf8;
    uint64_t offset;
    uint64_t comp_size;
    uint64_t uncomp_size;
    bool is_compressed;
    bool is_encrypted;
} xp3_entry_t;

int xp3_loader(arch_t *a, const char *path) {
    uint8_t magic[11];
    pread(a->fd, magic, 11, 0);
    if (memcmp(magic, "XP3\r\n \n\x1a\x8b\x67\x01", 11) != 0)
        return -1;
    
    uint64_t toc_offset;
    pread(a->fd, &toc_offset, 8, 11);
    
    uint8_t header_type;
    pread(a->fd, &header_type, 1, toc_offset);
    
    /* Descomprimir índice Zlib a buffer en memoria */
    /* Deserializar chunks "File", "info", "segm" */
    /* Poblar lista a->entries */
    return 0;
}

void xp3_destructor(arch_t *a) {
    for (size_t i = 0; i < a->entry_count; i++) {
        free(a->entries[i].name);
    }
    free(a->entries);
    a->entries = NULL;
    if (a->fd >= 0) close(a->fd);
}
```

---

## 3. CatSystem 2 (Formato INT)

Utilizado extensamente por empresas como Frontwing (*Grisaia no Kajitsu*, *Island*, *Corona Blossom*, etc.).

### 3.1 Cabecera y Estructura

- **Firma Mágica (4 bytes):**
  `'K', 'I', 'F', 0x00` (`0x0046494B`).
- **Cantidad de Entradas (Entry Count):**
  Offset `0x04`: `uint32_t entry_count`.
- **Estructura de la Tabla de Entradas:**
  Comienza en el offset `0x08`. Cada registro de archivo posee un tamaño fijo:
  - Variante clásica: 32 bytes de nombre + 8 bytes de metadatos (Total 40 bytes).
  - Variante extendida: 64 bytes de nombre + 8 bytes de metadatos (Total 72 bytes).
  
  Estructura por entrada:
  - `char filename[32 o 64]`: Nombre codificado en CP932 (Shift-JIS) terminado en null.
  - `uint32_t file_offset`: Desplazamiento absoluto en el archivo contenedor.
  - `uint32_t file_size`: Longitud del archivo.

### 3.2 Cifrado CatSystem (Blowfish + Mersenne Twister)

Si la primera entrada del índice es `__key__.dat`, el archivo se encuentra protegido:
1. El archivo contiene un valor de semilla (`seed`) en el offset `0x4C`.
2. Se inicializa un generador de números pseudoaleatorios Mersenne Twister (MT19937).
3. Se deriva la clave del algoritmo simétrico **Blowfish**.
4. La tabla de archivos restante y sus contenidos se descifran en bloques de 64 bits (8 bytes).

### 3.3 Formato de Imágenes Internas: HG2 y HG3

Los archivos de imagen dentro de los paquetes INT suelen ser `.hg2` o `.hg3`:
- Cabecera: Firma `HG-2` o `HG-3`.
- Bloques de compresión RLE/LZSS y ZLIB combinados.
- Planos de color RGBA divididos en franjas horizontales.

---

## 4. BGI / Ethornell (Formato ARC)

Utilizado en títulos de August, MangaGamer y Circus (*Fortune Arterial*, *Daitoshokan no Hitsujikai*, *Aokana*).

### 4.1 Estructura Binaria

- **Firma Mágica (12 bytes):**
  `"PackFile    "` (`0x6b636150` seguido de `"File    "`).
- **Cantidad de Entradas:**
  Offset `0x0C` (12): `uint32_t entry_count`.
- **Tabla de Entradas:**
  Inicia en offset `0x10` (16).
  Cada entrada tiene un tamaño exacto de 32 bytes (`0x20`):
  - `char filename[16]`: Nombre del archivo en CP932 (16 bytes).
  - `uint32_t offset`: Offset relativo a la base de datos (`base_offset = 0x10 + 32 * count`).
  - `uint32_t size`: Tamaño en bytes.
  - `uint8_t reserved[8]`: Padding o datos reservados.

---

## 5. Nitro+ (Formato PAK / NPA)

Utilizado por Nitroplus en clásicos como *Saya no Uta*, *Chaos;Head*, *Steins;Gate*, *Muramasa*.

### 5.1 Versión 2 y Versión 3

- **Versión (4 bytes):**
  Offset `0x00`: `uint32_t version` (`2` o `3`).
- **Entry Count:**
  Offset `0x04`: `uint32_t count`.
- **Tamaños del Índice:**
  Offset `0x08`: `uint32_t unpacked_index_size`.
  Offset `0x0C`: `uint32_t packed_index_size`.
- **Datos del Índice:**
  Inician en offset `0x114`.
  El bloque de tamaño `packed_index_size` está comprimido con ZLIB.
  Al descomprimirlo:
  - Bucle de entradas:
    - `int32_t name_len`.
    - `char name[name_len]`: Nombre ofuscado con una máscara de bytes XOR cíclica.
    - `uint32_t offset`.
    - `uint32_t size`.

---

## 6. AliceSoft (Formato AFA / ALD)

Utilizado por AliceSoft (*Rance series*, *Evenicle*, *Doushin*).

### 6.1 Estructura AFA

- **Firma Mágica (8 bytes):**
  `"AFAH"` seguido de `"AlicArch"` en offset `0x08`.
- **Cabecera INFO:**
  Offset `0x1C`: Bloque `"INFO"`.
  - `uint32_t version`.
  - `uint32_t base_offset`.
  - `uint32_t packed_size`.
  - `uint32_t unpacked_size`.
  - `uint32_t file_count`.
- El bloque de índice está comprimido con ZLIB y define las longitudes de pasos (*step lengths*), nombres y offsets.

---

## 7. Tabla Comparativa de Loaders y Destructores de Novelas Visuales

| Motor / Formato | Firma Mágica | Offset de Índice | Compresión de Índice | Algoritmo de Datos | Procedimiento de Destructor |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **KiriKiri (XP3)** | `XP3\r\n \n\x1a\x8b\x67\x01` | En cabecera (offset 11) | ZLIB (Tipo 1) | ZLIB / Plano | Liberar segmentos, strings UTF-8 y descriptores |
| **CatSystem (INT)** | `KIF\0` | Offset 8 fijo | Plano o Cifrado | HG2/HG3/ZLIB/Plano | Liberar llaves Blowfish/MT y tabla de entradas |
| **BGI / Ethornell** | `PackFile    ` | Offset 16 fijo | Plano | DSC / Plano | Liberar tabla y cerrar stream |
| **Nitro+ (NPA/PAK)**| `0x00000002` / `3` | Offset 0x114 | ZLIB | ZLIB / XOR / Plano | Liberar búfer ZLIB de índice y strings |
| **AliceSoft (AFA)** | `AFAH` | Bloque INFO | ZLIB | QNT/AJP/Plano | Liberar chunks y tabla de pasos |
| **Majiro (ARC)** | `MajiroArcV1.000` | En cabecera | Plano | Plano / Cifrado | Liberar tablas de hash CRC64 y nombres |
| **NScripter (NSA)** | Variable | Offset 0 / variable | Plano | SPB / LZSS / Plano | Liberar offsets relativos y buffers de scanline |
