# Diseño e Implementación del Parche de sxiv para Carpetas y Archivos Comprimidos

## 1. Objetivos del Parche

1. **Lectura Nativa de Carpetas (Directorios):**
   - Permitir abrir directorios de manera natural (`sxiv /ruta/carpeta` o `sxiv .`).
   - Escaneo inteligente y filtrado automático de formatos de imagen válidos.
   - Exploración recursiva o plana con ordenamiento natural (*natural alphanumeric sort*).

2. **Soporte de Archivos de Cómic:**
   - Soporte transparente para `.cbz`, `.cb7`, `.cbr`, `.zip`, `.7z`, `.tar`.
   - Detección automática al abrir el archivo o al navegar dentro de directorios que contienen cómics.

3. **Soporte de Contenedores de Novelas Visuales (Estilo GARbro):**
   - Soporte nativo para contenedores de recursos de novelas visuales:
     - **KiriKiri XP3** (`*.xp3`)
     - **CatSystem 2 INT** (`*.int`)
     - **BGI/Ethornell ARC** (`*.arc`)
     - Contenedores simples PAK / DAT.

4. **Acceso de Latencia Cero y Carga en Memoria:**
   - No extraer todo el contenedor a disco.
   - Carga bajo demanda utilizando `imlib_load_image_mem()`.
   - Generación de miniaturas (*thumbnails*) directamente desde el flujo de memoria.

---

## 2. Arquitectura de Módulos para sxiv

Se incorporan los siguientes componentes al código base de `sxiv`:

```
sxiv/src/sxiv-26/
├── archive.h       <-- Cabecera con estructuras arch_t, arch_entry_t y APIs
├── archive.c       <-- Implementación de loaders, probing, streaming y destructores
├── main.c          <-- Integración de escaneo de contenedores y carga de archivos
├── image.c         <-- Soporte para decodificación desde buffer de memoria
├── thumbs.c        <-- Generación de miniaturas para entradas de archivo
├── Makefile        <-- Enlace con -larchive y -lz
└── config.def.h    <-- Opciones de configuración
```

### 2.1 La Abstracción `arch_entry_t` en `fileinfo_t`

En `sxiv.h`, la estructura principal que representa una imagen es `fileinfo_t`:

```c
typedef struct {
    const char *name;      /* Nombre presentado al usuario (e.g. "comic.cbz:cap1/01.jpg") */
    const char *path;      /* Ruta física al archivo en el disco */
    fileflags_t flags;
    /* Campos extendidos para contenedores */
    struct arch *archive;  /* Puntero al contexto de archivo contenedor (si proviene de uno) */
    size_t arch_entry_idx; /* Índice de la entrada dentro del contenedor */
} fileinfo_t;
```

---

## 3. Ciclo de Vida: Apertura, Visualización y Cierre

1. **Apertura del Visor (`main()`):**
   - Cuando el usuario ejecuta `sxiv <ruta>`:
     - Si es un directorio -> `scan_directory()` añade todas las imágenes y recursa en subcarpetas. Si encuentra archivos `.cbz`, `.xp3`, etc., los inspecciona y registra sus imágenes contenidas.
     - Si es un archivo regular -> `archive_probe()` comprueba si es un contenedor soportado. Si lo es, el Loader lee la tabla de contenidos e inserta cada imagen interna en `files[]`.
     - Si es una imagen normal -> Se añade directamente como hasta ahora.

2. **Carga de Imagen (`img_open()` en `image.c`):**
   ```c
   if (file->archive != NULL) {
       void *mem_buf = NULL;
       size_t mem_size = 0;
       if (archive_extract_entry(file->archive, file->arch_entry_idx, &mem_buf, &mem_size) == 0) {
           im = imlib_load_image_mem(file->name, mem_buf, mem_size);
           free(mem_buf);
       }
   } else {
       im = imlib_load_image(file->path);
   }
   ```

3. **Generación de Miniaturas (`tns_load()` en `thumbs.c`):**
   - Se utiliza el mismo mecanismo de extracción bajo demanda hacia `imlib_load_image_mem()`, escalando la miniatura en memoria y guardando la caché de miniaturas estándar si está habilitada.

4. **Destructor (`cleanup()` en `main.c`):**
   - Al cerrar la aplicación, se invoca `archive_cleanup_all()`, liberando cada `arch_t`, cerrando descriptores de archivo y liberando la memoria asignada para las tablas de entradas.
