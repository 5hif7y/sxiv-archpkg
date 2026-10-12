# Estudio del Uso de libxft en sxiv

**Fecha:** 4 de Septiembre de 2026  
**Objetivo:** Determinar exhaustivamente cómo se está utilizando `libxft` en el visor de imágenes `sxiv`, y confirmar si su uso está restringido exclusivamente al renderizado de cadenas de texto / etiquetas (status bar) o si interviene en otras áreas del sistema.

---

## 1. Resumen Ejecutivo y Respuesta al Requerimiento

> **Confirmación categórica:**  
> **`libxft` se utiliza ÚNICA Y EXCLUSIVAMENTE para la barra de estado (*status bar*) inferior.**
>
> 1. Se utiliza para **calcular la altura de la barra** a partir del *ascent* y *descent* de la fuente.
> 2. Se utiliza para **medir el ancho en píxeles** de las cadenas de texto formateadas (`TEXTWIDTH`).
> 3. Se utiliza para **rasterizar y dibujar los glifos UTF-8** de las partes izquierda y derecha de la barra de estado (`win_draw_bar` -> `win_draw_text`).
> 4. Se utiliza para **gestionar fuentes fallback** cuando un glifo/rune Unicode no existe en la fuente primaria (`XftCharExists` + `FcCharSet` + `XftFontOpen`).
> 5. Se utiliza de forma accesoria para **analizar y reservar los colores de la interfaz** (`win->bg` y `win->fg`) mediante `XftColorAllocName`, pero únicamente porque `win_draw_text` requería recibir un puntero `XftColor*`. El resto del programa solo lee el campo `pixel` (`unsigned long`) para llamadas estándar de Xlib.
>
> **En ninguna otra parte de `sxiv` se utiliza `libxft`**:
> - La visualización y escalado de imágenes utiliza **Imlib2**.
> - El modo cuadrícula / miniaturas (*thumbnails*) utiliza **Imlib2** y rectángulos X11 puros (`win_draw_rect` / `XChangeGC`).
> - Los títulos de ventana se configuran mediante propiedades de ventana X11 y átomos ICCCM/EWMH (`XChangeProperty`, `_NET_WM_NAME`).
> - Los cursores de ratón se gestionan mediante `XCreateFontCursor` (cursor estándar de X11).

---

## 2. Auditoría Completa del Código Fuente (Grep & Símbolos)

Se ejecutó un análisis estático de dependencias de símbolos en todo el árbol de código fuente de `sxiv`. A continuación se detalla la presencia exacta de `Xft` en cada archivo:

| Archivo | Línea(s) | Símbolo / Función `Xft` | Propósito |
| :--- | :--- | :--- | :--- |
| `sxiv.h` | 374 | `#include <X11/Xft/Xft.h>` | Inclusión del encabezado público de Xft. |
| `sxiv.h` | 410-411 | `XftColor bg; XftColor fg;` | Definición de colores de fondo y primer plano en `win_t`. |
| `window.c` | 49 | `static XftFont *font;` | Puntero a la fuente cargada para la barra de estado. |
| `window.c` | 58 | `XftFontOpenName(...)` | Apertura de la fuente principal configurada por el usuario o por defecto (`monospace-8`). |
| `window.c` | 65-72 | `win_alloc_color(...)` / `XftColorAllocName(...)` | Asignación de nombres de color (`"white"`, `"black"`, etc.) o hexadecimáles. |
| `window.c` | 350-382 | `win_draw_text(...)` | Función de trazado y medición de texto con fallback de fuentes. |
| `window.c` | 362 | `XftCharExists(...)` | Comprobación de si un carácter Unicode existe en la fuente primaria. |
| `window.c` | 367 | `XftFontOpen(...)` | Apertura de una fuente alternativa (*fallback*) para el rune faltante. |
| `window.c` | 372 | `XftTextExtentsUtf8(...)` | Medición del avance horizontal y métricas del glifo actual. |
| `window.c` | 375 | `XftDrawStringUtf8(...)` | Renderizado de los glifos UTF-8 sobre el *drawable* de X11. |
| `window.c` | 379 | `XftFontClose(...)` | Cierre de la fuente de fallback una vez dibujado el carácter. |
| `window.c` | 389 | `XftDraw *d;` | Contexto de renderizado para el buffer de la ventana (`win->buf.pm`). |
| `window.c` | 397 | `XftDrawCreate(...)` | Creación del contexto de renderizado Xft asociado al `Pixmap` de la ventana. |
| `window.c` | 418 | `XftDrawDestroy(d);` | Destrucción del contexto de renderizado Xft al finalizar el dibujo de la barra. |
| `Makefile` | 27 | `-lXft` | Enlace dinámico con `libXft.so`. |
| `sxiv.1` | 388 | `Name of Xft bar font` | Documentación en página de manual. |
| `README.md` | 42 | `* Xft` | Lista de dependencias en la documentación. |

---

## 3. Desglose del Flujo de Datos y Uso de `libxft`

### 3.1. Inicialización de la Fuente y Dimensiones de la Barra
En `window.c`:
```c
static XftFont *font;
static int fontheight;
static double fontsize;
static int barheight;

void win_init_font(const win_env_t *e, const char *fontstr)
{
	if ((font = XftFontOpenName(e->dpy, e->scr, fontstr)) == NULL)
		error(EXIT_FAILURE, 0, "Error loading font '%s'", fontstr);
	fontheight = font->ascent + font->descent;
	FcPatternGetDouble(font->pattern, FC_SIZE, 0, &fontsize);
	barheight = fontheight + 2 * V_TEXT_PAD;
}
```
- Se abre la fuente con `XftFontOpenName`. Si falla, `sxiv` finaliza con error.
- Se calculan las métricas verticales: `fontheight = font->ascent + font->descent`.
- Se extrae el tamaño en puntos (`FC_SIZE`) para poder solicitar fuentes de reemplazo con el mismo tamaño si se requieren caracteres Unicode especiales.
- La altura total de la barra (`barheight`) se deriva directamente de la altura de la fuente sumándole un padding vertical (`2 * V_TEXT_PAD`).

### 3.2. Gestión de Colores (`win->bg` y `win->fg`)
En `window.c`:
```c
void win_alloc_color(const win_env_t *e, const char *name, XftColor *col)
{
	if (!XftColorAllocName(e->dpy, DefaultVisual(e->dpy, e->scr),
	                       DefaultColormap(e->dpy, e->scr), name, col))
	{
		error(EXIT_FAILURE, 0, "Error allocating color '%s'", name);
	}
}
```
`XftColorAllocName` realiza dos tareas en una sola llamada:
1. Asigna el color en el `Colormap` de X11 y rellena `col->pixel` (un entero `unsigned long`).
2. Rellena la estructura `col->color` (componentes `red`, `green`, `blue`, `alpha` de 16 bits cada uno) para su uso con XRender.

**¿Dónde se utilizan `win->bg` y `win->fg` en el resto de sxiv?**
- `window.c` (líneas 261, 343, 403):
  `XSetForeground(e->dpy, gc, win->bg.pixel);`
  Se pasa únicamente el valor numérico `.pixel` al `GC` (Graphics Context) de Xlib para limpiar ventanas o rellenar el fondo de la barra.
- `thumbs.c` (líneas 472, 493):
  `unsigned long col = hl ? win->fg.pixel : win->bg.pixel;`
  `win_draw_rect(win, ..., col);`
  Se usa únicamente el `.pixel` para dibujar los marcos de selección de las miniaturas.
- `image.c` (línea 500):
  `c = win->bg.pixel;`
  `imlib_context_set_color(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF, 0xFF);`
  Se extraen los canales RGB directamente del entero de 32 bits para el color de fondo cuando no se usa transparencia en damero (*checkerboard*).
- `window.c` (líneas 411, 416):
  `win_draw_text(win, d, &win->bg, x, y, ...);`
  Aquí es el **único lugar** donde se utiliza la estructura `XftColor` como tal, ya que `XftDrawStringUtf8` exige recibir un puntero a `XftColor`.

### 3.3. Medición y Renderizado de Cadenas (`win_draw_text` y `win_draw_bar`)
En `window.c`:
```c
#define TEXTWIDTH(win, text, len) \
	win_draw_text(win, NULL, NULL, 0, 0, text, len, 0)

int win_draw_text(win_t *win, XftDraw *d, const XftColor *color, int x, int y,
                  char *text, int len, int w)
{
	int err, tw = 0;
	char *t, *next;
	uint32_t rune;
	XftFont *f;
	FcCharSet *fccharset;
	XGlyphInfo ext;

	for (t = text; t - text < len; t = next) {
		next = utf8_decode(t, &rune, &err);
		if (XftCharExists(win->env.dpy, font, rune)) {
			f = font;
		} else { /* fallback font */
			fccharset = FcCharSetCreate();
			FcCharSetAddChar(fccharset, rune);
			f = XftFontOpen(win->env.dpy, win->env.scr, FC_CHARSET, FcTypeCharSet,
			                fccharset, FC_SCALABLE, FcTypeBool, FcTrue,
			                FC_SIZE, FcTypeDouble, fontsize, NULL);
			FcCharSetDestroy(fccharset);
		}
		XftTextExtentsUtf8(win->env.dpy, f, (XftChar8*)t, next - t, &ext);
		tw += ext.xOff;
		if (tw <= w) {
			XftDrawStringUtf8(d, color, f, x, y, (XftChar8*)t, next - t);
			x += ext.xOff;
		}
		if (f != font)
			XftFontClose(win->env.dpy, f);
	}
	return tw;
}
```
En esta función:
1. Si `d == NULL`, solo mide el ancho acumulando `ext.xOff` de cada carácter y retorna `tw` (ancho total en píxeles).
2. Si `d != NULL`, además de medir, invoca `XftDrawStringUtf8` para pintar el glifo en el buffer `win->buf.pm` si no sobrepasa el ancho límite `w`.
3. Para cada carácter decodificado mediante UTF-8 (`utf8_decode`), consulta `XftCharExists`. Si el glifo no está en la fuente configurada, acude a Fontconfig para abrir una fuente de respaldo (*fallback*) que contenga dicho punto de código.

### 3.4. La Barra de Estado (`win_draw_bar`)
La barra de estado se dibuja en la parte inferior de la ventana cuando `win->bar.h > 0`:
- La parte izquierda (`win->bar.l.buf`) contiene: estado del archivo, índice, marcas, nombre de archivo.
- La parte derecha (`win->bar.r.buf`) contiene: porcentaje de zoom, dimensiones de imagen WxH, tamaño de archivo, etc.
- Ambas cadenas son dibujadas sobre el Pixmap `win->buf.pm` mediante `win_draw_text`.

---

## 4. Áreas de sxiv que NO utilizan `libxft`

Para garantizar que desacoplar `libxft` no romperá ninguna otra funcionalidad, se verificó cada subsistema:

1. **Visor de Imágenes (`image.c`):**
   - El renderizado de imágenes JPG, PNG, GIF, BMP, WebP, etc., es responsabilidad exclusiva de **Imlib2**.
   - Cero dependencias de Xft.
2. **Generación y Visualización de Miniaturas (`thumbs.c`):**
   - Las miniaturas se cargan con Imlib2 y se escalan con las rutinas de Imlib2.
   - El grid de miniaturas se dibuja en el pixmap de la ventana.
   - Las selecciones se trazan con llamadas estándar `XFillRectangle` / `XDrawRectangle` a través de `win_draw_rect`.
   - Cero dependencias de Xft.
3. **Gestión de Ventanas y Títulos (`window.c`):**
   - El título de la ventana se actualiza mediante `win_set_title()` usando átomos X11: `_NET_WM_NAME` y `_NET_WM_ICON_NAME` con tipo `UTF8_STRING`.
   - Cero dependencias de Xft.
4. **Cursores (`window.c`):**
   - Los cursores (`XC_left_ptr`, `XC_watch`, etc.) se inicializan con `XCreateFontCursor(e->dpy, ...)`. A pesar de tener la palabra "Font" en el nombre, esta es una llamada estándar de X11 Core que no tiene relación alguna con libxft.
5. **Control de Eventos y Comandos (`main.c`, `commands.c`):**
   - La entrada de teclado, rueda de ratón, señales inotify y lógica de comandos no interactúan con fuentes.

---

## 5. Conclusión del Estudio

La biblioteca `libxft` actúa estrictamente como un motor de trazado de texto para la barra de estado en `window.c`. Al reemplazar `libxft` por llamadas directas a `FreeType2` (apoyado en `Fontconfig` para la resolución de nombres y fallback), es posible desacoplar por completo `libxft` sin alterar ninguna característica funcional ni visual de `sxiv`.
