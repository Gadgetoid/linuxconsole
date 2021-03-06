/*
 *  linux/drivers/char/vt.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Hopefully this will be a rather complete VT102 implementation.
 *
 * Beeping thanks to John T Kohl.
 *
 * Virtual Consoles, Screen Blanking, Screen Dumping, Color, Graphics
 *   Chars, and VT100 enhancements by Peter MacDonald.
 *
 * Copy and paste function by Andrew Haylett,
 *   some enhancements by Alessandro Rubini.
 *
 * Code to check for different video-cards mostly by Galen Hunt,
 * <g-hunt@ee.utah.edu>
 *
 * Rudimentary ISO 10646/Unicode/UTF-8 character set support by
 * Markus Kuhn, <mskuhn@immd4.informatik.uni-erlangen.de>.
 *
 * Dynamic allocation of consoles, aeb@cwi.nl, May 1994
 * Resizing of consoles, aeb, 940926
 *
 * Code for xterm like mouse click reporting by Peter Orbaek 20-Jul-94
 * <poe@daimi.aau.dk>
 *
 * User-defined bell sound, new setterm control sequences and printk
 * redirection by Martin Mares <mj@k332.feld.cvut.cz> 19-Nov-95
 *
 * APM screenblank bug fixed Takashi Manabe <manabe@roy.dsl.tutics.tut.jp>
 *
 * Merge with the abstract console driver by Geert Uytterhoeven
 * <geert@linux-m68k.org>, Jan 1997.
 *
 *   Original m68k console driver modifications by
 *
 *     - Arno Griffioen <arno@usn.nl>
 *     - David Carter <carter@cs.bris.ac.uk>
 * 
 *   Note that the abstract console driver allows all consoles to be of
 *   potentially different sizes, so the following variables depend on the
 *   current console (currcons):
 *
 *     - video_num_columns
 *     - video_num_lines
 *     - video_size_row
 *     - can_do_color
 *
 *   The abstract console driver provides a generic interface for a text
 *   console. It supports VGA text mode, frame buffer based graphical consoles
 *   and special graphics processors that are only accessible through some
 *   registers (e.g. a TMS340x0 GSP).
 *
 *   The interface to the hardware is specified using a special structure
 *   (struct consw) which contains function pointers to console operations
 *   (see <linux/console.h> for more information).
 *
 * Support for changeable cursor shape
 * by Pavel Machek <pavel@atrey.karlin.mff.cuni.cz>, August 1997
 *
 * Ported to i386 and con_scrolldelta fixed
 * by Emmanuel Marty <core@ggi-project.org>, April 1998
 *
 * Resurrected character buffers in videoram plus lots of other trickery
 * by Martin Mares <mj@atrey.karlin.mff.cuni.cz>, July 1998
 *
 * Removed old-style timers, introduced console_timer, made timer
 * deletion SMP-safe.  17Jun00, Andrew Morton <andrewm@uow.edu.au>
 *
 * Removed console_lock, enabled interrupts across all console operations
 * 13 March 2001, Andrew Morton
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/tiocl.h>
#include <linux/consolemap.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/config.h>
#include <linux/workqueue.h>
#include <linux/bootmem.h>
#include <linux/pm.h>
#include <linux/font.h>

#include <asm/io.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/bitops.h>

/* A bitmap for codes <32. A bit of 1 indicates that the code
 * corresponding to that bit number invokes some special action
 * (such as cursor movement) and should not be displayed as a
 * glyph unless the disp_ctrl mode is explicitly enabled.
 */
#define CTRL_ACTION 0x0d00ff81
#define CTRL_ALWAYS 0x0800f501	/* Cannot be overridden by disp_ctrl */

extern void vcs_make_devfs(struct tty_struct *tty);
extern void vcs_remove_devfs(struct tty_struct *tty);
extern void console_map_init(void);

#ifdef CONFIG_VGA_CONSOLE
extern int vga_console_init(void);
#endif
#ifdef CONFIG_DUMMY_CONSOLE
extern int dumb_console_init(void);
#endif
#if defined (CONFIG_PROM_CONSOLE)
extern void prom_con_init(void);
#endif
#ifdef CONFIG_PROC_FS
extern int vt_proc_init(void);
#endif

struct tty_driver *console_driver;	/* TTY driver for all VT consoles */
static unsigned int current_vc;		/* Which /dev/vc/X to allocate next */
static unsigned int current_vt;		/* Which VT to allocate next */
struct vt_struct *admin_vt;		/* Administrative VT */
LIST_HEAD(vt_list);			/* Head to link list of VTs */

static void vt_flush_chars(struct tty_struct *tty);
enum {
	blank_off = 0,
	blank_normal_wait,
	blank_vesa_wait,
};

#ifdef CONFIG_VT_CONSOLE
static int kmsg_redirect = 0;	/* kmsg_redirect is the VC for printk */
static int printable;		/* Is console ready for printing? */
#endif

/* 
 * the default colour table, for colour systems 
 */
int default_red[] = { 0x00,0xaa,0x00,0xaa,0x00,0xaa,0x00,0xaa,
    0x55,0xff,0x55,0xff,0x55,0xff,0x55,0xff 
};
int default_grn[] = { 0x00,0x00,0xaa,0x55,0x00,0x00,0xaa,0xaa,
    0x55,0x55,0xff,0xff,0x55,0x55,0xff,0xff 
};
int default_blu[] = { 0x00,0x00,0x00,0x00,0xaa,0xaa,0xaa,0xaa,
    0x55,0x55,0x55,0x55,0xff,0xff,0xff,0xff
};

unsigned char color_table[] = { 0, 4, 2, 6, 1, 5, 3, 7,
	8,12,10,14, 9,13,11,15
};

/*
 * ignore_poke: don't unblank the screen when things are typed. This is
 * mainly for the privacy of braille terminal users.
 */
static int ignore_poke;
int do_poke_blanked_console;

/*
 * Hook so that the power management routines can (un)blank
 * the console on our behalf.
 */
int (*console_blank_hook)(int);

/*
 *	Low-Level Functions
 */
#ifdef VT_BUF_VRAM_ONLY
#define DO_UPDATE 0
#else
#define DO_UPDATE IS_VISIBLE
#endif

#define sw vc->display_fg->vt_sw

/*
 * Console cursor handling
 */
void add_softcursor(struct vc_data *vc)
{
	struct vt_struct *vt = vc->display_fg;
	int i = scr_readw((u16 *) vc->vc_pos);
	u32 type = vc->vc_cursor_type;

	if (! (type & 0x10)) return;
	if (vt->cursor_original != -1) return;
	vt->cursor_original = i;
	i |= ((type >> 8) & 0xff00 );
	i ^= ((type) & 0xff00 );
	if ((type & 0x20) && ((vt->cursor_original & 0x7000) == (i & 0x7000))) i ^= 0x7000;
	if ((type & 0x40) && ((i & 0x700) == ((i & 0x7000) >> 4))) i ^= 0x0700;
	scr_writew(i, (u16 *) vc->vc_pos);
	if (DO_UPDATE)
		sw->con_putc(vc, i, vc->vc_y, vc->vc_x);
}

static void hide_softcursor(struct vc_data *vc)
{
	struct vt_struct *vt = vc->display_fg;
	if (vt->cursor_original != -1) {
		scr_writew(vt->cursor_original,(u16 *) vc->vc_pos);
		if (DO_UPDATE)
			sw->con_putc(vc, vt->cursor_original, vc->vc_y, vc->vc_x);
		vt->cursor_original = -1;
	}
}

void hide_cursor(struct vc_data *vc)
{
	if (vc == sel_cons)
		clear_selection();
	sw->con_cursor(vc, CM_ERASE);
	hide_softcursor(vc);
}

void set_cursor(struct vc_data *vc)
{
    if (!IS_VISIBLE || vc->display_fg->vt_blanked || vc->vc_mode == KD_GRAPHICS)
	return;
    if (vc->vc_dectcem) {
	if (vc == sel_cons)
		clear_selection();
	add_softcursor(vc);
	if ((vc->vc_cursor_type & 0x0f) != 1)
		sw->con_cursor(vc, CM_DRAW);
    } else
	hide_cursor(vc);
}

/*
 * gotoxy() must verify all boundaries, because the arguments
 * might also be negative. If the given position is out of
 * bounds, the cursor is placed at the nearest margin.
 */
void gotoxy(struct vc_data *vc, int new_x, int new_y)
{
	int min_y, max_y;

	if (new_x < 0)
		vc->vc_x = 0;
	else
		if (new_x >= vc->vc_cols)
			vc->vc_x = vc->vc_cols - 1;
		else
			vc->vc_x = new_x;
 	if (vc->vc_decom) {
		min_y = vc->vc_top;
		max_y = vc->vc_bottom;
	} else {
		min_y = 0;
		max_y = vc->vc_rows;
	}
	if (new_y < min_y)
		vc->vc_y = min_y;
	else if (new_y >= max_y)
		vc->vc_y = max_y - 1;
	else
		vc->vc_y = new_y;
	vc->vc_pos = vc->vc_origin + vc->vc_y * vc->vc_size_row + (vc->vc_x << 1);
	vc->vc_need_wrap = 0;
}

/* for absolute user moves, when decom is set */
inline void gotoxay(struct vc_data *vc, int new_x, int new_y)
{
	gotoxy(vc, new_x, vc->vc_decom ? (vc->vc_top+new_y) : new_y);
}

/*
 *	Palettes
 */
void set_palette(struct vc_data *vc)
{
	WARN_CONSOLE_UNLOCKED();
	if (IS_VISIBLE && sw->con_set_palette && vc->vc_mode != KD_GRAPHICS)
		sw->con_set_palette(vc, color_table);
}

void reset_palette(struct vc_data *vc)
{
	int j, k;

	for (j=k=0; j<16; j++) {
		vc->vc_palette[k++] = default_red[j];
		vc->vc_palette[k++] = default_grn[j];
		vc->vc_palette[k++] = default_blu[j];
	}
	set_palette(vc);
}

/*
 * Functions to handle console scrolling.
 */
static inline void scrolldelta(struct vt_struct *vt, int lines)
{
	vt->scrollback_delta += lines;
	schedule_work(&vt->vt_work);
}

void scroll_up(struct vc_data *vc, int lines)
{
	if (!lines)
		lines = vc->vc_rows/2;
	scrolldelta(vc->display_fg, -lines);
}

void scroll_down(struct vc_data *vc, int lines)
{
	if (!lines)
		lines = vc->vc_rows/2;
	scrolldelta(vc->display_fg, lines);
}

void scroll_region_up(struct vc_data *vc, unsigned int t, unsigned int b, int nr)
{
	unsigned short *d, *s;

	if (t+nr >= b)
		nr = b - t - 1;
	if (b > vc->vc_rows || t >= b || nr < 1)
		return;
	if (IS_VISIBLE && sw->con_scroll_region(vc, t, b, SM_UP, nr))
		return;
	d = (unsigned short *) (vc->vc_origin + vc->vc_size_row*t);
	s = (unsigned short *) (vc->vc_origin + vc->vc_size_row*(t+nr));
	scr_memmovew(d, s, (b-t-nr) * vc->vc_size_row);
	scr_memsetw(d + (b-t-nr) * vc->vc_cols, vc->vc_video_erase_char, vc->vc_size_row*nr);
}

void scroll_region_down(struct vc_data *vc, unsigned int t, unsigned int b, int nr)
{
	unsigned short *s;
	unsigned int step;

	if (t+nr >= b)
		nr = b - t - 1;
	if (b > vc->vc_rows || t >= b || nr < 1)
		return;
	if (IS_VISIBLE && sw->con_scroll_region(vc, t, b, SM_DOWN, nr))
		return;
	s = (unsigned short *) (vc->vc_origin + vc->vc_size_row*t);
	step = vc->vc_cols * nr;
	scr_memmovew(s + step, s, (b-t-nr)*vc->vc_size_row);
	scr_memsetw(s, vc->vc_video_erase_char, 2*step);
}

/*
 * Console attribute handling. Structure of attributes is hardware-dependent
 */
void default_attr(struct vc_data *vc)
{
	vc->vc_intensity = 1;
	vc->vc_underline = 0;
	vc->vc_reverse = 0;
	vc->vc_blink = 0;
	if (vc->vc_can_do_color)
		vc->vc_color = vc->vc_def_color;
}

/*
 * ++roman: I completely changed the attribute format for monochrome
 * mode (!can_do_color). The formerly used MDA (monochrome display
 * adapter) format didn't allow the combination of certain effects.
 * Now the attribute is just a bit vector:
 *  Bit 0..1: intensity (0..2)
 *  Bit 2   : underline
 *  Bit 3   : reverse
 *  Bit 7   : blink
 */
static u8 build_attr(struct vc_data *vc, u8 _color, u8 _intensity, u8 _blink, u8 _underline, u8 _reverse)
{
	if (sw->con_build_attr)
		return sw->con_build_attr(vc, _color, _intensity, _blink, _underline, _reverse);
#ifndef VT_BUF_VRAM_ONLY
	{
	u8 a = vc->vc_color;
	if (!vc->vc_can_do_color)
		return _intensity |
		       (_underline ? 4 : 0) |
		       (_reverse ? 8 : 0) |
		       (_blink ? 0x80 : 0);
	if (_underline)
		a = (a & 0xf0) | vc->vc_ulcolor;
	else if (_intensity == 0)
		a = (a & 0xf0) | vc->vc_halfcolor;
	if (_reverse)
		a = ((a) & 0x88) | ((((a) >> 4) | ((a) << 4)) & 0x77);
	if (_blink)
		a ^= 0x80;
	if (_intensity == 2)
		a ^= 0x08;
	if (vc->vc_hi_font_mask == 0x100)
		a <<= 1;
	return a;
	}
#else
	return 0;
#endif
}

void update_attr(struct vc_data *vc)
{
	vc->vc_attr = build_attr(vc, vc->vc_color, vc->vc_intensity, vc->vc_blink,
				vc->vc_underline, vc->vc_reverse ^ vc->vc_decscnm);
	vc->vc_video_erase_char = (build_attr(vc, vc->vc_color, vc->vc_intensity, 0, 0, vc->vc_decscnm) << 8) | ' ';
}

static void clear_buffer_attributes(struct vc_data *vc)
{
	unsigned short *p = (unsigned short *) vc->vc_origin;
	int count = vc->vc_screenbuf_size/2;
	int mask = vc->vc_hi_font_mask | 0xff;

	for (; count > 0; count--, p++) {
		scr_writew((scr_readw(p)&mask) | (vc->vc_video_erase_char&~mask), p);
	}
}

/*
 *  Character management
 */
void insert_char(struct vc_data *vc, unsigned int nr)
{
	unsigned short *p, *q = (unsigned short *) vc->vc_pos;

	p = q + vc->vc_cols - nr - vc->vc_x;
	while (--p >= q)
		scr_writew(scr_readw(p), p + nr);
	scr_memsetw(q, vc->vc_video_erase_char, nr*2);
	vc->vc_need_wrap = 0;
	if (DO_UPDATE) {
		unsigned short oldattr = vc->vc_attr;
		sw->con_bmove(vc, vc->vc_y, vc->vc_x, vc->vc_y, vc->vc_x + nr,
				1, vc->vc_cols - vc->vc_x - nr);
		vc->vc_attr = vc->vc_video_erase_char >> 8;
		while (nr--)
			sw->con_putc(vc, vc->vc_video_erase_char, vc->vc_y,
					vc->vc_x + nr);
		vc->vc_attr = oldattr;
	}
}

void delete_char(struct vc_data *vc, unsigned int nr)
{
	unsigned short *p = (unsigned short *) vc->vc_pos;
	unsigned int i = vc->vc_x;

	while (++i <= vc->vc_cols - nr) {
		scr_writew(scr_readw(p+nr), p);
		p++;
	}
	scr_memsetw(p, vc->vc_video_erase_char, nr*2);
	vc->vc_need_wrap = 0;
	if (DO_UPDATE) {
		unsigned short oldattr = vc->vc_attr;
		sw->con_bmove(vc, vc->vc_y, vc->vc_x + nr, vc->vc_y, vc->vc_x,
				1, vc->vc_cols - vc->vc_x - nr);
		vc->vc_attr = vc->vc_video_erase_char >> 8;
		while (nr--)
			sw->con_putc(vc, vc->vc_video_erase_char, vc->vc_y,
				     vc->vc_cols - 1 - nr);
		vc->vc_attr = oldattr;
	}
}

void insert_line(struct vc_data *vc, unsigned int nr)
{
	scroll_region_down(vc, vc->vc_y, vc->vc_bottom, nr);
	vc->vc_need_wrap = 0;
}

void delete_line(struct vc_data *vc, unsigned int nr)
{
	scroll_region_up(vc, vc->vc_y, vc->vc_bottom, nr);
	vc->vc_need_wrap = 0;
}


/*
 * Functions that manage whats displayed on the screen
 */
void set_origin(struct vc_data *vc)
{
	WARN_CONSOLE_UNLOCKED();

	if (!IS_VISIBLE || !sw->con_set_origin || !sw->con_set_origin(vc))
		vc->vc_origin = (unsigned long) vc->vc_screenbuf;
	vc->vc_visible_origin = vc->vc_origin;
	vc->vc_scr_end = vc->vc_origin + vc->vc_screenbuf_size;
	vc->vc_pos = vc->vc_origin + vc->vc_size_row * vc->vc_y + 2 * vc->vc_x;
}

inline void clear_region(struct vc_data *vc, int sx, int sy, int width, int height)
{
	/* Clears the video memory, not the screen buffer */
	if (DO_UPDATE && sw->con_clear)
		return sw->con_clear(vc, sy, sx, height, width);
}

inline void save_screen(struct vc_data *vc)
{
	WARN_CONSOLE_UNLOCKED();

	if (sw->con_save_screen)
		sw->con_save_screen(vc);
}

void do_update_region(struct vc_data *vc, unsigned long start, int count)
{
#ifndef VT_BUF_VRAM_ONLY
	unsigned int xx, yy, offset;
	u16 *p;

	p = (u16 *) start;
	if (!sw->con_getxy) {
		offset = (start - vc->vc_origin) / 2;
		xx = offset % vc->vc_cols;
		yy = offset / vc->vc_cols;
	} else {
		int nxx, nyy;
		start = sw->con_getxy(vc, start, &nxx, &nyy);
		xx = nxx; yy = nyy;
	}
	for(;;) {
		u16 attrib = scr_readw(p) & 0xff00;
		int startx = xx;
		u16 *q = p;
		while (xx < vc->vc_cols && count) {
			if (attrib != (scr_readw(p) & 0xff00)) {
				if (p > q)
					sw->con_putcs(vc, q, p-q, yy, startx);
				startx = xx;
				q = p;
				attrib = scr_readw(p) & 0xff00;
			}
			p++;
			xx++;
			count--;
		}
		if (p > q)
			sw->con_putcs(vc, q, p-q, yy, startx);
		if (!count)
			break;
		xx = 0;
		yy++;
		if (sw->con_getxy) {
			p = (u16 *)start;
			start = sw->con_getxy(vc, start, NULL, NULL);
		}
	}
#endif
}

void update_region(struct vc_data *vc, unsigned long start, int count)
{
	WARN_CONSOLE_UNLOCKED();

	if (DO_UPDATE) {
		hide_cursor(vc);
		do_update_region(vc, start, count);
		set_cursor(vc);
	}
}

void update_screen(struct vc_data *vc)
{
	int update, old_was_color;

	if (!vc)
		return;
	old_was_color = vc->vc_can_do_color;
	hide_cursor(vc);
	set_origin(vc);
	update = sw->con_switch(vc);
	set_palette(vc);

	/*
	 * If console changed from mono<->color, the best we can do
	 * is to clear the buffer attributes. As it currently stands,
	 * rebuilding new attributes from the old buffer is not doable
	 * without overly complex code.
	 */
	if (old_was_color != vc->vc_can_do_color) {
		update_attr(vc);
		clear_buffer_attributes(vc);
	}
	if (update && vc->vc_mode != KD_GRAPHICS)
		do_update_region(vc, vc->vc_origin, vc->vc_screenbuf_size/2);
	set_cursor(vc);
}

inline unsigned short *screenpos(struct vc_data *vc, int offset, int viewed)
{
	unsigned short *p;

	if (!viewed)
		p = (unsigned short *)(vc->vc_origin + offset);
	else if (!sw->con_screen_pos)
		p = (unsigned short *)(vc->vc_visible_origin + offset);
	else
		p = sw->con_screen_pos(vc, offset);
	return p;
}

/* Note: inverting the screen twice should revert to the original state */
void invert_screen(struct vc_data *vc, int offset, int count, int viewed)
{
	unsigned short *p;

	WARN_CONSOLE_UNLOCKED();

	count /= 2;
	p = screenpos(vc, offset, viewed);
	if (sw->con_invert_region)
		sw->con_invert_region(vc, p, count);
#ifndef VT_BUF_VRAM_ONLY
	else {
		int cnt = count;
		u16 *q = p;
		u16 a;

		if (!vc->vc_can_do_color) {
			while (cnt--) {
			    a = scr_readw(q);
			    a ^= 0x0800;
			    scr_writew(a, q);
			    q++;
			}
		} else if (vc->vc_hi_font_mask == 0x100) {
			while (cnt--) {
				a = scr_readw(q);
				a = ((a) & 0x11ff) | (((a) & 0xe000) >> 4) | (((a) & 0x0e00) << 4);
				scr_writew(a, q);
				q++;
			}
		} else {
			while (cnt--) {
				a = scr_readw(q);
				a = ((a) & 0x88ff) | (((a) & 0x7000) >> 4) | (((a) & 0x0700) << 4);
				scr_writew(a, q);
				q++;
			}
		}
	}
#endif
	if (DO_UPDATE)
		do_update_region(vc, (unsigned long) p, count);
}

inline int resize_screen(struct vc_data *vc, int width, int height)
{
	/* Resizes the resolution of the display adapater */
	int err = 0;

	if (IS_VISIBLE && vc->vc_mode != KD_GRAPHICS && sw->con_resize)
		err = sw->con_resize(vc, width, height);
	return err;
}

/* used by selection: complement pointer position */
void complement_pos(struct vc_data *vc, int offset)
{
	static unsigned short oldx, oldy, old;
	static unsigned short *p;

	WARN_CONSOLE_UNLOCKED();

	if (p) {
		scr_writew(old, p);
		if (DO_UPDATE)
			sw->con_putc(vc, old, oldy, oldx);
	}
	if (offset == -1)
		p = NULL;
	else {
		unsigned short new;
		p = screenpos(vc, offset, 1);
		old = scr_readw(p);
		new = old ^ vc->vc_complement_mask;
		scr_writew(new, p);
		if (DO_UPDATE) {
			oldx = (offset >> 1) % vc->vc_cols;
			oldy = (offset >> 1) / vc->vc_cols;
			sw->con_putc(vc, new, oldy, oldx);
		}
	}
}

/*
 *	Screen blanking
 */

static void powerdown_screen(unsigned long private)
{
	/*
 	 *  Power down if currently suspended (1 or 2),
	 *  suspend if currently blanked (0),
	 *  else do nothing (i.e. already powered down (3)).
	 *  Called only if powerdown features are allowed.
	 */
	struct vt_struct *vt = (struct vt_struct *) private;
	struct vc_data *vc = vt->fg_console;

	switch (vc->display_fg->blank_mode) {
	case VESA_NO_BLANKING:
		sw->con_blank(vc, VESA_VSYNC_SUSPEND+1, 0);
		break;
	case VESA_VSYNC_SUSPEND:
	case VESA_HSYNC_SUSPEND:
		sw->con_blank(vc, VESA_POWERDOWN+1, 0);
		break;
	}
}

void do_blank_screen(struct vt_struct *vt, int entering_gfx)
{
	struct vc_data *vc = vt->fg_console;
	int i;

	WARN_CONSOLE_UNLOCKED();

	if (vt->vt_blanked) {
		if (vt->blank_state == blank_vesa_wait) {
			vt->blank_state = blank_off;
			powerdown_screen((unsigned long)vt);
		}
		return;
	}
	if (vt->blank_state != blank_normal_wait)
		return;
	vt->blank_state = blank_off;

	/* entering graphics mode? */
	if (entering_gfx) {
		hide_cursor(vc);
		save_screen(vc);
		sw->con_blank(vc, -1, 1);
		vt->vt_blanked = 1;
		set_origin(vc);
		return;
	}

	/* don't blank graphics */
	if (vc->vc_mode != KD_TEXT) {
		vt->vt_blanked = 1;
		return;
	}

	hide_cursor(vc);
	del_timer_sync(&vt->timer);
	vt->blank_timer_expired = 0;

	save_screen(vc);
	/* In case we need to reset origin, blanking hook returns 1 */
	i = sw->con_blank(vc, 1, 0);
	vt->vt_blanked = 1;
	if (i)
		set_origin(vc);

	if (console_blank_hook && console_blank_hook(1))
		return;
	if (vt->off_interval) {
		vt->blank_state = blank_vesa_wait;
		mod_timer(&vt->timer, jiffies + vt->off_interval);
	}
	if (vt->blank_mode)
		sw->con_blank(vc, vt->blank_mode + 1, 0);
}
EXPORT_SYMBOL(do_blank_screen);

/*
 * We defer the timer blanking to work queue so it can take the console semaphore
 * (console operations can still happen at irq time, but only from printk which
 * has the console semaphore. Not perfect yet, but better than no locking
 */
static void blank_screen_t(unsigned long dummy)
{
	struct vt_struct *vt = (struct vt_struct *) dummy;

	if (unlikely(!keventd_up())) {
		mod_timer(&vt->timer, jiffies + vt->off_interval);
		return;
	}
	vt->blank_timer_expired = 1;
	schedule_work(&vt->vt_work);
}

/*
 * Called by timer
 */
void unblank_vt(struct vt_struct *vt)
{
	struct vc_data *vc = vt->fg_console;

	ignore_poke = 0;
	if (!vt->vt_blanked)
		return;
	if (!vc) {
		/* impossible */
		printk("unblank_vt: visible tty not allocated ??\n");
		return;
	}
	if (vc->vc_mode != KD_TEXT)
		return; /* but leave vt->vt_blanked != 0 */

	if (vt->blank_interval) {
		mod_timer(&vt->timer, jiffies + vt->blank_interval);
		vt->blank_state = blank_normal_wait;
	}

	vt->vt_blanked = 0;
	if (sw->con_blank(vc, 0, 1))
		/* Low-level driver cannot restore -> do it ourselves */
		update_screen(vc);
	if (console_blank_hook)
		console_blank_hook(0);
	set_palette(vc);
	set_cursor(vc);
}
EXPORT_SYMBOL(unblank_vt);

/*
 * Called by vt_console_driver
 */
void unblank_screen(void)
{
        struct vt_struct *vt;

        list_for_each_entry (vt, &vt_list, node) {
                unblank_vt(vt);
        }
}

void poke_blanked_console(struct vt_struct *vt)
{
	struct vc_data *vc = vt->fg_console;

	WARN_CONSOLE_UNLOCKED();

	del_timer(&vt->timer);
	vt->blank_timer_expired = 0;
	if (ignore_poke || !vc || vc->vc_mode == KD_GRAPHICS)
		return;
	if (vt->vt_blanked) {
		unblank_vt(vt);
	} else if (vt->blank_interval) {
		mod_timer(&vt->timer, jiffies + vt->blank_interval);
		vt->blank_state = blank_normal_wait;
	}
}

/*
 * This is the console switching callback.
 *
 * Doing console switching in a process context allows
 * us to do the switches asynchronously (needed when we want
 * to switch due to a keyboard interrupt).  Synchronization
 * with other console code and prevention of re-entrancy is
 * ensured with the console semaphore.
 */
static void vt_callback(void *private)
{
	struct vt_struct *vt = (struct vt_struct *) private;

	if (!vt || !vt->want_vc || !vt->want_vc->vc_tty)
		return;

	acquire_console_sem();

	if ((vt->want_vc != vt->fg_console) && !vt->vt_dont_switch) {
		hide_cursor(vt->fg_console);
		change_console(vt->want_vc, vt->fg_console);
		/* we only changed when the console had already
		   been allocated - a new console is not created
		   in an interrupt routine */
	}
	if (do_poke_blanked_console) { /* do not unblank for a LED change */
		do_poke_blanked_console = 0;
		poke_blanked_console(vt);
	}
	if (vt->scrollback_delta) {
		struct vc_data *vc = vt->fg_console;
		clear_selection();
		if (vc->vc_mode == KD_TEXT)
			sw->con_scroll(vc, vt->scrollback_delta);
		vt->scrollback_delta = 0;
	}
	if (vt->blank_timer_expired) {
		do_blank_screen(vt, 0);
		vt->blank_timer_expired = 0;
	}
	release_console_sem();
}

inline void set_console(struct vc_data *vc)
{
	vc->display_fg->want_vc = vc;
	schedule_work(&vc->display_fg->vt_work);
}

/*
 *	Allocation, freeing and resizing of VTs.
 */
static void visual_init(struct vc_data *vc, int init)
{
	/*struct vc_data *default_mode = vc->display_fg->default_mode;*/
	
	/* ++Geert: sw->con_startup determines console size */
	vc->vc_uni_pagedir_loc = &vc->vc_uni_pagedir;
	vc->vc_uni_pagedir = 0;
	vc->vc_hi_font_mask = 0;
	vc->vc_complement_mask = 0;
	vc->vc_can_do_color = 0;
	sw->con_init(vc, init);
	if (!vc->vc_complement_mask)
		vc->vc_complement_mask = vc->vc_can_do_color ? 0x7700 : 0x0800;
	vc->vc_s_complement_mask = vc->vc_complement_mask;
	vc->vc_size_row = vc->vc_cols << 1;
	vc->vc_screenbuf_size = vc->vc_rows * vc->vc_size_row;
}

static void vc_init(struct vc_data *vc, int do_clear)
{
	set_origin(vc);
	vc->vc_pos = vc->vc_origin;
	reset_vc(vc);
	vc->vc_def_color = 0x07;	/* white */
	vc->vc_ulcolor = 0x0f;		/* bold white */
	vc->vc_halfcolor = 0x08;	/* grey */
	init_waitqueue_head(&vc->paste_wait);
	vte_ris(vc, do_clear);
}

struct vc_data *find_vc(int currcons)
{
	struct vt_struct *vt;

	list_for_each_entry(vt, &vt_list, node) {
		if ((currcons < vt->first_vc + vt->vc_count) &&
		    currcons >= vt->first_vc)
			return vt->vc_cons[currcons - vt->first_vc];
	}
	return NULL;
}

struct vc_data *vc_allocate(unsigned int currcons)
{
	struct vc_data *vc = NULL;
	struct vt_struct *vt;

	WARN_CONSOLE_UNLOCKED();

	if (currcons >= MAX_NR_CONSOLES) {
		currcons = -ENXIO;
		return NULL;
	}

	/* prevent users from taking too much memory */
	if (currcons >= MAX_NR_CONSOLES && !capable(CAP_SYS_RESOURCE)) {
		currcons = -EPERM;
		return NULL;
	}

	list_for_each_entry(vt, &vt_list, node) {
		if (currcons < vt->first_vc + vt->vc_count &&
		    currcons >= vt->first_vc)
			goto found_pool;
	}
	currcons = -ENXIO;
	return NULL;
found_pool:
	/* due to the granularity of kmalloc, we waste some memory here */
	/* the alloc is done in two steps, to optimize the common situation
	   of a 25x80 console (structsize=216, screenbuf_size=4000) */
	/* although the numbers above are not valid since long ago, the
	   point is still up-to-date and the comment still has its value
	   even if only as a historical artifact.  --mj, July 1998 */
	if (vt->kmalloced || !((vt->first_vc)== currcons))
		vc = (struct vc_data *) kmalloc(sizeof(struct vc_data), GFP_KERNEL);
	else
		vc = (struct vc_data *) alloc_bootmem(sizeof(struct vc_data));
	if (!vc) {
		currcons = -ENOMEM;
		return NULL;
	}
   	memset(vc, 0, sizeof(struct vc_data));
		
	vc->vc_num = currcons;
	vc->display_fg = vt;
	visual_init(vc, 1);
	if (vt->kmalloced || !((vt->first_vc) == currcons)) {
		vc->vc_screenbuf = (unsigned short *) kmalloc(vc->vc_screenbuf_size, GFP_KERNEL);
		if (!vc->vc_screenbuf) {
			kfree(vc);
			currcons = -ENOMEM;
			return NULL;
		}
		vt->kmalloced = 1;
		if (!*vc->vc_uni_pagedir_loc)
			con_set_default_unimap(vc);
	} else {
		vc->vc_screenbuf = (unsigned short *) alloc_bootmem(vc->vc_screenbuf_size);
		if (!vc->vc_screenbuf) {
			free_bootmem((unsigned long) vc, sizeof(struct vc_data));
			currcons = -ENOMEM;
			return NULL;
		}
	}
	vt->vc_cons[currcons - vt->first_vc] = vc;
	if ((vt->first_vc) == currcons)
		vt->want_vc = vt->fg_console = vt->last_console = vc;
	vc_init(vc, 1);
	return vc;
}

int vc_disallocate(struct vc_data *vc)
{
	struct vt_struct *vt = vc->display_fg;

	WARN_CONSOLE_UNLOCKED();

	if (vc && vc->vc_num > MIN_NR_CONSOLES) {
		sw->con_deinit(vc);
		vt->vc_cons[vc->vc_num - vt->first_vc] = NULL;
		if (vt->kmalloced)
			kfree(screenbuf);
		kfree(vc);
	}
	return 0;
}

void reset_vc(struct vc_data *vc)
{
	vc->vc_mode = KD_TEXT;
	vc->kbd_table.kbdmode = VC_XLATE;
	vc->vt_mode.mode = VT_AUTO;
	vc->vt_mode.waitv = 0;
	vc->vt_mode.relsig = 0;
	vc->vt_mode.acqsig = 0;
	vc->vt_mode.frsig = 0;
	vc->vt_pid = -1;
	vc->vt_newvt = -1;
	if (!in_interrupt())	/* Via keyboard.c:SAK() - akpm */
		reset_palette(vc) ;
}

/*
 * Change # of rows and columns (0 means unchanged/the size of fg_console)
 * [this is to be used together with some user program
 * like resize that changes the hardware videomode]
 */

#define VC_RESIZE_MAXCOL (32767)
#define VC_RESIZE_MAXROW (32767)

int vc_resize(struct vc_data *vc, unsigned int cols, unsigned int lines)
{
	unsigned int old_cols, old_rows, old_screenbuf_size, old_row_size;
	unsigned long ol, nl, nlend, rlth, rrem;
	unsigned int new_cols, new_rows, ss, new_row_size, err = 0;
	unsigned short *newscreen;

	WARN_CONSOLE_UNLOCKED();

	if (!vc)
		return 0;

	if (cols > VC_RESIZE_MAXCOL || lines > VC_RESIZE_MAXROW)
		return -EINVAL;

	new_cols = (cols ? cols : vc->vc_cols);
	new_rows = (lines ? lines : vc->vc_rows);
	new_row_size = new_cols << 1;
	ss = new_row_size * new_rows;

	if (new_cols == vc->vc_cols && new_rows == vc->vc_rows)
		return 0;

	newscreen = (unsigned short *) kmalloc(ss, GFP_USER);
	if (!newscreen) 
		return -ENOMEM;

	old_rows = vc->vc_rows;
	old_cols = vc->vc_cols;
	old_row_size = vc->vc_size_row;
	old_screenbuf_size = vc->vc_screenbuf_size;

	err = resize_screen(vc, new_cols, new_rows);
	if (err) {
		resize_screen(vc, old_cols, old_rows);
		kfree(newscreen);
		return err;
	}

	vc->vc_rows = new_rows;
	vc->vc_cols = new_cols;
	vc->vc_size_row = new_row_size;
	vc->vc_screenbuf_size = ss;

	rlth = min(old_row_size, new_row_size);
	rrem = new_row_size - rlth;
	ol = vc->vc_origin;
	nl = (long) newscreen;
	nlend = nl + ss;
	if (new_rows < old_rows)
		ol += (old_rows - new_rows) * old_row_size;

	update_attr(vc);

	while (ol < vc->vc_scr_end) {
		scr_memcpyw((unsigned short *) nl, (unsigned short *) ol, rlth);
		if (rrem)
			scr_memsetw((void *)(nl + rlth), vc->vc_video_erase_char, rrem);
		ol += old_row_size;
		nl += new_row_size;
	}
	if (nlend > nl)
		scr_memsetw((void *) nl, vc->vc_video_erase_char, nlend - nl);
	if (vc->display_fg->kmalloced)
		kfree(vc->vc_screenbuf);
	vc->vc_screenbuf = newscreen;
	vc->display_fg->kmalloced = 1;
	vc->vc_screenbuf_size = ss;
	set_origin(vc);

	/* do part of a reset_terminal() */
	vc->vc_top = 0;
	vc->vc_bottom = vc->vc_rows;
	gotoxy(vc, vc->vc_x, vc->vc_y);
	vte_decsc(vc);

	if (vc->vc_tty) {
		struct winsize ws, *cws = &vc->vc_tty->winsize;

		memset(&ws, 0, sizeof(ws));
		ws.ws_row = vc->vc_rows;
		ws.ws_col = vc->vc_cols;
		ws.ws_ypixel = vc->vc_scan_lines;
		if ((ws.ws_row != cws->ws_row || ws.ws_col != cws->ws_col) &&
		    vc->vc_tty->pgrp > 0)
			kill_pg(vc->vc_tty->pgrp, SIGWINCH, 1);
		*cws = ws;
	}

	if (IS_VISIBLE)
		update_screen(vc);
	return 0;
}

/*
 * Selection stuff for GPM.
 */
void mouse_report(struct vc_data *vc, int butt, int mrx, int mry)
{
	char buf[8];

	sprintf(buf, "\033[M%c%c%c", (char)(' ' + butt), (char)('!' + mrx),
		(char)('!' + mry));
	puts_queue(vc, buf);
}

/* invoked via ioctl(TIOCLINUX) and through set_selection */
int mouse_reporting(struct vc_data *vc)
{
	return vc->vc_report_mouse;
}

/* 
 * This is a temporary buffer used to prepare a tty console write
 * so that we can easily avoid touching user space while holding the
 * console spinlock.  It is allocated in con_init and is shared by
 * this code and the vc_screen read/write tty calls.
 *
 * We have to allocate this statically in the kernel data section
 * since console_init (and thus con_init) are called before any
 * kernel memory allocation is available.
 */

static int do_con_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
#ifdef VT_BUF_VRAM_ONLY
#define FLUSH do { } while(0);
#else
#define FLUSH if (draw_x >= 0 && sw->con_putcs) { \
	sw->con_putcs(vc, (u16 *)draw_from, (u16 *)draw_to-(u16 *)draw_from, vc->vc_y, draw_x); \
	draw_x = -1; \
	}
#endif
	unsigned long draw_from = 0, draw_to = 0;
	struct vc_data *vc = tty->driver_data;
	const unsigned char *orig_buf = NULL;
	int c, tc, ok, n = 0, draw_x = -1;
	u16 himask, charmask;
	int orig_count;

	if (in_interrupt())
		return count;

	might_sleep();

	acquire_console_sem();

	if (!vc) {
		printk("vt_write: tty %d not allocated\n", tty->index);
		release_console_sem();
		return 0;
	}
	release_console_sem();
	
	orig_buf = buf;
	orig_count = count;

	/* At this point 'buf' is guaranteed to be a kernel buffer
	 * and therefore no access to userspace (and therefore sleeping)
	 * will be needed.  The VT buffer lock serializes all tty based
	 * console rendering and vcs write/read operations.  We hold
	 * the console spinlock during the entire write.
	 */

	acquire_console_sem();

	himask = vc->vc_hi_font_mask;
	charmask = himask ? 0x1ff : 0xff;

	/* undraw cursor first */
	if (IS_VISIBLE)
		hide_cursor(vc);

	while (!tty->stopped && count) {
		c = *buf;
		buf++;
		n++;
		count--;

		/* Do no translation at all in control states */
		if (!vc->vc_state) {
			tc = c;
		} else if (vc->vc_utf) {
		    /* Combine UTF-8 into Unicode */
		    /* Incomplete characters silently ignored */
		    if(c > 0x7f) {
			if (vc->vc_utf_count > 0 && (c & 0xc0) == 0x80) {
				vc->vc_utf_char = (vc->vc_utf_char << 6) | (c & 0x3f);
				vc->vc_utf_count--;
				if (vc->vc_utf_count == 0)
				    tc = c = vc->vc_utf_char;
				else continue;
			} else {
				if ((c & 0xe0) == 0xc0) {
				    vc->vc_utf_count = 1;
				    vc->vc_utf_char = (c & 0x1f);
				} else if ((c & 0xf0) == 0xe0) {
				    vc->vc_utf_count = 2;
				    vc->vc_utf_char = (c & 0x0f);
				} else if ((c & 0xf8) == 0xf0) {
				    vc->vc_utf_count = 3;
				    vc->vc_utf_char = (c & 0x07);
				} else if ((c & 0xfc) == 0xf8) {
				    vc->vc_utf_count = 4;
				    vc->vc_utf_char = (c & 0x03);
				} else if ((c & 0xfe) == 0xfc) {
				    vc->vc_utf_count = 5;
				    vc->vc_utf_char = (c & 0x01);
				} else
				    vc->vc_utf_count = 0;
				continue;
			      }
		    } else {
		      tc = c;
		      vc->vc_utf_count = 0;
		    }
		} else {	/* no utf */
		  tc = vc->vc_translate[vc->vc_toggle_meta ? (c|0x80) : c];
		}

                /* If the original code was a control character we
                 * only allow a glyph to be displayed if the code is
                 * not normally used (such as for cursor movement) or
                 * if the disp_ctrl mode has been explicitly enabled.
                 * Certain characters (as given by the CTRL_ALWAYS
                 * bitmap) are always displayed as control characters,
                 * as the console would be pretty useless without
                 * them; to display an arbitrary font position use the
                 * direct-to-font zone in UTF-8 mode.
                 */
                ok = tc && (c >= 32 ||
                            (!vc->vc_utf && !(((vc->vc_disp_ctrl ? CTRL_ALWAYS
                                         : CTRL_ACTION) >> c) & 1)))
                        && (c != 127 || vc->vc_disp_ctrl)
			&& (c != 128+27);

		if (!vc->vc_state && ok) {
			/* Now try to find out how to display it */
			tc = conv_uni_to_pc(vc, tc);
			if ( tc == -4 ) {
                                /* If we got -4 (not found) then see if we have
                                   defined a replacement character (U+FFFD) */
                                tc = conv_uni_to_pc(vc, 0xfffd);

				/* One reason for the -4 can be that we just
				   did a clear_unimap();
				   try at least to show something. */
				if (tc == -4)
				     tc = c;
                        } else if ( tc == -3 ) {
                                /* Bad hash table -- hope for the best */
                                tc = c;
                        }
			if (tc & ~charmask)
                                continue; /* Conversion failed */

			if (vc->vc_need_wrap || vc->vc_irm)
				FLUSH
			if (vc->vc_need_wrap) {
				vte_cr(vc);
				vte_lf(vc);
			}
			if (vc->vc_irm)
				insert_char(vc, 1);
			scr_writew(himask ?
				     ((vc->vc_attr << 8) & ~himask) + ((tc & 0x100) ? himask : 0) + (tc & 0xff) : (vc->vc_attr << 8) + tc, (u16 *) vc->vc_pos);
			if (DO_UPDATE && draw_x < 0) {
				draw_x = vc->vc_x;
				draw_from = vc->vc_pos;
			}
			if (vc->vc_x == vc->vc_cols - 1) {
				vc->vc_need_wrap = vc->vc_decawm;
				draw_to = vc->vc_pos + 2;
			} else {
				vc->vc_x++;
				draw_to = (vc->vc_pos += 2);
			}
			continue;
		}
		FLUSH
		terminal_emulation(tty, c);
	}
	FLUSH
	console_conditional_schedule();
	release_console_sem();
	return n;
#undef FLUSH
}

/*
 *	/dev/ttyN handling
 */

/* Allocate the console screen memory. */
static int vt_open(struct tty_struct *tty, struct file *filp)
{
	struct vc_data *vc = tty->driver_data;
	int ret = 0;

	if (tty->count == 1) {
		acquire_console_sem();
		ret = tty->index;
		vc = vc_allocate(ret);
		if (vc) {
			tty->driver_data = vc;
			vc->vc_tty = tty;

			if (!tty->winsize.ws_row && !tty->winsize.ws_col) {
				tty->winsize.ws_row = vc->vc_rows;
				tty->winsize.ws_col = vc->vc_cols;
			}
			vcs_make_devfs(tty);
		}
		release_console_sem();
	}
	return ret;
}

/*
 * We take tty_sem in here to prevent another thread from coming in via init_dev
 * and taking a ref against the tty while we're in the process of forgetting
 * about it and cleaning things up.
 *
 * This is because vcs_remove_devfs() can sleep and will drop the BKL.
 */
static void vt_close(struct tty_struct *tty, struct file * filp)
{
	down(&tty_sem);
	acquire_console_sem();
	if (tty && tty->count == 1) {
		struct vc_data *vc = tty->driver_data;
		if (vc)
			vc->vc_tty = NULL;
		tty->driver_data = NULL;
		release_console_sem();
		vcs_remove_devfs(tty);
		up(&tty_sem);
		/*
		 * tty_sem is released, but we still hold BKL, so there is
		 * still exclusion against init_dev()
		 */
		return;
	}
	release_console_sem();
	up(&tty_sem);
}

static int vt_write(struct tty_struct * tty, const unsigned char *buf, int count)
{
	int	retval;

	retval = do_con_write(tty, buf, count);
	vt_flush_chars(tty);
	return retval;
}

static void vt_put_char(struct tty_struct *tty, unsigned char ch)
{
	if (in_interrupt())
		return;		/* n_r3964 calls put_char() from interrupt context */
	do_con_write(tty, &ch, 1);
}

static int vt_write_room(struct tty_struct *tty)
{
	if (tty->stopped)
		return 0;
	return 4096;		/* No limit, really; we're not buffering */
}

static void vt_flush_chars(struct tty_struct *tty)
{
	struct vc_data *vc;

	if (in_interrupt())	/* from flush_to_ldisc */
		return;

	/* if we race with vt_close(), vc may be null */
	acquire_console_sem();
	vc = tty->driver_data;
	if (vc)
		set_cursor(vc);
	release_console_sem();
}

static int vt_chars_in_buffer(struct tty_struct *tty)
{
	return 0;		/* we're not buffering */
}

/*
 * Turn the Scroll-Lock LED on when the tty is stopped
 */
static void vt_stop(struct tty_struct *tty)
{
	struct vc_data *vc;

	if (!tty)
		return;
	
	vc = tty->driver_data;
	if (!vc)
		return;
	set_kbd_led(&vc->kbd_table, VC_SCROLLOCK);
	set_leds();
}

/*
 * Turn the Scroll-Lock LED off when the console is started
 */
static void vt_start(struct tty_struct *tty)
{
	struct vc_data *vc;
	
	if (!tty)
		return;
	vc = tty->driver_data;
	
	if (!vc) 	
		return;
	clr_kbd_led(&vc->kbd_table, VC_SCROLLOCK);
	set_leds();
}

/*
 * con_throttle and con_unthrottle are only used for
 * paste_selection(), which has to stuff in a large number of
 * characters...
 */
static void vt_throttle(struct tty_struct *tty)
{
}

static void vt_unthrottle(struct tty_struct *tty)
{
	struct vc_data *vc = tty->driver_data;

	wake_up_interruptible(&vc->paste_wait);
}

#ifdef CONFIG_VT_CONSOLE

/*
 *	Console on virtual terminal
 *
 * The console must be locked when we get here.
 */

void vt_console_print(struct console *co, const char *b, unsigned count)
{
	struct vc_data *vc = find_vc(kmsg_redirect);
	static unsigned long printing;
	const ushort *start;
	ushort myx, cnt = 0;
	unsigned char c;

	/* console busy or not yet initialized */
	if (!printable || test_and_set_bit(0, &printing))
		return;

	if (!vc)
		vc = admin_vt->fg_console;

	/* read `x' only after setting currcons properly (otherwise
	   the `x' macro will read the x of the foreground console). */
	myx = vc->vc_x;

	if (vc->vc_mode != KD_TEXT)
		goto quit;

	/* undraw cursor first */
	if (IS_VISIBLE)
		hide_cursor(vc);

	start = (ushort *)vc->vc_pos;

	/* Contrived structure to try to emulate original need_wrap behaviour
	 * Problems caused when we have need_wrap set on '\n' character */
	while (count--) {
		c = *b++;
		if (c == 10 || c == 13 || c == 8 || vc->vc_need_wrap) {
			if (cnt > 0) {
				if (IS_VISIBLE)
					sw->con_putcs(vc, start, cnt, vc->vc_y, vc->vc_x);
				vc->vc_x += cnt;
				if (vc->vc_need_wrap)
					vc->vc_x--;
				cnt = 0;
			}
			if (c == 8) {		/* backspace */
				vte_bs(vc);
				start = (ushort *)vc->vc_pos;
				myx = vc->vc_x;
				continue;
			}
			if (c != 13)
				vte_lf(vc);
			vte_cr(vc);
			start = (ushort *)vc->vc_pos;
			myx = vc->vc_x;
			if (c == 10 || c == 13)
				continue;
		}
		scr_writew((vc->vc_attr << 8) + c, (unsigned short *) vc->vc_pos);
		cnt++;
		if (myx == vc->vc_cols - 1) {
			vc->vc_need_wrap = 1;
			continue;
		}
		vc->vc_pos += 2;
		myx++;
	}
	if (cnt > 0) {
		if (IS_VISIBLE)
			sw->con_putcs(vc, start, cnt, vc->vc_y, vc->vc_x);
		vc->vc_x += cnt;
		if (vc->vc_x == vc->vc_cols) {
			vc->vc_x--;
			vc->vc_need_wrap = 1;
		}
	}
	set_cursor(vc);

	if (!oops_in_progress)
		poke_blanked_console(vc->display_fg);
quit:
	clear_bit(0, &printing);
}

static struct tty_driver *vt_console_device(struct console *c, int *index)
{
	*index = c->index ? c->index - 1 : admin_vt->fg_console->vc_num;
	return console_driver;
}

struct console vt_console_driver = {
	.name		= "tty",
	.write		= vt_console_print,
	.device		= vt_console_device,
	.unblank	= unblank_screen,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
};
#endif

/*
 *	Handling of Linux-specific VC ioctls
 */

/*
 * Generally a bit racy with respect to console_sem().
 *
 * There are some functions which don't need it.
 *
 * There are some functions which can sleep for arbitrary periods
 * (paste_selection) but we don't need the lock there anyway.
 *
 * set_selection has locking, and definitely needs it
 */

int tioclinux(struct tty_struct *tty, unsigned long arg)
{
	struct vc_data *vc = tty->driver_data;
	char __user *p = (char __user *)arg;
	char type, data;
	int lines, ret;

	if (tty->driver->type != TTY_DRIVER_TYPE_CONSOLE)
		return -EINVAL;
	if (current->signal->tty != tty && !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (get_user(type, p))
		return -EFAULT;
	ret = 0;
	switch (type)
	{
		case TIOCL_SETSEL:
			acquire_console_sem();
			ret = set_selection((struct tiocl_selection __user *)(p+1), tty);
			release_console_sem();
			break;
		case TIOCL_PASTESEL:
			ret = paste_selection(tty);
			break;
		case TIOCL_UNBLANKSCREEN:
			unblank_screen();
			break;
		case TIOCL_SELLOADLUT:
			ret = sel_loadlut(p);
			break;
		case TIOCL_GETSHIFTSTATE:
			
	/*
	 * Make it possible to react to Shift+Mousebutton.
	 * Note that 'shift_state' is an undocumented
	 * kernel-internal variable; programs not closely
	 * related to the kernel should not use this.
	 */
	 		data = shift_state;
			ret = __put_user(data, p);
			break;
		case TIOCL_GETMOUSEREPORTING:
			data = mouse_reporting(vc);
			ret = __put_user(data, p);
			break;
		case TIOCL_SETVESABLANK:
			if (get_user(data, p + 1))
				return -EFAULT;
			vc->display_fg->blank_mode = (data < 4) ? data : 0;
			break;
		case TIOCL_SETKMSGREDIRECT: 
			if (!capable(CAP_SYS_ADMIN)) {
				ret = -EPERM;
			} else {
				if (get_user(data, p+1))
					ret = -EFAULT;
				else
					kmsg_redirect = data;
			}
			break;
		case TIOCL_GETFGCONSOLE:
			ret = vc->display_fg->fg_console->vc_num;
			break;
		case TIOCL_SCROLLCONSOLE:
			if (get_user(lines, (s32 __user *)(p+4))) {
				ret = -EFAULT;
			} else {
				scroll_down(vc, lines);
				ret = 0;
			}
			break;
		case TIOCL_BLANKSCREEN: /* until explicitly unblanked, not only poked */
			ignore_poke = 1;
			do_blank_screen(vc->display_fg, 0);
			break;
		case TIOCL_BLANKEDSCREEN:	
			ret = vc->display_fg->vt_blanked;
			break;
		default:
			ret = -EINVAL;
			break;
	}
	return ret;
}

/*
 * Mapping and unmapping displays to a VT
 */
const char *vt_map_display(struct vt_struct *vt, int init, int vc_count)
{
	const char *display_desc;

	if (current_vc + vc_count - 1 > MAX_NR_CONSOLES)
	        return NULL;

	vt->first_vc = current_vc;
	vt->vc_count = vc_count;
	display_desc = vt->vt_sw->con_startup(vt, init);
	if (!display_desc)
		return NULL;

	/* Now to setup VT */
	list_add_tail(&vt->node, &vt_list);
	init_MUTEX(&vt->lock);
	vt->vt_num = current_vt;
	vt->display_desc = (char *)display_desc;
	vt->vt_dont_switch = 0;
	vt->scrollback_delta = 0;	
	vt->vt_blanked = 0;
	vt->blank_interval = 10 * 60 * HZ;
	vt->off_interval = 0;
	vt->blank_state = blank_normal_wait;
	init_timer(&vt->timer);
	vt->timer.data = (long) vt;
	vt->timer.function = blank_screen_t;
	mod_timer(&vt->timer, jiffies + vt->blank_interval);
	vt->keyboard = NULL;
	INIT_WORK(&vt->vt_work, vt_callback, vt);

	if (!admin_vt) {
		admin_vt = vt;
#ifdef CONFIG_VT_CONSOLE
		register_console(&vt_console_driver);
		printable = 1;
#endif
	}
	acquire_console_sem();
	vt->vc_cons[0] = vc_allocate(current_vc);
	gotoxy(vt->fg_console, vt->fg_console->vc_x, vt->fg_console->vc_y);
	vte_ed(vt->fg_console, 0);
	update_screen(vt->fg_console);
	release_console_sem();
	current_vc += vc_count;
	current_vt += 1;
	if (vt->kmalloced) {
		vt_create_sysfs_dev_files(vt);
#ifdef CONFIG_PROC_FS
		vt_proc_attach(vt);
#endif
	}
	return display_desc;
}

void vt_map_input(struct vt_struct *vt)
{
	if (vt->beeper) {
		init_timer(&vt->beep);
		vt->beep.data = (long) vt->beeper;
		vt->beep.function = kd_nosound;
	}
}	

/*
 * This routine initializes console interrupts, and does nothing
 * else. If you want the screen to clear, call tty_write with
 * the appropriate escape-sequence.
 */
static int __init vt_console_init(void)
{
	int err = 0;
	
#ifdef CONFIG_VGA_CONSOLE
	err = vga_console_init();	
#elif defined (CONFIG_DUMMY_CONSOLE)
        err = dumbcon_init();
#endif
	return err;
}

console_initcall(vt_console_init);

static struct tty_operations vt_ops = {
	.open			= vt_open,
	.close			= vt_close,
	.write			= vt_write,
	.write_room		= vt_write_room,
	.put_char		= vt_put_char,
	.flush_chars		= vt_flush_chars,
	.chars_in_buffer 	= vt_chars_in_buffer,
	.ioctl			= vt_ioctl,
	.stop			= vt_stop,
	.start			= vt_start,
	.throttle		= vt_throttle,
	.unthrottle		= vt_unthrottle,
};	

int __init vty_init(void)
{
	if (list_empty(&vt_list))
		return -ENXIO;
	
	vcs_init();

	console_driver = alloc_tty_driver(MAX_NR_CONSOLES);
	if (!console_driver)
		panic("Couldn't allocate VT console driver\n");
	console_driver->owner = THIS_MODULE;
	console_driver->devfs_name = "vc/";
	console_driver->name = "tty";
	console_driver->name_base = 1;
	console_driver->major = TTY_MAJOR;
	console_driver->minor_start = 1;
	console_driver->type = TTY_DRIVER_TYPE_CONSOLE;
	console_driver->init_termios = tty_std_termios;
	console_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_RESET_TERMIOS;
	tty_set_operations(console_driver, &vt_ops);
	if (tty_register_driver(console_driver))
		panic("Couldn't register console driver\n");

	vt_sysfs_init();
#ifdef CONFIG_PROC_FS
        vt_proc_init();
#endif
#if defined (CONFIG_PROM_CONSOLE)
	prom_con_init();
#endif
#if defined (CONFIG_DUMMY_CONSOLE)
        dumb_console_init();
#endif
	kbd_init();
	console_map_init();
	return 0;
}

/*
 *	If we support more console drivers, this function is used
 *	when a driver wants to take over some existing consoles
 *	and become default driver for newly opened ones.
 */

int take_over_console(struct vt_struct *vt, const struct consw *csw)
{
	struct vc_data *vc = vt->fg_console;
	struct module *owner;
	const char *desc;
	int i;

	owner = csw->owner;
	if (!try_module_get(owner))
		return -ENODEV;

	/* First shutdown old console driver */
	acquire_console_sem();
	hide_cursor(vc);

	for (i = 0; i < vt->vc_count; i++) {
		vc = vt->vc_cons[i];
		if (vc)
			sw->con_deinit(vc);
	}

	/* under Linux-ruby one reference per console driver */
	module_put(vt->vt_sw->owner);
	__module_get(owner);

	/* Test new hardware state */
	desc = csw->con_startup(vt, 0);
	if (!desc) {
		/* Make sure the original driver state is restored to normal */
		vt->vt_sw->con_startup(vt, 1);
		release_console_sem();
		module_put(owner);
		return -ENODEV;
	}
	vt->display_desc = (char *)desc;
	vt->vt_sw = csw;

	/* Set the VC states to the new default mode */
	for (i = 0; i < vt->vc_count; i++) {
		int old_was_color;
		vc = vt->vc_cons[i];

		if (vc) {
			old_was_color = vc->vc_can_do_color;
			vc->vc_num = vt->first_vc + i;
			vc->vc_origin = (unsigned long) vc->vc_screenbuf;
			vc->vc_visible_origin = vc->vc_origin;
			vc->vc_scr_end = vc->vc_origin + vc->vc_screenbuf_size;
			vc->vc_pos = vc->vc_origin + vc->vc_size_rows * vc->vc_y + 2 * vc->vc_x;
			visual_init(vc, 0);
			update_attr(vc);

			/* If the console changed between mono <-> color, then
			 * the attributes in the screenbuf will be wrong.  The
			 * following resets all attributes to something sane.
			 */
			if (old_was_color != vc->vc_can_do_color)
				clear_buffer_attributes(vc);
		}
	}
	vc = vt->fg_console;
	update_screen(vc);

	printk("Console: switching to %s %s %dx%d vc:%d-%d\n",
			vc->vc_can_do_color ? "colour" : "mono",
			desc, vc->vc_cols, vc->vc_rows,
			vt->first_vc + 1, vt->first_vc + vt->vc_count);
	release_console_sem();
	module_put(owner);
	return 0;
}

/*
 *	Visible symbols for modules
 */

EXPORT_SYMBOL(color_table);
EXPORT_SYMBOL(default_red);
EXPORT_SYMBOL(default_grn);
EXPORT_SYMBOL(default_blu);
EXPORT_SYMBOL(vc_resize);
EXPORT_SYMBOL(console_blank_hook);
EXPORT_SYMBOL(vt_list);
EXPORT_SYMBOL(take_over_console);
EXPORT_SYMBOL(update_region);
EXPORT_SYMBOL(update_screen);
EXPORT_SYMBOL(vt_map_display);
EXPORT_SYMBOL(admin_vt);
EXPORT_SYMBOL(find_vc);
