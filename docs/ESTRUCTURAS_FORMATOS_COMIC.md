# Especificación Técnica de Estructuras: Formatos de Cómic (CBZ, CB7, CBR)

## 1. Introducción

Los formatos de archivo para cómics digitales son contenedores de compresión estándar que agrupan imágenes secuenciales (generalmente JPEG, PNG o WebP). Las extensiones principales son:
- **CBZ**: Comic Book Zip (basado en el estándar PKZIP).
- **CB7**: Comic Book 7-Zip (basado en el contenedor LZMA/LZMA2 de 7-Zip).
- **CBR**: Comic Book RAR (basado en el contenedor propietario RAR4 / RAR5).
- **CBT**: Comic Book Tar (archivo TAR sin comprimir o comprimido con gzip/bzip2/xz).

---

## 2. Formato CBZ (Comic Book Zip)

### 2.1 Arquitectura del Archivo PKZIP

Un archivo ZIP se estructura en tres secciones principales:
1. Registros de Archivos Locales (*Local File Headers* + *Compressed Data*).
2. Directorio Central (*Central Directory*).
3. Registro de Fin de Directorio Central (*End of Central Directory Record - EOCD*).

```
+-------------------------------------------------------------+
|              ESTRUCTURA DE UN ARCHIVO CBZ (ZIP)             |
+-------------------------------------------------------------+
| [Local File Header 1] [Compressed Data 1]                   |
| [Local File Header 2] [Compressed Data 2]                   |
| ...                                                         |
| [Local File Header N] [Compressed Data N]                   |
+-------------------------------------------------------------+
| [Central Directory File Header 1]                           |
| [Central Directory File Header 2]                           |
| ...                                                         |
| [Central Directory File Header N]                           |
+-------------------------------------------------------------+
| [End of Central Directory Record (EOCD)]                    |
+-------------------------------------------------------------+
```

### 2.2 Estructura Binaria del EOCD (End of Central Directory)

El Loader de un CBZ debe comenzar leyendo el **EOCD**, ubicado en los últimos bytes del archivo:

| Campo | Offset | Tamaño (Bytes) | Valor / Descripción |
| :--- | :---: | :---: | :--- |
| **Signature** | 0x00 | 4 | `0x06054B50` (`PK\x05\x06`) |
| **Disk Number** | 0x04 | 2 | Número del disco actual (0 para archivo único) |
| **Start Disk** | 0x06 | 2 | Número del disco donde comienza el Directorio Central |
| **Disk Entries** | 0x08 | 2 | Número de entradas en este disco |
| **Total Entries** | 0x0A | 2 | Número total de entradas en el Directorio Central |
| **Directory Size**| 0x0C | 4 | Tamaño en bytes del Directorio Central |
| **Directory Offset**| 0x10| 4 | Offset relativo donde inicia el Directorio Central |
| **Comment Length**| 0x14 | 2 | Longitud del comentario final ($L$) |
| **Comment** | 0x16 | $L$ | Cadena de comentario opcional |

### 2.3 Estructura Binaria del Central Directory Header

| Campo | Offset | Tamaño (Bytes) | Valor / Descripción |
| :--- | :---: | :---: | :--- |
| **Signature** | 0x00 | 4 | `0x02014B50` (`PK\x01\x02`) |
| **Version Made By** | 0x04 | 2 | Versión de software y sistema operativo origen |
| **Min Version** | 0x06 | 2 | Versión mínima de extractor requerida |
| **Flags** | 0x08 | 2 | Banderas de opciones (e.g. bit 0 = cifrado) |
| **Compression Method** | 0x0A | 2 | 0 = Store, 8 = Deflate, 12 = BZIP2, 14 = LZMA |
| **Mod Time & Date** | 0x0C | 4 | Marca de tiempo en formato MS-DOS |
| **CRC-32** | 0x10 | 4 | Checksum de redundancia cíclica de 32 bits |
| **Compressed Size** | 0x14 | 4 | Tamaño del payload comprimido en disco |
| **Uncompressed Size**| 0x18 | 4 | Tamaño real del archivo descomprimido |
| **File Name Length** | 0x1C | 2 | Longitud del nombre de archivo ($n$) |
| **Extra Field Length**| 0x1E | 2 | Longitud del campo de datos extra ($m$) |
| **Comment Length** | 0x20 | 2 | Longitud del comentario de archivo ($k$) |
| **Disk Number Start**| 0x22 | 2 | Disco de inicio |
| **Internal Attributes**|0x24 | 2 | Atributos internos |
| **External Attributes**|0x26 | 4 | Atributos externos de archivo POSIX / DOS |
| **Local Header Offset**|0x28 | 4 | Offset absoluto al *Local File Header* |
| **File Name** | 0x2C | $n$ | Nombre relativo / ruta del archivo |

### 2.4 Ciclo de Vida del Loader y Destructor CBZ

```c
/* Loader CBZ */
int cbz_loader(arch_t *a, const char *path) {
    /* 1. Abrir archivo y buscar firma EOCD retrocediendo desde el final */
    /* 2. Leer directory_offset y total_entries */
    /* 3. Alojar a->entries = calloc(total_entries, sizeof(arch_entry_t)) */
    /* 4. Iterar sobre cada Central Directory File Header */
    /* 5. Almacenar name, offset, comp_size, uncomp_size, comp_method */
    return 0;
}

/* Destructor CBZ */
void cbz_destructor(arch_t *a) {
    for (size_t i = 0; i < a->entry_count; i++) {
        free(a->entries[i].name);
    }
    free(a->entries);
    a->entries = NULL;
    if (a->fd >= 0) close(a->fd);
}
```

---

## 3. Formato CB7 (Comic Book 7-Zip)

### 3.1 Arquitectura del Contenedor 7-Zip

7-Zip utiliza una arquitectura modular basada en bloques y flujos codificados (*streams & coders*):
- **Start Header (32 bytes fijo):**
  - Firma mágica: 6 bytes `0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C` (`7z\xbc\xaf'\x1c`).
  - Versión mayor / menor (2 bytes: `0x00, 0x04` comúnmente).
  - CRC-32 del Start Header (4 bytes).
  - Offset relativo de la cabecera principal (*NextHeaderOffset*, 8 bytes, uint64_t).
  - Tamaño de la cabecera principal (*NextHeaderSize*, 8 bytes, uint64_t).
  - CRC-32 de la cabecera principal (4 bytes).

### 3.2 Coders y Descompresión

7z soporta compresión sólida (*solid archive*), donde múltiples archivos pequeños se concatenan en un único flujo antes de ser comprimidos con **LZMA** o **LZMA2**. 
- En archivos sólidos, el Loader debe poder decodificar secuencias continuas o utilizar `libarchive` como backend de descompresión optimizado con gestión interna de ventanas deslizantes.

### 3.3 Destructor CB7

El destructor de CB7 debe asegurar:
- Liberación del decodificador LZMA / LZMA2.
- Liberación de la memoria intermedia de carpetas (*Folder unpack buffers*).
- Cierre del descriptor del archivo origen.

---

## 4. Formato CBR (Comic Book RAR)

### 4.1 Bloques y Cabeceras en RAR

RAR (en versiones 4.x y 5.0) estructura sus datos en bloques encabezados por CRC, tipo y banderas:
- **Firma RAR4:** `0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00` (`Rar!\x1a\x07\x00`).
- **Firma RAR5:** `0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00` (`Rar!\x1a\x07\x01\x00`).
- Tipos de bloque:
  - `0x73`: Cabecera principal del archivo (*Archive Header*).
  - `0x74`: Cabecera de archivo/fichero (*File Header*).
  - `0x7B`: Fin de archivo (*End of Archive*).

### 4.2 Integración mediante `libarchive`

Dado que el formato RAR utiliza patentes y algoritmos de compresión complejos (Huffman adaptativo, PPMII y predicción LZ), la integración en entornos C se realiza de forma estandarizada y segura a través de `libarchive` (`archive_read_support_format_rar` y `archive_read_support_format_rar5`).
