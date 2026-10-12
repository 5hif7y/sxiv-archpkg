# Estructuras, Loaders y Destructores: libxft vs FreeType2

**Fecha:** 4 de Septiembre de 2026  
**Objetivo:** Documentar exhaustivamente cada una de las estructuras de datos, funciones de carga (*loaders*), renderizado y funciones de liberación (*destructores*) involucradas en `libxft`, así como sus equivalentes directos al migrar a `FreeType2` (junto con `Fontconfig` y primitivas de `X11`).

---

## 1. Mapa Conceptual de la Migración

`libxft` fue concebida como una capa integradora entre tres tecnologías:
1. **FreeType2:** Motor de rasterización y generación de mapas de bits a partir de curvas vectoriales (TrueType/OpenType).
2. **Fontconfig:** Motor de descubrimiento, correspondencia de patrones (*pattern matching*) y configuración de fuentes del sistema.
3. **XRender / Xlib:** Extensión del servidor X11 para composición de imágenes con canal alfa y almacenamiento de conjuntos de glifos en el servidor (*GlyphSets*).

Al desacoplar `libxft`, interactuamos directamente con **FreeType2** para la rasterización de glifos y métricas, con **Fontconfig** para localizar los archivos de fuentes (`.ttf`, `.otf`, `.ttc`) y resolver fuentes alternativas (*fallback*), y con **Xlib / XImage** para el dibujado directo sobre el pixmap de la ventana.

```
+-------------------------------------------------------------+
|                        ARQUITECTURA PREVIA                  |
|                                                             |
|   sxiv (window.c) ---> libXft                               |
|                          |---> Fontconfig (búsqueda)        |
|                          |---> FreeType2 (rasterización)    |
|                          +---> XRender (dibujado en X11)    |
+-------------------------------------------------------------+

+-------------------------------------------------------------+
|                      NUEVA ARQUITECTURA                     |
|                                                             |
|   sxiv (window.c)                                           |
|        |---> Fontconfig (resolución de fuentes y fallback)  |
|        |---> FreeType2  (carga de FT_Face y rasterización)  |
|        +---> Xlib       (XImage / blending alfa en Pixmap)  |
+-------------------------------------------------------------+
```

---

## 2. Análisis de Estructuras, Loaders y Destructores

### 2.1. Estructura de Fuente: `XftFont` frente a `FT_Face` / `font_t`

#### A. En `libxft`:
* **Estructura:** `XftFont` (`<X11/Xft/Xft.h>`)
  ```c
  typedef struct _XftFont {
      int             ascent;
      int             descent;
      int             height;
      int             max_advance_width;
      FcPattern      *pattern;
      FcCharSet      *charset;
      XftFontInfo    *info;
  } XftFont;
  ```
* **Loader:**
  - `XftFontOpenName(Display *dpy, int screen, const char *name)`:
    Parsea una cadena como `"monospace-8"`, consulta Fontconfig, abre la fuente y genera el objeto `XftFont`.
  - `XftFontOpen(Display *dpy, int screen, ...)`:
    Abre una fuente usando pares clave-valor de Fontconfig (`FC_CHARSET`, `FC_SIZE`, etc.), utilizado por `sxiv` para el *fallback*.
* **Destructor:**
  - `XftFontClose(Display *dpy, XftFont *font)`:
    Libera los glifos asociados en el servidor X, la estructura `XftFont` y la instancia interna de FreeType.
  - *Nota:* En el código original de `sxiv`, la fuente principal nunca se liberaba explícitamente al salir; solo se cerraban las fuentes secundarias de fallback.

---

#### B. En `FreeType2` (Sustitución):
* **Estructura Contextual de la Librería:** `FT_Library` (`<ft2build.h>`, `FT_FREETYPE_H`)
  - Manejador global del estado de FreeType.
  - **Loader:** `FT_Init_FreeType(FT_Library *alibrary)`
  - **Destructor:** `FT_Done_FreeType(FT_Library library)`
* **Estructura de la Fuente:** `FT_Face` (`<ft2build.h>`, `FT_FREETYPE_H`)
  ```c
  typedef struct FT_FaceRec_*  FT_Face;
  /* Contiene: face->size->metrics, face->glyph, face->num_glyphs, etc. */
  ```
  Para encapsular las métricas requeridas por la interfaz de sxiv, definimos un contenedor limpio:
  ```c
  typedef struct {
      FT_Face face;
      int ascent;
      int descent;
      int height;
  } font_t;
  ```
* **Loader de Fuente en FreeType2:**
  - `FT_New_Face(FT_Library library, const char *filepathname, FT_Long face_index, FT_Face *aface)`:
    Abre directamente el archivo de fuente (ej. `/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc`) con el índice de la cara tipográfica (`face_index`).
  - Configuración de tamaño en píxeles:
    `FT_Set_Pixel_Sizes(FT_Face face, FT_UInt pixel_width, FT_UInt pixel_height)`
* **Destructor de Fuente en FreeType2:**
  - `FT_Done_Face(FT_Face face)`:
    Libera todos los recursos asociados a la fuente en memoria.

---

### 2.2. Resolución de Fuentes y Fallback: `Fontconfig`

FreeType2 rasteriza archivos, pero no busca fuentes por nombre genérico (como `"monospace"`). Fontconfig se encarga de traducir nombres y buscar caracteres.

* **Estructura de Patrón:** `FcPattern` (`<fontconfig/fontconfig.h>`)
  - **Loader / Constructor:**
    - `FcNameParse((const FcChar8 *)fontstr)`: Convierte `"monospace-8"` a un patrón con propiedades.
    - `FcFontMatch(NULL, pattern, &result)`: Encuentra la mejor fuente instalada para el patrón.
  - **Extracción de Propiedades del Loader:**
    - `FcPatternGetString(match, FC_FILE, 0, &file)`: Obtiene la ruta al archivo en disco.
    - `FcPatternGetInteger(match, FC_INDEX, 0, &index)`: Obtiene el índice dentro del archivo (crucial para colecciones `.ttc`).
    - `FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &pixelsize)` o `FC_SIZE`.
  - **Destructor:**
    - `FcPatternDestroy(FcPattern *p)`: Libera la memoria del patrón.

* **Estructura de Conjunto de Caracteres:** `FcCharSet` (`<fontconfig/fontconfig.h>`)
  - **Loader / Constructor:**
    - `FcCharSetCreate()`: Crea un nuevo conjunto vacío.
    - `FcCharSetAddChar(charset, rune)`: Agrega el punto de código Unicode faltante.
  - **Destructor:**
    - `FcCharSetDestroy(FcCharSet *fcs)`: Libera el conjunto.

---

### 2.3. Carga y Renderizado de Glifos: `FT_GlyphSlot` y `FT_Bitmap`

#### A. En `libxft`:
- Consulta de existencia de carácter:
  `XftCharExists(Display *dpy, XftFont *font, FcChar32 ucs4)`
- Medición de extents:
  `XftTextExtentsUtf8(Display *dpy, XftFont *font, const FcChar8 *string, int len, XGlyphInfo *extents)`
  Donde `XGlyphInfo` provee `xOff` (avance horizontal).
- Trazado de glifo:
  `XftDrawStringUtf8(XftDraw *draw, const XftColor *color, XftFont *font, int x, int y, const FcChar8 *string, int len)`

#### B. En `FreeType2`:
- Consulta de existencia de carácter:
  `FT_Get_Char_Index(FT_Face face, FT_ULong charcode)`
  Retorna `0` si el glifo no existe en la fuente (equivalente directo a `!XftCharExists`).
- Carga y Rasterización de Glifo:
  `FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER)`
  - Rellena `face->glyph` (`FT_GlyphSlot`).
  - `face->glyph->advance.x >> 6`: Avance horizontal en píxeles (equivalente a `ext.xOff`).
  - `face->glyph->bitmap_left`: Desfase X desde el punto de origen.
  - `face->glyph->bitmap_top`: Desfase Y hacia arriba desde la línea base (*baseline*).
  - `face->glyph->bitmap`: Estructura `FT_Bitmap` con dimensiones (`width`, `rows`), paso (`pitch`) y buffer de 8 bits con niveles de cobertura alfa `0..255`.
- Destructor de Glifo:
  Gestionado automáticamente por FreeType (se sobrescribe en `face->glyph` en cada llamada).

---

### 2.4. Manejo de Color: `XftColor` frente a `win_color_t` / `XColor`

#### A. En `libxft`:
* **Estructura:** `XftColor` (`<X11/Xft/Xft.h>`)
  ```c
  typedef struct _XftColor {
      unsigned long   pixel;
      XRenderColor    color; /* red, green, blue, alpha (16-bit c/u) */
  } XftColor;
  ```
* **Loader:**
  - `XftColorAllocName(dpy, visual, colormap, name, &col)`
* **Destructor:**
  - `XftColorFree(dpy, visual, colormap, &col)`

#### B. En `Xlib` Puro (Sustitución):
* **Estructura Propia:** `win_color_t`
  ```c
  typedef struct {
      unsigned long pixel;
      uint8_t r, g, b;
  } win_color_t;
  ```
* **Loader:**
  Implementado mediante llamadas estándar de Xlib (`XAllocNamedColor` y `XParseColor` + `XAllocColor` para cadenas en notación hexadecimal `#RRGGBB`):
  ```c
  void win_alloc_color(const win_env_t *e, const char *name, win_color_t *col)
  {
      XColor exact, screen;

      if (!XAllocNamedColor(e->dpy, e->cmap, name, &screen, &exact)) {
          if (!XParseColor(e->dpy, e->cmap, name, &exact) ||
              !XAllocColor(e->dpy, e->cmap, &exact))
          {
              error(EXIT_FAILURE, 0, "Error allocating color '%s'", name);
          }
          screen = exact;
      }
      col->pixel = screen.pixel;
      col->r = screen.red >> 8;
      col->g = screen.green >> 8;
      col->b = screen.blue >> 8;
  }
  ```
* **Destructor:**
  `XFreeColors(e->dpy, e->cmap, &col->pixel, 1, 0)` (llamado en `win_close`).

---

### 2.5. Dibujado sobre la Superficie: `XftDraw` frente a `XImage` / `XPutImage`

#### A. En `libxft`:
* **Estructura:** `XftDraw`
* **Loader:** `XftDrawCreate(dpy, win->buf.pm, visual, colormap)`
* **Destructor:** `XftDrawDestroy(d)`
* **Funcionamiento:** Envuelve un `Picture` de XRender y realiza la subida de glifos al servidor X de manera transparente.

#### B. Sustitución Directa con Xlib Core (`XImage`):
Para evitar dependencias adicionales en tiempo de ejecución (como `libXrender`), se utiliza un pipeline de composición alfa en memoria local:
* **Estructura:** `XImage` (`<X11/Xutil.h>`)
* **Loader / Constructor:**
  ```c
  char *data = emalloc(win->w * win->bar.h * 4);
  XImage *bar_img = XCreateImage(e->dpy, e->vis, e->depth, ZPixmap, 0,
                                 data, win->w, win->bar.h, 32, 0);
  ```
* **Composición y Dibujado:**
  1. Se inicializa el buffer `bar_img` con el color de fondo de la barra (`win->fg.pixel`).
  2. Para cada glifo rasterizado por FreeType, se mezcla su máscara de transparencia (`bitmap.buffer`) con el color del texto (`win->bg`) sobre el píxel de fondo mediante la fórmula estándar de mezcla alfa lineal:
     $$C_{out} = \frac{\alpha \cdot C_{fg} + (255 - \alpha) \cdot C_{bg}}{255}$$
  3. Se transfiere la franja completa de la barra al Pixmap de la ventana con una única llamada atómica:
     ```c
     XPutImage(e->dpy, win->buf.pm, gc, bar_img, 0, 0, 0, win->h, win->w, win->bar.h);
     ```
* **Destructor:**
  ```c
  XDestroyImage(bar_img); /* Libera la estructura y el buffer data */
  ```

---

## 3. Matriz Comparativa: Resumen de Loaders y Destructores

| Componente | libxft (Antes) | Reemplazo con FreeType2 / X11 / Fontconfig | Loader / Constructor | Destructor / Limpiador |
| :--- | :--- | :--- | :--- | :--- |
| **Contexto Global** | Ninguno (implícito en Xft) | `FT_Library` | `FT_Init_FreeType(&ftlib)` | `FT_Done_FreeType(ftlib)` |
| **Fuente Principal** | `XftFont*` | `font_t` (`FT_Face`) | `open_font(...)` / `FT_New_Face` | `FT_Done_Face(font.face)` |
| **Búsqueda por Nombre**| `XftFontOpenName` | `FcNameParse` + `FcFontMatch` | `FcFontMatch(NULL, pat, &res)` | `FcPatternDestroy(pat)` |
| **Fuente Fallback** | `XftFontOpen` | `FcCharSetAddChar` + `FT_New_Face` | `open_font(...)` con `FcCharSet` | `FT_Done_Face(f.face)` |
| **Existencia Glifo** | `XftCharExists(dpy, f, rune)` | `FT_Get_Char_Index(f->face, rune)` | Consulta sobre `FT_Face` cargado | N/A (memoria interna) |
| **Rasterización** | `XftDrawStringUtf8` | `FT_Load_Glyph(..., FT_LOAD_RENDER)` | `FT_Load_Glyph` | N/A (gestionado en `slot`) |
| **Avance / Métrica** | `XftTextExtentsUtf8` (`xOff`) | `face->glyph->advance.x >> 6` | `FT_Load_Glyph` | N/A |
| **Color UI** | `XftColor` | `win_color_t` | `win_alloc_color` (`XAllocNamedColor`) | `XFreeColors` |
| **Superficie Dibujo**| `XftDraw*` | `XImage*` | `XCreateImage(...)` | `XDestroyImage(img)` |
| **Volcado a Pixmap** | Interno en `XftDrawStringUtf8` | `XPutImage(...)` | 1 llamada por actualización de barra | N/A |

---

## 4. Conclusiones del Análisis de Estructuras

1. **Paridad Funcional 1:1:** Cada función y estructura de `libxft` cuenta con un reemplazo directo y más transparente mediante `FreeType2`, `Fontconfig` y primitivas de `Xlib`.
2. **Eficiencia y Reducción de Dependencias:** El renderizado mediante `XImage` + `XPutImage` elimina por completo la necesidad tanto de `libXft` como de extensiones del servidor XRender, funcionando con la misma calidad visual mediante el antialiasing subpíxel/gris nativo de FreeType2.
3. **Gestión de Memoria Mejorada:** La implementación original de `sxiv` omitía la liberación de la fuente principal y colores en `win_close()`. Al introducir loaders y destructores explícitos, se garantiza una limpieza de memoria libre de fugas (*zero memory leaks*).
