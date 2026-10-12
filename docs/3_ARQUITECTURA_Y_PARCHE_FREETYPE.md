# Arquitectura de la Desacoplación de libxft y Parche FreeType2

**Fecha:** 4 de Septiembre de 2026  
**Objetivo:** Detallar el diseño técnico, la implementación en código C, el parche resultante y la integración en el empaquetado de Arch Linux para reemplazar `libxft` por `FreeType2` directamente en `sxiv`, preservando el 100% de la funcionalidad.

---

## 1. Visión General de la Solución

Para eliminar la dependencia de `libxft` sin perder funcionalidad:
1. **Desacoplamos los tipos de datos en `sxiv.h`**:
   Sustituimos `XftColor` por una estructura nativa liviana `win_color_t` que mantiene el campo `unsigned long pixel` (utilizado por el resto de sxiv para operaciones X11) y añade componentes de 8 bits `r, g, b` para operaciones de color local.
2. **Carga y dimensionado de fuentes mediante FreeType2 y Fontconfig en `window.c`**:
   - Se utiliza `FcNameParse` y `FcFontMatch` para resolver nombres estándar (ej. `"monospace-8"` o `"DejaVu Sans Mono:size=10"`).
   - Se extraen la ruta del archivo (`FC_FILE`), el índice dentro del archivo (`FC_INDEX`, esencial para colecciones `.ttc` como *Noto Sans CJK*) y el tamaño en píxeles (`FC_PIXEL_SIZE` / `FC_SIZE`).
   - Se inicializa el contexto de FreeType (`FT_Init_FreeType`) y se crea la cara tipográfica (`FT_New_Face` + `FT_Set_Pixel_Sizes`).
   - Se extraen las métricas de línea base: `ascent`, `descent` y `height`.
3. **Mecanismo de Fallback de Fuentes Unicode**:
   - Ante cada carácter, se comprueba su existencia en la fuente principal con `FT_Get_Char_Index(face, rune)`.
   - Si no existe (índice 0), se genera dinámicamente un `FcCharSet` con dicho punto de código y se busca una fuente alternativa mediante `FcFontMatch`.
   - Se carga el glifo con la fuente de respaldo y se libera dicha cara tipográfica al completar el carácter.
4. **Pipeline de Renderizado Antialiased Directo en Memoria**:
   - En lugar de requerir extensiones de servidor complejas como XRender, `win_draw_bar()` genera un buffer local de imagen `XImage` para la barra de estado.
   - El buffer se pre-rellena con el color de fondo de la barra (`win->fg.pixel`).
   - Cada glifo rasterizado por FreeType (`FT_LOAD_RENDER`, modo `FT_PIXEL_MODE_GRAY`) genera un mapa de bits con valores alfa de 0 a 255.
   - Se aplica una mezcla alfa lineal (*software blending*) de alta precisión entre el color de fondo y el color de texto (`win->bg`), respetando la máscara visual de X11.
   - La barra completa se vuelca al pixmap de la ventana (`win->buf.pm`) con una única llamada atómica `XPutImage`.
5. **Cero Fugas de Memoria (*Zero Memory Leaks*)**:
   - `win_close()` incorpora las llamadas a `FT_Done_Face()`, `FT_Done_FreeType()` y `FcFini()`.
6. **Ajuste de Makefile y PKGBUILD**:
   - En `Makefile`, se reemplaza `-lXft` por `-lfreetype`.
   - En `PKGBUILD`, se retira `libxft` de las dependencias (`depends`) y se agregan `freetype2` y `fontconfig`.

---

## 2. Cambios Implementados en el Código Fuente

### 2.1. `sxiv.h`
Se elimina la inclusión de `<X11/Xft/Xft.h>` y se define `win_color_t`:
```diff
--- a/sxiv.h
+++ b/sxiv.h
@@ -371,7 +371,12 @@ int r_mkdir(char*);
 /* window.c */
 
 #include <X11/Xutil.h>
-#include <X11/Xft/Xft.h>
+#include <stdint.h>
+
+typedef struct {
+	unsigned long pixel;
+	uint8_t r, g, b;
+} win_color_t;
 
 enum {
 	BAR_L_LEN = 512,
@@ -406,8 +411,8 @@ struct win {
 	Window xwin;
 	win_env_t env;
 
-	XftColor bg;
-	XftColor fg;
+	win_color_t bg;
+	win_color_t fg;
 
 	int x;
 	int y;
```

### 2.2. `window.c`
#### Inicialización de FreeType2 y Resolución Fontconfig:
```c
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

typedef struct {
	FT_Face face;
	int ascent;
	int descent;
	int height;
} font_t;

static FT_Library ftlib;
static font_t font;
static int fontheight;
static double fontsize;
static int barheight;

static bool open_font_pattern(FcPattern *pat, font_t *f)
{
	FcPattern *match;
	FcResult res;
	FcChar8 *file = NULL;
	int index = 0;
	double size = 0.0;

	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	match = FcFontMatch(NULL, pat, &res);
	if (!match)
		return false;

	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || file == NULL) {
		FcPatternDestroy(match);
		return false;
	}
	FcPatternGetInteger(match, FC_INDEX, 0, &index);
	if (FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &size) != FcResultMatch) {
		if (FcPatternGetDouble(match, FC_SIZE, 0, &size) == FcResultMatch)
			size = size * 96.0 / 72.0;
		else
			size = 12.0;
	}

	if (FT_New_Face(ftlib, (const char *)file, index, &f->face) != 0) {
		FcPatternDestroy(match);
		return false;
	}

	if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)size) != 0) {
		if (f->face->num_fixed_sizes > 0)
			FT_Select_Size(f->face, 0);
	}

	f->ascent = f->face->size->metrics.ascender >> 6;
	f->descent = -(f->face->size->metrics.descender >> 6);
	f->height = f->face->size->metrics.height >> 6;
	if (f->height < f->ascent + f->descent)
		f->height = f->ascent + f->descent;

	FcPatternDestroy(match);
	return true;
}
```

#### Asignación de Color Estándar X11:
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

#### Algoritmo de Mezcla Alfa (Alpha Blending):
```c
static inline unsigned long blend_pixel(const win_env_t *e, unsigned long bg_pix,
                                       const win_color_t *fg_col, uint8_t a)
{
	if (a == 0)
		return bg_pix;
	if (a == 255)
		return fg_col->pixel;

	if (e->vis->red_mask == 0xff0000 && e->vis->green_mask == 0xff00 && e->vis->blue_mask == 0xff) {
		uint32_t bg_r = (bg_pix >> 16) & 0xFF;
		uint32_t bg_g = (bg_pix >> 8) & 0xFF;
		uint32_t bg_b = bg_pix & 0xFF;

		uint32_t r = (fg_col->r * a + bg_r * (255 - a)) / 255;
		uint32_t g = (fg_col->g * a + bg_g * (255 - a)) / 255;
		uint32_t b = (fg_col->b * a + bg_b * (255 - a)) / 255;

		return (r << 16) | (g << 8) | b;
	} else {
		unsigned long rm = e->vis->red_mask;
		unsigned long gm = e->vis->green_mask;
		unsigned long bm = e->vis->blue_mask;

		int rs = rm ? __builtin_ctzl(rm) : 0;
		int gs = gm ? __builtin_ctzl(gm) : 0;
		int bs = bm ? __builtin_ctzl(bm) : 0;

		uint32_t bg_r = (bg_pix & rm) >> rs;
		uint32_t bg_g = (bg_pix & gm) >> gs;
		uint32_t bg_b = (bg_pix & bm) >> bs;

		uint32_t r = (fg_col->r * a + bg_r * (255 - a)) / 255;
		uint32_t g = (fg_col->g * a + bg_g * (255 - a)) / 255;
		uint32_t b = (fg_col->b * a + bg_b * (255 - a)) / 255;

		return ((r << rs) & rm) | ((g << gs) & gm) | ((b << bs) & bm);
	}
}
```

#### Medición y Trazado de Texto con Fallback:
```c
int win_draw_text(win_t *win, XImage *bar_img, const win_color_t *color, int x, int y,
                  char *text, int len, int w)
{
	int err, tw = 0;
	char *t, *next;
	uint32_t rune;
	font_t *f, fallback_font;
	bool is_fallback;

	for (t = text; t - text < len; t = next) {
		next = utf8_decode(t, &rune, &err);
		is_fallback = false;
		f = &font;

		FT_UInt glyph_index = FT_Get_Char_Index(font.face, rune);
		if (glyph_index == 0) {
			FcPattern *pat = FcPatternCreate();
			FcCharSet *fcs = FcCharSetCreate();
			FcCharSetAddChar(fcs, rune);
			FcPatternAddCharSet(pat, FC_CHARSET, fcs);
			FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
			FcPatternAddDouble(pat, FC_SIZE, fontsize);
			if (open_font_pattern(pat, &fallback_font)) {
				f = &fallback_font;
				is_fallback = true;
				glyph_index = FT_Get_Char_Index(f->face, rune);
			}
			FcCharSetDestroy(fcs);
			FcPatternDestroy(pat);
		}

		if (FT_Load_Glyph(f->face, glyph_index, bar_img != NULL ? FT_LOAD_RENDER : FT_LOAD_DEFAULT) != 0) {
			if (is_fallback)
				FT_Done_Face(fallback_font.face);
			continue;
		}

		int advance = (f->face->glyph->advance.x + 32) >> 6;
		tw += advance;

		if (bar_img != NULL && tw <= w) {
			FT_GlyphSlot slot = f->face->glyph;
			FT_Bitmap *bm = &slot->bitmap;
			int gx = x + slot->bitmap_left;
			int gy = y - slot->bitmap_top;

			for (int row = 0; row < (int)bm->rows; row++) {
				int py = gy + row;
				if (py < 0 || py >= (int)win->bar.h)
					continue;
				for (int col = 0; col < (int)bm->width; col++) {
					int px = gx + col;
					if (px < 0 || px >= (int)win->w)
						continue;

					uint8_t alpha = bm->buffer[row * bm->pitch + col];
					if (alpha == 0)
						continue;

					unsigned long bg_pix = XGetPixel(bar_img, px, py);
					unsigned long blended = blend_pixel(&win->env, bg_pix, color, alpha);
					XPutPixel(bar_img, px, py, blended);
				}
			}
			x += advance;
		}

		if (is_fallback)
			FT_Done_Face(fallback_font.face);
	}
	return tw;
}
```

#### Generación de la Barra de Estado y Destructores:
```c
void win_draw_bar(win_t *win)
{
	int len, x, y, w, tw;
	win_env_t *e;
	win_bar_t *l, *r;
	XImage *bar_img;
	char *img_data;

	if ((l = &win->bar.l)->buf == NULL || (r = &win->bar.r)->buf == NULL)
		return;

	e = &win->env;
	y = font.ascent + V_TEXT_PAD;
	w = win->w - 2 * H_TEXT_PAD;

	if ((len = strlen(r->buf)) > 0) {
		tw = TEXTWIDTH(win, r->buf, len);
		if (tw > w)
			return;
	} else {
		tw = 0;
	}

	img_data = emalloc(win->w * win->bar.h * 4);
	bar_img = XCreateImage(e->dpy, e->vis, e->depth, ZPixmap, 0,
	                       img_data, win->w, win->bar.h, 32, 0);

	for (unsigned int py = 0; py < win->bar.h; py++) {
		for (unsigned int px = 0; px < win->w; px++) {
			XPutPixel(bar_img, px, py, win->fg.pixel);
		}
	}

	if (len > 0) {
		x = win->w - tw - H_TEXT_PAD;
		w -= tw;
		win_draw_text(win, bar_img, &win->bg, x, y, r->buf, len, tw);
	}
	if ((len = strlen(l->buf)) > 0) {
		x = H_TEXT_PAD;
		w -= 2 * H_TEXT_PAD;
		win_draw_text(win, bar_img, &win->bg, x, y, l->buf, len, w);
	}

	XPutImage(e->dpy, win->buf.pm, gc, bar_img, 0, 0, 0, win->h, win->w, win->bar.h);
	XDestroyImage(bar_img);
}

CLEANUP void win_close(win_t *win)
{
	int i;

	for (i = 0; i < ARRLEN(cursors); i++)
		XFreeCursor(win->env.dpy, cursors[i].icon);

	XFreeGC(win->env.dpy, gc);

	XDestroyWindow(win->env.dpy, win->xwin);
	XCloseDisplay(win->env.dpy);

	if (font.face)
		FT_Done_Face(font.face);
	if (ftlib)
		FT_Done_FreeType(ftlib);
	FcFini();
}
```

### 2.3. `Makefile`
```diff
--- a/Makefile
+++ b/Makefile
@@ -24,7 +24,7 @@ lib_exif_0 =
 lib_exif_1 = -lexif
 lib_gif_0 =
 lib_gif_1 = -lgif
-ldlibs = $(LDLIBS) -lImlib2 -lX11 -lXft -lfontconfig \
+ldlibs = $(LDLIBS) -lImlib2 -lX11 -lfreetype -lfontconfig \
   $(lib_exif_$(HAVE_LIBEXIF)) $(lib_gif_$(HAVE_GIFLIB))
 
 objs = autoreload_$(AUTORELOAD).o commands.o image.o main.o options.o \
```

---

## 3. Archivo de Parche Generado

El archivo de parche unificado `sxiv-freetype.patch` ha sido generado y depositado en:
- `/home/shifty/ANTIGRAVITY-DEV/sxiv-sin-xftlib/sxiv-freetype.patch`
- `/home/shifty/ANTIGRAVITY-DEV/sxiv-sin-xftlib/sxiv/sxiv-freetype.patch`

Puede ser aplicado en cualquier copia limpia de `sxiv v26` mediante:
```bash
patch -Np1 -i sxiv-freetype.patch
```

---

## 4. Verificación de Compilación y Enlace

Al ejecutar la compilación:
```bash
make clean && make
```
El ejecutable se construye exitosamente. La verificación con `ldd` demuestra que:
1. `libXft.so` ya **no está presente** entre las bibliotecas requeridas.
2. `libfreetype.so.6` está vinculada directamente.
3. No se producen advertencias del compilador ni errores de símbolos no definidos.
