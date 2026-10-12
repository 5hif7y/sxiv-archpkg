# Arquitectura de Loaders y Destructores para Contenedores y Archivos

## 1. Introducción y Conceptos Fundamentales

En el contexto de visores de imágenes, motores de novelas visuales y sistemas de archivos virtuales (VFS), los términos **Loader** (Cargador) y **Destructor** (Liberador / Finalizador) definen el ciclo de vida completo de un recurso contenedor (archivo comprimido, paquete empaquetado o directorio de imágenes).

```
+-------------------------------------------------------------------------+
|                        CICLO DE VIDA DEL ARCHIVO                        |
+-------------------------------------------------------------------------+
       |
       v
[ 1. Probing / Identificación ] ---> Firma mágica, extensión, heurística
       |
       v
[ 2. Loader (Cargador) ]       ---> Deserialización de TOC (Tabla de Contenidos)
       |                            Descompresión / Desencriptación del Índice
       |                            Construcción de estructuras en memoria
       v
[ 3. Virtual File System ]     ---> Mapeo jerárquico o plano de entradas
       |
       v
[ 4. Entry Loader (Streaming)] ---> Acceso aleatorio / Extracción bajo demanda
       |                            Decodificación de imagen a memoria (Imlib2)
       v
[ 5. Destructor ]              ---> Liberación de tablas, buffers temporales,
                                    cierre de streams y descriptores de archivo.
```

---

## 2. Anatomía y Estructuras de un Loader (Cargador)

El **Loader** es la unidad de código responsable de:
1. Validar si un archivo corresponde al formato gestionado.
2. Leer y deserializar los metadatos y la tabla de contenidos (TOC / Central Directory).
3. Construir una representación en memoria optimizada para indexación $O(1)$ o $O(\log N)$.
4. Proporcionar la interfaz de lectura o *stream* de descompresión para cada entrada individual.

### 2.1 Componentes Estructurales en C

```c
typedef struct arch_entry {
    char *name;               /* Ruta relativa interna (UTF-8) */
    uint64_t offset;          /* Posición del payload de datos en el archivo */
    uint64_t comp_size;       /* Tamaño comprimido en disco */
    uint64_t uncomp_size;     /* Tamaño real descomprimido */
    uint32_t flags;           /* Banderas: comprimido, cifrado, directorio */
    void *extra;              /* Metadatos específicos del motor */
} arch_entry_t;

typedef struct arch_ops {
    const char *format_tag;
    const char *description;
    bool (*probe)(const uint8_t *magic, size_t magic_len, const char *path);
    int (*open)(struct arch *a, const char *path);
    int (*read_entry)(struct arch *a, size_t entry_index, void **out_buf, size_t *out_size);
    void (*close)(struct arch *a);
} arch_ops_t;

typedef struct arch {
    const arch_ops_t *ops;    /* Puntero a la tabla de operaciones */
    char *filepath;           /* Ruta del archivo en el sistema de archivos */
    int fd;                   /* Descriptor de archivo POSIX */
    FILE *fp;                 /* Stream C estándar */
    void *mapped_data;        /* Puntero mmap() para acceso de latencia cero */
    size_t file_size;         /* Tamaño total del archivo contenedor */
    arch_entry_t *entries;    /* Lista plana de archivos indexados */
    size_t entry_count;       /* Número total de entradas */
    void *priv_data;          /* Contexto privado del motor (e.g. zlib, llaves) */
} arch_t;
```

### 2.2 Fases de Ejecución del Loader

1. **Fase de Detección (Probing):**
   - Lectura de los primeros bytes del archivo (firma binaria o *magic number*).
   - Detección de cabeceras ubicadas al final del archivo (como el *End of Central Directory* en ZIP/CBZ).
   - Verificación de límites de archivo y comprobación de sanidad de la cantidad de entradas.

2. **Fase de Deserialización de Cabecera y Tabla de Contenidos:**
   - Si el índice está comprimido (e.g., ZLIB en KiriKiri XP3, NPA de Nitro+, o AFA de AliceSoft), el Loader instancia un flujo de descompresión en memoria sobre el bloque del índice.
   - Si el índice está ofuscado o cifrado (e.g., cadenas XOR rotativas o llaves Blowfish/Mersenne Twister), se aplica el descifrado correspondiente a la tabla de nombres y offsets.

3. **Fase de Mapeo de Entradas y Normalización:**
   - Cada archivo contenido se convierte en una entrada `arch_entry_t`.
   - Conversión de codificaciones de caracteres (Shift-JIS / CP932 comúnmente encontradas en novelas visuales japonesas hacia UTF-8 estándar).
   - Normalización de separadores de ruta (`\\` a `/`).

---

## 3. Anatomía y Estructuras de un Destructor

A diferencia de entornos administrados como .NET/C# (donde GARbro delega en el recolector de basura `GC` y la interfaz `IDisposable`), en lenguaje **C** la destrucción de recursos debe ser rigurosamente explícita y determinista. Cualquier fallo en la destrucción causa *memory leaks* (fugas de memoria) o agotamiento de descriptores de archivo (*FD leak*).

### 3.1 Responsabilidades del Destructor

1. **Liberación de Memoria Dinámica de Entradas:**
   - Iteración sobre cada elemento en `arch->entries`:
     - `free(entry->name)`
     - Si la entrada contiene arrays dinámicos de segmentos o bloques: `free(entry->extra)`
   - `free(arch->entries)`
   - `arch->entries = NULL; arch->entry_count = 0;`

2. **Liberación de Datos Específicos del Formato (`priv_data`):**
   - Liberación de tablas de sustitución de cifrado o llaves generadas.
   - Finalización de streams de bibliotecas externas (e.g. `inflateEnd(&zstream)` en Zlib, o `archive_read_free(a)` en libarchive).
   - `free(arch->priv_data)`

3. **Liberación de Recursos a Nivel de Sistema Operativo:**
   - Si se utilizó memoria mapeada: `munmap(arch->mapped_data, arch->file_size)`
   - Si se utilizó un descriptor POSIX: `close(arch->fd)`
   - Si se utilizó un stream de C: `fclose(arch->fp)`

4. **Destrucción de Buffers Temporales de Imagen:**
   - Los buffers de descompresión bajo demanda entregados al visor de imágenes deben ser liberados tan pronto como `imlib_load_image_mem()` termina de generar la textura del framebuffer X11 / Imlib.

---

## 4. Comparativa de Arquitectura: GARbro (.NET/C#) vs sxiv (C)

| Característica | Modelo GARbro (.NET / C#) | Modelo sxiv (C / POSIX) |
| :--- | :--- | :--- |
| **Abstracción Base** | `ArchiveFormat` (Clase abstracta) | `arch_ops_t` (Estructura de punteros a funciones / vtable en C) |
| **Representación de Archivo** | `ArcFile : IDisposable` | `arch_t *` asignado en el heap |
| **Acceso a Datos** | `ArcView` (Memory-Mapped File VFS) | `mmap()` o `pread()` directo |
| **Extracción de Entrada** | `OpenEntry(arc, entry)` -> `Stream` | `read_entry(arc, idx, &buf, &sz)` -> buffer en RAM |
| **Decodificación de Imagen** | `OpenImage()` -> `IImageDecoder` | `imlib_load_image_mem()` (Imlib2 nativo) |
| **Destructor** | `Dispose()` y Finalizador (`~ArcFile()`) | `arch_close(arch_t *a)` explícito |
| **Manejo de Cómics** | No prioritario / complementario | Integración nativa de alto rendimiento vía `libarchive` |
| **Formatos VN** | 400+ clases compiladas en DLL | Parsers C modulares para los formatos más prevalentes |

---

## 5. Estrategias de Carga en Memoria para Visores de Alto Rendimiento

Para un visor de imágenes ligero y veloz como `sxiv`, existen tres estrategias de carga posibles:

1. **Extracción Completa a Directorio Temporal (Enfoque Tradicional - Desaconsejado):**
   - Descomprimir todo el contenido en `/tmp` al abrir el archivo.
   - *Desventajas:* Si un archivo de novela visual pesa 4 GB con 5000 imágenes, el usuario debe esperar minutos antes de ver la primera imagen, saturando el disco.

2. **Acceso Aleatorio Bajo Demanda (On-Demand Random Access - Enfoque Óptimo Implementado):**
   - El Loader únicamente analiza el índice del archivo (generalmente unos pocos kilobytes o megabytes).
   - Se llena la lista de archivos de `sxiv` con referencias a las imágenes interiores.
   - Cuando el usuario navega a la imagen $N$, el Loader busca el offset de la entrada $N$, descomprime ese único flujo en un buffer de memoria volátil y se lo entrega a `imlib_load_image_mem()`.
   - El buffer se libera inmediatamente.
   - *Ventajas:* Tiempo de apertura instantáneo (< 50 milisegundos) y consumo de memoria mínimo (< 30 MB).
