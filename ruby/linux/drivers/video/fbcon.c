/*
 *  linux/drivers/video/fbcon.c -- Low level frame buffer based console driver
 *
 *	Copyright (C) 1995 Geert Uytterhoeven
 *
 *
 *  This file is based on the original Amiga console driver (amicon.c):
 *
 *	Copyright (C) 1993 Hamish Macdonald
 *			   Greg Harp
 *	Copyright (C) 1994 David Carter [carter@compsci.bristol.ac.uk]
 *
 *	      with work by William Rucklidge (wjr@cs.cornell.edu)
 *			   Geert Uytterhoeven
 *			   Jes Sorensen (jds@kom.auc.dk)
 *			   Martin Apel
 *
 *  and on the original Atari console driver (atacon.c):
 *
 *	Copyright (C) 1993 Bjoern Brauel
 *			   Roman Hodek
 *
 *	      with work by Guenther Kelleter
 *			   Martin Schaller
 *			   Andreas Schwab
 *
 *  Hardware cursor support added by Emmanuel Marty (core@ggi-project.org)
 *  Smart redraw scrolling, arbitrary font width support, 512char font support
 *  and software scrollback added by 
 *                         Jakub Jelinek (jj@ultra.linux.cz)
 *
 *  Random hacking by Martin Mares <mj@ucw.cz>
 *
 *
 *  The low level operations for the various display memory organizations are
 *  now in separate source files.
 *
 *  Currently the following organizations are supported:
 *
 *    o afb			Amiga bitplanes
 *    o cfb{2,4,8,16,24,32}	Packed pixels
 *    o ilbm			Amiga interleaved bitplanes
 *    o iplan2p[248]		Atari interleaved bitplanes
 *    o mfb			Monochrome
 *    o vga			VGA characters/attributes
 *
 *  To do:
 *
 *    - Implement 16 plane mode (iplan2p16)
 *
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#undef FBCONDEBUG

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/delay.h>	/* MSch: for IRQ probe */
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/string.h>
#include <linux/kd.h>
#include <linux/malloc.h>
#include <linux/fb.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/smp.h>
#include <linux/init.h>

#include <asm/irq.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#ifdef CONFIG_AMIGA
#include <asm/amigahw.h>
#include <asm/amigaints.h>
#endif /* CONFIG_AMIGA */
#ifdef CONFIG_ATARI
#include <asm/atariints.h>
#endif
#ifdef CONFIG_MAC
#include <asm/macints.h>
#endif
#if defined(__mc68000__) || defined(CONFIG_APUS)
#include <asm/machdep.h>
#include <asm/setup.h>
#endif
#ifdef CONFIG_FBCON_VGA_PLANES
#include <asm/io.h>
#endif
#define INCLUDE_LINUX_LOGO_DATA
#include <asm/linux_logo.h>

#include "fbcon-mac.h"	/* for 6x11 font on mac */
#include "fbcon.h"
#include <video/font.h>

#ifdef FBCONDEBUG
#  define DPRINTK(fmt, args...) printk(KERN_DEBUG "%s: " fmt, __FUNCTION__ , ## args)
#else
#  define DPRINTK(fmt, args...)
#endif

#define LOGO_H			80
#define LOGO_W			80
#define LOGO_LINE	(LOGO_W/8)

static int logo_lines;
static int logo_shown = -1;

#define REFCOUNT(fd)	(((int *)(fd))[-1])
#define FNTSIZE(fd)	(((int *)(fd))[-2])
#define FNTCHARCNT(fd)	(((int *)(fd))[-3])
#define FNTSUM(fd)	(((int *)(fd))[-4])
#define FONT_EXTRA_WORDS 4

#define CM_SOFTBACK	(8)

#define advance_row(p, delta) (unsigned short *)((unsigned long)(p) + (delta) * vc->vc_size_row)
#define fontwidthvalid(p,w) ((p)->dispsw->fontwidthmask & FONTWIDTH(w))

static void fbcon_font_widths(struct display *);
static void fbcon_free_font(struct display *);
static int fbcon_set_origin(struct vc_data *);

/*
 * Emmanuel: fbcon will now use a hardware cursor if the
 * low-level driver provides a non-NULL dispsw->cursor pointer,
 * in which case the hardware should do blinking, etc.
 *
 * if dispsw->cursor is NULL, use Atari alike software cursor
 */

#define CURSOR_DRAW_DELAY		(1)

/* # VBL ints between cursor state changes */
#define ARM_CURSOR_BLINK_RATE		(10)
#define AMIGA_CURSOR_BLINK_RATE		(20)
#define ATARI_CURSOR_BLINK_RATE		(42)
#define MAC_CURSOR_BLINK_RATE		(32)
#define DEFAULT_CURSOR_BLINK_RATE	(20)

static inline void cursor_undrawn(struct fbvt_data *par)
{
    par->vbl_cursor_cnt = 0;
    par->cursor_drawn = 0;
}

#define divides(a, b)	((!(a) || (b)%(a)) ? 0 : 1)

/*
 *  Interface used by the world
 */

static const char *fbcon_startup(struct vt_struct *vt);
static void fbcon_init(struct vc_data *vc, int init);
static void fbcon_deinit(struct vc_data *vc);
static void fbcon_clear(struct vc_data *vc, int sy, int sx, int height,
		       int width);
static void fbcon_putc(struct vc_data *vc, int c, int ypos, int xpos);
static void fbcon_putcs(struct vc_data *vc,const unsigned short *s, int count,
			int ypos, int xpos);
static void fbcon_cursor(struct vc_data *vc, int mode);
static int fbcon_scroll(struct vc_data *vc, int t, int b, int dir,
			 int count);
static void fbcon_bmove(struct vc_data *vc, int sy, int sx, int dy, int dx,
			int height, int width);
static int fbcon_switch(struct vc_data *vc);
static int fbcon_blank(struct vc_data *vc, int blank);
static int fbcon_font_op(struct vc_data *vc, struct console_font_op *op);
static int fbcon_resize(struct vc_data *vc,unsigned int rows,unsigned int cols);
static int fbcon_set_palette(struct vc_data *vc, unsigned char *table);
static int fbcon_scrolldelta(struct vc_data *vc, int lines);

/*
 *  Internal routines
 */

static int __init fbcon_setup(char *options);
static __inline__ int fbcon_set_def_font(struct vc_data *vc, struct console_font_op
*op);
static __inline__ int real_y(struct display *p, int ypos);
static __inline__ void updatescrollmode(struct display *p);
static __inline__ void ywrap_up(struct vc_data *vc,struct display *p,int count);
static __inline__ void ywrap_down(struct vc_data *vc, struct display *p, 
				  int count);
static __inline__ void ypan_up(struct vc_data *vc, struct display *p,int count);
static __inline__ void ypan_down(struct vc_data *vc, struct display *p,
				 int count);
static void fbcon_bmove_rec(struct display *p, int sy, int sx, int dy, int dx,
			    int height, int width, u_int y_break);
static int fbcon_show_logo(struct vc_data *vc);

#ifdef CONFIG_MAC
/*
 * On the Macintoy, there may or may not be a working VBL int. We need to probe
 */
static int vbl_detected = 0;

static void fbcon_vbl_detect(int irq, void *dev_id, struct pt_regs *fp)
{
      vbl_detected++;
}
#endif

static void fbcon_vbl_handler(int irq, void *dev_id, struct pt_regs *fp)
{
    struct vt_struct *vt = (struct vt_struct *) dev_id;	
    struct fbvt_data *par = (struct fbvt_data *) vt->data_hook; 	
    struct display *p = &par->fb_display[vt->fg_console->vc_num];

    if (!par->cursor_on)
        return;

    if (par->vbl_cursor_cnt && --par->vbl_cursor_cnt == 0) {
        if (p->dispsw->revc)
                p->dispsw->revc(p, p->cursor_x, real_y(p, p->cursor_y));
        par->cursor_drawn ^= 1;
        par->vbl_cursor_cnt = par->cursor_blink_rate;
    }
}

static void cursor_timer_handler(unsigned long dev_id);

static struct timer_list cursor_timer = {
    function: cursor_timer_handler
};

static void cursor_timer_handler(unsigned long dev_id)
{
      fbcon_vbl_handler(0, dev_id, NULL);
      cursor_timer.expires = jiffies+HZ/50;
      add_timer(&cursor_timer);
}

static int __init fbcon_setup(char *options)
{
    if (!options || !*options)
            return 0;

/*
    if (!strncmp(options, "font:", 5))
        strcpy(fontname, options+5);

    if (!strncmp(options, "scrollback:", 11)) {
            options += 11;
            if (*options) {
                fbcon_softback_size = simple_strtoul(options, &options, 0);
                if (*options == 'k' || *options == 'K') {
                        fbcon_softback_size *= 1024;
                        options++;
                }
                if (*options != ',')
                        return 0;
                options++;
            } else
                return 0;
    }
*/
    return 0;
}

__setup("fbcon=", fbcon_setup);

/*
 *  Low Level Operations
 */

struct display_switch fbcon_dummy;

/* NOTE: fbcon cannot be __init: it may be called from take_over_console later */

static const char *fbcon_startup(struct vt_struct *vt)
{
    const char *display_desc = "frame buffer device";
    struct fbcon_font_desc *font = NULL;
    struct fbvt_data *par;
    int index, irqres = 1;	

    /*
     *  If num_registered_fb is zero, this is a call for the dummy part.
     *  The frame buffer devices weren't initialized yet.
     */
    if (!num_registered_fb)
	return display_desc;

    index = num_registered_fb--;
    par = kmalloc(sizeof(struct fbvt_data), GFP_KERNEL);	
    par->fb_info = registered_fb[index];	
	
#ifdef CONFIG_AMIGA
    if (MACH_IS_AMIGA) {
	par->cursor_blink_rate = AMIGA_CURSOR_BLINK_RATE;
	irqres = request_irq(IRQ_AMIGA_VERTB, fbcon_vbl_handler, 0,
			     "console/cursor", fbcon_vbl_handler);
    }
#endif /* CONFIG_AMIGA */
#ifdef CONFIG_ATARI
    if (MACH_IS_ATARI) {
	par->cursor_blink_rate = ATARI_CURSOR_BLINK_RATE;
	irqres = request_irq(IRQ_AUTO_4, fbcon_vbl_handler, IRQ_TYPE_PRIO,
			     "console/cursor", fbcon_vbl_handler);
    }
#endif /* CONFIG_ATARI */

#ifdef CONFIG_MAC
    /*
     * On a Macintoy, the VBL interrupt may or may not be active. 
     * As interrupt based cursor is more reliable and race free, we 
     * probe for VBL interrupts.
     */
    if (MACH_IS_MAC) {
	int ct = 0;
	/*
	 * Probe for VBL: set temp. handler ...
	 */
	irqres = request_irq(IRQ_MAC_VBL, fbcon_vbl_detect, 0,
			     "console/cursor", fbcon_vbl_detect);
	vbl_detected = 0;

	/*
	 * ... and spin for 20 ms ...
	 */
	while (!vbl_detected && ++ct<1000)
	    udelay(20);
 
	if(ct==1000)
	    printk("fbcon_startup: No VBL detected, using timer based cursor.\n");
 
	free_irq(IRQ_MAC_VBL, fbcon_vbl_detect);

	if (vbl_detected) {
	    /*
	     * interrupt based cursor ok
	     */
	    par->cursor_blink_rate = MAC_CURSOR_BLINK_RATE;
	    irqres = request_irq(IRQ_MAC_VBL, fbcon_vbl_handler, 0,
				 "console/cursor", fbcon_vbl_handler);
	} else {
	    /*
	     * VBL not detected: fall through, use timer based cursor
	     */
	    irqres = 1;
	}
    }
#endif /* CONFIG_MAC */

#if defined(__arm__) && defined(IRQ_VSYNCPULSE)
    par->cursor_blink_rate = ARM_CURSOR_BLINK_RATE;
    irqres = request_irq(IRQ_VSYNCPULSE, fbcon_vbl_handler, SA_SHIRQ,
			 "console/cursor", fbcon_vbl_handler);
#endif

    if (irqres) {
	par->cursor_blink_rate = DEFAULT_CURSOR_BLINK_RATE;
	cursor_timer.expires = jiffies+HZ/50;
	cursor_timer.data = (long) vt;
	add_timer(&cursor_timer);
    }
    
    if (!par->fb_info->fontname[0] ||
    	(font = fbcon_find_font(par->fb_info->fontname))) {
        vt->default_font.width = font->width;
        vt->default_font.height = font->height;
        vt->default_font.data = font->data;
    }

    if (par->fbcon_softback_size) {
    	if (!par->softback_buf) {
        	par->softback_buf = (unsigned long)kmalloc(par->fbcon_softback_size, GFP_KERNEL);
                if (!par->softback_buf) {
                    par->fbcon_softback_size = 0;
                    par->softback_top = 0;
                }
        }
    } else {
    	if (par->softback_buf) {
        	kfree((void *)par->softback_buf);
                par->softback_buf = 0;
                par->softback_top = 0;
        }
    }
    if (par->softback_buf) {
    	par->softback_in = par->softback_top = par->softback_curr = par->softback_buf;
        par->softback_lines = 0;
    }
    vt->data_hook = par;
    return display_desc;
}

static void fbcon_init(struct vc_data *vc, int init)
{
    struct fbvt_data *par = vc->display_fg->data_hook;
    struct module *owner = par->fb_info->fbops->owner;		
    struct display *p = par->fb_display[vc->vc_num];	
    unsigned short *save = NULL, *r, *q;
    struct fb_fix_screeninfo fix;	
    int old_rows, old_cols;
    int nr_rows, nr_cols;
    int charcnt = 256;
    int i, logo = !init;

    if (owner)
    	__MOD_INC_USE_COUNT(owner);
    if (par->fb_info->fbops->fb_open && par->fb_info->fbops->fb_open(par->fb_info,0) && owner)
         __MOD_DEC_USE_COUNT(owner);

    if ((i = par->fb_info->fbops->fb_get_fix(&fix, par->fb_info)))
	return;
    p = kmalloc(sizeof(struct display), GFP_KERNEL); 
    p->screen_base = par->fb_info->screen_base;
    p->visual = fix.visual;
    p->type = fix.type;
    p->type_aux = fix.type_aux;
    p->ypanstep = fix.ypanstep;
    p->ywrapstep = fix.ywrapstep;
    p->line_length = fix.line_length;
    if (par->fb_info->fbops->fb_blank || fix.visual == FB_VISUAL_PSEUDOCOLOR ||
        fix.visual == FB_VISUAL_DIRECTCOLOR)
        p->can_soft_blank = 1;
    else
        p->can_soft_blank = 0;
    
    p = par->fb_info->disp; /* copy from default */
    DPRINTK("mode:   %s\n", par->fb_info->modename);
    DPRINTK("visual: %d\n", p->visual);
    DPRINTK("res:    %dx%d-%d\n",p->var.xres,p->var.yres,p->var.bits_per_pixel);

    if (!IS_VISIBLE || (par->fb_info->flags & FBINFO_FLAG_MODULE) || p->type == FB_TYPE_TEXT)
    	logo = 0;

    p->var.xoffset = p->var.yoffset = p->yscroll = 0;  /* reset wrap/pan */

    if (!fontwidthvalid(p, vc->display_fg->default_font.width)) {
        /* ++Geert: changed from panic() to `correct and continue' */
        printk(KERN_ERR "fbcon_startup: No support for fontwidth %d\n", fontwidth(p));
        p->dispsw = &fbcon_dummy;
    }
    fbcon_set_def_font(vc, &vc->display_fg->default_font);
    fbcon_font_widths(p);
 
    if (p->dispsw->set_font)
    	p->dispsw->set_font(p, vc->vc_font->width, vc->vc_font->height);
    updatescrollmode(p);
    
    old_cols = vc->vc_cols;
    old_rows = vc->vc_rows;
    
    nr_cols = p->var.xres/vc->vc_font->width;
    nr_rows = p->var.yres/vc->vc_font->height;
    
    if (logo) {
    	/* Need to make room for the logo */
	int cnt;
	int step;
    
    	logo_lines = (LOGO_H + vc->vc_font->height - 1) / vc->vc_font->height;
    	q = (unsigned short *)(vc->vc_origin + vc->vc_size_row * old_rows);
    	step = logo_lines * old_cols;
    	for (r = q - logo_lines * old_cols; r < q; r++)
    	    if (scr_readw(r) != vc->vc_video_erase_char)
    	    	break;
	if (r != q && nr_rows >= old_rows + logo_lines) {
    	    save = kmalloc(logo_lines * nr_cols * 2, GFP_KERNEL);
    	    if (save) {
    	        int i = old_cols < nr_cols ? old_cols : nr_cols;
    	    	scr_memsetw(save, vc->vc_video_erase_char, logo_lines * nr_cols * 2);
    	    	r = q - step;
    	    	for (cnt = 0; cnt < logo_lines; cnt++, r += i)
    	    		scr_memcpyw_from(save + cnt * nr_cols, r, 2 * i);
    	    	r = q;
    	    }
    	}
    	if (r == q) {
    	    /* We can scroll screen down */
	    r = q - step - old_cols;
    	    for (cnt = old_rows - logo_lines; cnt > 0; cnt--) {
    	    	scr_memcpyw(r + step, r, vc->vc_size_row);
    	    	r -= old_cols;
    	    }
    	    if (!save) {
	    	vc->vc_y += logo_lines;
    		vc->vc_pos += logo_lines * vc->vc_size_row;
    	    }
    	}
    	scr_memsetw((unsigned short *)vc->vc_origin, vc->vc_video_erase_char,
    		     vc->vc_size_row * logo_lines);
    }
    
    /*
     *  ++guenther: console.c:vc_allocate() relies on initializing
     *  vc_{cols,rows}, but we must not set those if we are only
     *  resizing the console.
     */
    if (init) {
	vc->vc_cols = nr_cols;
	vc->vc_rows = nr_rows;
    }
    p->vrows = p->var.yres_virtual/vc->vc_font->height;
    if ((p->var.yres % vc->vc_font->height) &&
	(p->var.yres_virtual % vc->vc_font->height < p->var.yres % vc->vc_font->height))
	p->vrows--;
    vc->vc_can_do_color = p->var.bits_per_pixel != 1;
    vc->vc_complement_mask = vc->vc_can_do_color ? 0x7700 : 0x0800;
    if (charcnt == 256) {
    	vc->vc_hi_font_mask = 0;
    	p->fgshift = 8;
    	p->bgshift = 12;
    	p->charmask = 0xff;
    } else {
    	vc->vc_hi_font_mask = 0x100;
    	if (vc->vc_can_do_color)
	    vc->vc_complement_mask <<= 1;
    	p->fgshift = 9;
    	p->bgshift = 13;
    	p->charmask = 0x1ff;
    }

    if (p->dispsw == &fbcon_dummy)
	printk(KERN_WARNING "fbcon_set_disp: type %d (aux %d, depth %d) not "
	       "supported\n", p->type, p->type_aux, p->var.bits_per_pixel);
    p->dispsw->setup(p);

    p->fgcol = p->var.bits_per_pixel > 2 ? 7 : (1<<p->var.bits_per_pixel)-1;
    p->bgcol = 0;

    if (!init) {
	if (vc->vc_cols != nr_cols || vc->vc_rows != nr_rows)
	    vc_resize(vc, nr_rows, nr_cols);
	else if (IS_VISIBLE && vc->display_fg->vc_mode == KD_TEXT) {
	    if (p->dispsw->clear_margins)
		p->dispsw->clear_margins(vc, p, 0);
	    update_screen(vc);
	}
	if (save) {
    	    q = (unsigned short *)(vc->vc_origin + vc->vc_size_row * old_rows);
	    scr_memcpyw(q, save, logo_lines * nr_cols * 2);
	    vc->vc_y += logo_lines;
    	    vc->vc_pos += logo_lines * vc->vc_size_row;
    	    kfree(save);
	}
    }
	
    if (logo) {
	logo_shown = -2;
    	vc->vc_top = logo_lines;
    }
    
    if (IS_VISIBLE && par->softback_buf) {
    	int l = par->fbcon_softback_size / vc->vc_size_row;
    	if (l > 5)
    	    par->softback_end = par->softback_buf + l * vc->vc_size_row;
    	else {
    	    /* Smaller scrollback makes no sense, and 0 would screw
    	       the operation totally */
    	    par->softback_top = 0;
    	}
    }
}

static void fbcon_deinit(struct vc_data *vc)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];

    fbcon_free_font(p);
    p->dispsw = &fbcon_dummy;
}

static __inline__ void updatescrollmode(struct display *p)
{
    int m;
    if (p->scrollmode & __SCROLL_YFIXED)
    	return;
    if (divides(p->ywrapstep, vc->vc_font->height) &&
	divides(vc->vc_font->height, p->var.yres_virtual))
	m = __SCROLL_YWRAP;
    else if (divides(p->ypanstep, vc->vc_font->height) &&
	     p->var.yres_virtual >= p->var.yres+ vc->vc_font->height)
	m = __SCROLL_YPAN;
    else if (p->scrollmode & __SCROLL_YNOMOVE)
    	m = __SCROLL_YREDRAW;
    else
	m = __SCROLL_YMOVE;
    p->scrollmode = (p->scrollmode & ~__SCROLL_YMASK) | m;
}

static void fbcon_font_widths(struct display *p)
{
    int i;
    
    p->_fontwidthlog = 0;
    for (i = 2; i <= 6; i++)
    	if (vc->vc_font->width == (1 << i))
	    p->_fontwidthlog = i;
    p->_fontheightlog = 0;
    for (i = 2; i <= 6; i++)
    	if (vc->vc_font->height == (1 << i))
	    p->_fontheightlog = i;
}

/* ====================================================================== */

/*  fbcon_XXX routines - interface used by the world
 *
 *  This system is now divided into two levels because of complications
 *  caused by hardware scrolling. Top level functions:
 *
 *	fbcon_bmove(), fbcon_clear(), fbcon_putc()
 *
 *  handles y values in range [0, scr_height-1] that correspond to real
 *  screen positions. y_wrap shift means that first line of bitmap may be
 *  anywhere on this display. These functions convert lineoffsets to
 *  bitmap offsets and deal with the wrap-around case by splitting blits.
 *
 *	fbcon_bmove_physical_8()    -- These functions fast implementations
 *	fbcon_clear_physical_8()    -- of original fbcon_XXX fns.
 *	fbcon_putc_physical_8()	    -- (fontwidth != 8) may be added later
 *
 *  WARNING:
 *
 *  At the moment fbcon_putc() cannot blit across vertical wrap boundary
 *  Implies should only really hardware scroll in rows. Only reason for
 *  restriction is simplicity & efficiency at the moment.
 */

static __inline__ int real_y(struct display *p, int ypos)
{
    int rows = p->vrows;

    ypos += p->yscroll;
    return ypos < rows ? ypos : ypos-rows;
}

static void fbcon_clear(struct vc_data *vc, int sy, int sx, int height,
			int width)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    u_int y_break;
    int redraw_cursor = 0;

    if (!p->can_soft_blank && vc->display_fg->vt_blanked)
	return;

    if (!height || !width)
	return;

    if ((sy <= p->cursor_y) && (p->cursor_y < sy+height) &&
	(sx <= p->cursor_x) && (p->cursor_x < sx+width)) {
	cursor_undrawn(par);
	redraw_cursor = 1;
    }

    /* Split blits that cross physical y_wrap boundary */

    y_break = p->vrows-p->yscroll;
    if (sy < y_break && sy+height-1 >= y_break) {
	u_int b = y_break-sy;
	p->dispsw->clear(vc, p, real_y(p, sy), sx, b, width);
	p->dispsw->clear(vc, p, real_y(p, sy+b), sx, height-b, width);
    } else
	p->dispsw->clear(vc, p, real_y(p, sy), sx, height, width);

    if (redraw_cursor)
	par->vbl_cursor_cnt = CURSOR_DRAW_DELAY;
}

static void fbcon_putc(struct vc_data *vc, int c, int ypos, int xpos)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    int redraw_cursor = 0;

    if (!p->can_soft_blank && vc->display_fg->vt_blanked)
	    return;
	    
    if (vc->display_fg->vc_mode != KD_TEXT)
    	    return;

    if ((p->cursor_x == xpos) && (p->cursor_y == ypos)) {
	    cursor_undrawn(par);
	    redraw_cursor = 1;
    }

    p->dispsw->putc(vc, p, c, real_y(p, ypos), xpos);

    if (redraw_cursor)
	    par->vbl_cursor_cnt = CURSOR_DRAW_DELAY;
}

static void fbcon_putcs(struct vc_data *vc, const unsigned short *s, 
			int count, int ypos, int xpos)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    int redraw_cursor = 0;

    if (!p->can_soft_blank && vc->display_fg->vt_blanked)
	    return;

    if (vc->display_fg->vc_mode != KD_TEXT)
    	    return;

    if ((p->cursor_y == ypos) && (xpos <= p->cursor_x) &&
	(p->cursor_x < (xpos + count))) {
	    cursor_undrawn(par);
	    redraw_cursor = 1;
    }
    p->dispsw->putcs(vc, p, s, count, real_y(p, ypos), xpos);
    if (redraw_cursor)
	    par->vbl_cursor_cnt = CURSOR_DRAW_DELAY;
}

static void fbcon_cursor(struct vc_data *vc, int mode)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    int y = vc->vc_y;
    
    if (mode & CM_SOFTBACK) {
    	mode &= ~CM_SOFTBACK;
    	if (par->softback_lines) {
    	    if (y + par->softback_lines >= vc->vc_rows)
    		mode = CM_ERASE;
    	    else
    	        y += par->softback_lines;
    	}
    } else if (par->softback_lines)
        fbcon_set_origin(vc);

    /* do we have a hardware cursor ? */
    if (p->dispsw->cursor) {
	p->cursor_x = vc->vc_x;
	p->cursor_y = y;
	p->dispsw->cursor(p, mode, p->cursor_x, real_y(p, p->cursor_y));
	return;
    }

    /* Avoid flickering if there's no real change. */
    if (p->cursor_x == vc->vc_x && p->cursor_y == y &&
	(mode == CM_ERASE) == !par->cursor_on)
	return;

    par->cursor_on = 0;
    if (par->cursor_drawn)
        p->dispsw->revc(p, p->cursor_x, real_y(p, p->cursor_y));

    p->cursor_x = vc->vc_x;
    p->cursor_y = y;

    switch (mode) {
        case CM_ERASE:
            par->cursor_drawn = 0;
            break;
        case CM_MOVE:
        case CM_DRAW:
            if (par->cursor_drawn)
	        p->dispsw->revc(p, p->cursor_x, real_y(p, p->cursor_y));
            par->vbl_cursor_cnt = CURSOR_DRAW_DELAY;
            par->cursor_on = 1;
            break;
        }
}

static __inline__ void ywrap_up(struct vc_data *vc,struct display *p,int count)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	

    p->yscroll += count;
    if (p->yscroll >= p->vrows)	/* Deal with wrap */
	p->yscroll -= p->vrows;
    p->var.xoffset = 0;
    p->var.yoffset = p->yscroll * vc->vc_font->height;
    p->var.vmode |= FB_VMODE_YWRAP;
    if (IS_VISIBLE)	
    	par->fb_info->updatevar(vc->vc_num, par->fb_info);
    par->scrollback_max += count;
    if (par->scrollback_max > par->scrollback_phys_max)
	par->scrollback_max = par->scrollback_phys_max;
    par->scrollback_current = 0;
}

static __inline__ void ywrap_down(struct vc_data *vc, struct display *p,
				  int count)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	

    p->yscroll -= count;
    if (p->yscroll < 0)		/* Deal with wrap */
	p->yscroll += p->vrows;
    p->var.xoffset = 0;
    p->var.yoffset = p->yscroll * vc->vc_font->height;
    p->var.vmode |= FB_VMODE_YWRAP;
    if (IS_VISIBLE)	
   	 par->fb_info->updatevar(vc->vc_num, par->fb_info);
    par->scrollback_max -= count;
    if (par->scrollback_max < 0)
	par->scrollback_max = 0;
    par->scrollback_current = 0;
}

static __inline__ void ypan_up(struct vc_data *vc, struct display *p, int count)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	

    p->yscroll += count;
    if (p->yscroll > p->vrows - vc->vc_rows) {
	p->dispsw->bmove(p, p->vrows - vc->vc_rows, 0, 0, 0, vc->vc_rows,
			 vc->vc_cols);
	p->yscroll -= p->vrows - vc->vc_rows;
    }
    p->var.xoffset = 0;
    p->var.yoffset = p->yscroll * vc->vc_font->height;
    p->var.vmode &= ~FB_VMODE_YWRAP;
    if (IS_VISIBLE)	
    	par->fb_info->updatevar(vc->vc_num, par->fb_info);
    if (p->dispsw->clear_margins)
	p->dispsw->clear_margins(vc, p, 1);
    par->scrollback_max += count;
    if (par->scrollback_max > par->scrollback_phys_max)
	par->scrollback_max = par->scrollback_phys_max;
    par->scrollback_current = 0;
}

static __inline__ void ypan_down(struct vc_data *vc, struct display *p, 
				 int count)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	

    p->yscroll -= count;
    if (p->yscroll < 0) {
	p->dispsw->bmove(p, 0, 0, p->vrows - vc->vc_rows, 0, vc->vc_rows, 
			 vc->vc_cols);
	p->yscroll += p->vrows - vc->vc_rows;
    }
    p->var.xoffset = 0;
    p->var.yoffset = p->yscroll * vc->vc_font->height;
    p->var.vmode &= ~FB_VMODE_YWRAP;
    if (IS_VISIBLE)	
    	par->fb_info->updatevar(vc->vc_num, par->fb_info);
    if (p->dispsw->clear_margins)
	p->dispsw->clear_margins(vc, p, 1);
    par->scrollback_max -= count;
    if (par->scrollback_max < 0)
	par->scrollback_max = 0;
    par->scrollback_current = 0;
}

static void fbcon_redraw_softback(struct vc_data *vc, struct display *p, 
				  long delta)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    unsigned short *d, *s;
    unsigned long n;
    int line = 0;
    int count = vc->vc_rows;
    
    d = (u16 *)par->softback_curr;
    if (d == (u16 *)par->softback_in)
	d = (u16 *)vc->vc_origin;
    n = par->softback_curr + delta * vc->vc_size_row;
    par->softback_lines -= delta;
    if (delta < 0) {
        if (par->softback_curr < par->softback_top && n < par->softback_buf) {
            n += par->softback_end - par->softback_buf;
	    if (n < par->softback_top) {
		par->softback_lines -= (par->softback_top - n)/vc->vc_size_row;
		n = par->softback_top;
	    }
        } else if (par->softback_curr >= par->softback_top && n < par->softback_top) {
	    par->softback_lines -= (par->softback_top - n) / vc->vc_size_row;
	    n = par->softback_top;
        }
    } else {
    	if (par->softback_curr > par->softback_in && n >= par->softback_end) {
    	    n += par->softback_buf - par->softback_end;
	    if (n > par->softback_in) {
		n = par->softback_in;
		par->softback_lines = 0;
	    }
	} else if (par->softback_curr <= par->softback_in && n > par->softback_in) {
	    n = par->softback_in;
	    par->softback_lines = 0;
	}
    }
    if (n == par->softback_curr)
    	return;
    par->softback_curr = n;
    s = (u16 *)par->softback_curr;
    if (s == (u16 *)par->softback_in)
	s = (u16 *)vc->vc_origin;
    while (count--) {
	unsigned short *start;
	unsigned short *le;
	unsigned short c;
	int x = 0;
	unsigned short attr = 1;

	start = s;
	le = advance_row(s, 1);
	do {
	    c = scr_readw(s);
	    if (attr != (c & 0xff00)) {
		attr = c & 0xff00;
		if (s > start) {
		    p->dispsw->putcs(vc, p, start, s-start, real_y(p,line), x);
		    x += s - start;
		    start = s;
		}
	    }
	    if (c == scr_readw(d)) {
	    	if (s > start) {
	    	    p->dispsw->putcs(vc, p, start, s - start, real_y(p,line),x);
		    x += s - start + 1;
		    start = s + 1;
	    	} else {
	    	    x++;
	    	    start++;
	    	}
	    }
	    s++;
	    d++;
	} while (s < le);
	if (s > start)
	    p->dispsw->putcs(vc, p, start, s - start, real_y(p, line), x);
	line++;
	if (d == (u16 *) par->softback_end)
	    d = (u16 *) par->softback_buf;
	if (d == (u16 *) par->softback_in)
	    d = (u16 *)vc->vc_origin;
	if (s == (u16 *) par->softback_end)
	    s = (u16 *) par->softback_buf;
	if (s == (u16 *) par->softback_in)
	    s = (u16 *)vc->vc_origin;
    }
}

static void fbcon_redraw(struct vc_data *vc, struct display *p, 
			 int line, int count, int offset)
{
    unsigned short *d = (unsigned short *)
	(vc->vc_origin + vc->vc_size_row * line);
    unsigned short *s = d + offset;

    while (count--) {
	unsigned short *start = s;
	unsigned short *le = advance_row(s, 1);
	unsigned short c;
	int x = 0;
	unsigned short attr = 1;

	do {
	    c = scr_readw(s);
	    if (attr != (c & 0xff00)) {
		attr = c & 0xff00;
		if (s > start) {
		    p->dispsw->putcs(vc, p, start, s - start,
				     real_y(p, line), x);
		    x += s - start;
		    start = s;
		}
	    }
	    if (c == scr_readw(d)) {
	    	if (s > start) {
	    	    p->dispsw->putcs(vc, p, start, s - start,
				     real_y(p, line), x);
		    x += s - start + 1;
		    start = s + 1;
	    	} else {
	    	    x++;
	    	    start++;
	    	}
	    }
	    scr_writew(c, d);
	    s++;
	    d++;
	} while (s < le);
	if (s > start)
	    p->dispsw->putcs(vc, p, start, s - start, real_y(p, line), x);
	if (offset > 0)
		line++;
	else {
		line--;
		/* NOTE: We subtract two lines from these pointers */
		s -= vc->vc_size_row;
		d -= vc->vc_size_row;
	}
    }
}

void fbcon_redraw_clear(struct vc_data *vc, struct display *p, int sy, int sx,
		     int height, int width)
{
    int x, y;
    for (y=0; y<height; y++)
	for (x=0; x<width; x++)
	    fbcon_putc(vc, ' ', sy+y, sx+x);
}

/* This cannot be used together with ypan or ywrap */
void fbcon_redraw_bmove(struct vc_data *vc, int sy, int sx, int dy, int dx, 
			int h, int w)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];		

    if (sy != dy)
    	panic("fbcon_redraw_bmove width sy != dy");
    /* h will be always 1, but it does not matter if we are more generic */

    while (h-- > 0) {
	unsigned short *d = (unsigned short *)
		(vc->vc_origin + vc->vc_size_row * dy + dx * 2);
	unsigned short *s = d + (dx - sx);
	unsigned short *start = d;
	unsigned short *ls = d;
	unsigned short *le = d + w;
	unsigned short c;
	int x = dx;
	unsigned short attr = 1;

	do {
	    c = scr_readw(d);
	    if (attr != (c & 0xff00)) {
		attr = c & 0xff00;
		if (d > start) {
		    p->dispsw->putcs(vc, p, start, d - start, dy, x);
		    x += d - start;
		    start = d;
		}
	    }
	    if (s >= ls && s < le && c == scr_readw(s)) {
		if (d > start) {
		    p->dispsw->putcs(vc, p, start, d - start, dy, x);
		    x += d - start + 1;
		    start = d + 1;
		} else {
		    x++;
		    start++;
		}
	    }
	    s++;
	    d++;
	} while (d < le);
	if (d > start)
	    p->dispsw->putcs(vc, p, start, d - start, dy, x);
	sy++;
	dy++;
    }
}

static inline void fbcon_softback_note(struct vc_data *vc, int t, int count)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    unsigned short *p;

    if (!IS_VISIBLE)
	return;
    p = (unsigned short *)(vc->vc_origin + t * vc->vc_size_row);

    while (count) {
    	scr_memcpyw((u16 *)par->softback_in, p, vc->vc_size_row);
    	count--;
    	p = advance_row(p, 1);
    	par->softback_in += vc->vc_size_row;
    	if (par->softback_in == par->softback_end)
    	    par->softback_in = par->softback_buf;
    	if (par->softback_in == par->softback_top) {
    	    par->softback_top += vc->vc_size_row;
    	    if (par->softback_top == par->softback_end)
    	    	par->softback_top = par->softback_buf;
    	}
    }
    par->softback_curr = par->softback_in;
}

static int fbcon_scroll(struct vc_data *vc, int t, int b, int dir, int count)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    int scroll_partial = !(p->scrollmode & __SCROLL_YNOPARTIAL);

    if (!p->can_soft_blank && vc->display_fg->vt_blanked)
	return 0;

    if (!count || vc->display_fg->vc_mode != KD_TEXT)
	return 0;

    fbcon_cursor(vc, CM_ERASE);
    
    /*
     * ++Geert: Only use ywrap/ypan if the console is in text mode
     * ++Andrew: Only use ypan on hardware text mode when scrolling the
     *           whole screen (prevents flicker).
     */

    switch (dir) {
	case SM_UP:
	    if (count > vc->vc_rows)	/* Maximum realistic size */
		count = vc->vc_rows;
	    if (par->softback_top)
	        fbcon_softback_note(vc, t, count);
	    if (logo_shown >= 0) goto redraw_up;
	    switch (p->scrollmode & __SCROLL_YMASK) {
	    case __SCROLL_YMOVE:
		p->dispsw->bmove(p, t+count, 0, t, 0, b-t-count, vc->vc_cols);
		p->dispsw->clear(vc, p, b-count, 0, count, vc->vc_cols);
		break;
	    case __SCROLL_YWRAP:
		if (b-t-count > 3*vc->vc_rows>>2) {
		    if (t > 0)
			fbcon_bmove(vc, 0, 0, count, 0, t, vc->vc_cols);
			ywrap_up(vc, p, count);
		    if (vc->vc_rows-b > 0)
			fbcon_bmove(vc, b-count, 0, b, 0, vc->vc_rows - b,
				    vc->vc_cols);
		} else if (p->scrollmode & __SCROLL_YPANREDRAW)
		    goto redraw_up;
		else
		    fbcon_bmove(vc, t+count, 0, t, 0, b-t-count, vc->vc_cols);
		fbcon_clear(vc, b-count, 0, count, vc->vc_cols);
		break;

	    case __SCROLL_YPAN:
		if (( p->yscroll + count <= 2 * (p->vrows - vc->vc_rows)) &&
		    (( !scroll_partial && (b-t == vc->vc_rows)) ||
		     ( scroll_partial  && (b-t-count > 3*vc->vc_rows>>2)))) {
		    if (t > 0)
			fbcon_bmove(vc, 0, 0, count, 0, t, vc->vc_cols);
		    ypan_up(vc, p, count);
		    if (vc->vc_rows-b > 0)
			fbcon_bmove(vc, b-count, 0, b, 0, vc->vc_rows - b,
				    vc->vc_cols);
		} else if (p->scrollmode & __SCROLL_YPANREDRAW)
		    goto redraw_up;
		else
		    fbcon_bmove(vc, t+count, 0, t, 0, b-t-count, vc->vc_cols);
		fbcon_clear(vc, b-count, 0, count, vc->vc_cols);
		break;

	    case __SCROLL_YREDRAW:
	    redraw_up:
		fbcon_redraw(vc, p, t, b-t-count, count*vc->vc_cols);
		p->dispsw->clear(vc, p, real_y(p,b-count), 0,count,vc->vc_cols);
		scr_memsetw((unsigned short *)(vc->vc_origin + 
		    	    vc->vc_size_row * (b-count)), 
		    	    vc->vc_video_erase_char,
		    	    vc->vc_size_row * count);
		return 1;
	    }
	    break;

	case SM_DOWN:
	    if (count > vc->vc_rows)	/* Maximum realistic size */
		count = vc->vc_rows;
	    switch (p->scrollmode & __SCROLL_YMASK) {
	    case __SCROLL_YMOVE:
		p->dispsw->bmove(p, t, 0, t+count, 0, b-t-count, vc->vc_cols);
		p->dispsw->clear(vc, p, t, 0, count, vc->vc_cols);
		break;
	    case __SCROLL_YWRAP:
		if (b-t-count > 3*vc->vc_rows>>2) {
		    if (vc->vc_rows-b > 0)
			fbcon_bmove(vc, b, 0, b-count, 0, vc->vc_rows-b,
				    vc->vc_cols);
		    ywrap_down(vc, p, count);
		    if (t > 0)
			fbcon_bmove(vc, count, 0, 0, 0, t, vc->vc_cols);
		} else if (p->scrollmode & __SCROLL_YPANREDRAW)
		    goto redraw_down;
		else
		    fbcon_bmove(vc, t, 0, t+count, 0, b-t-count, vc->vc_cols);
		fbcon_clear(vc, t, 0, count, vc->vc_cols);
		break;
	    case __SCROLL_YPAN:
		if (( count-p->yscroll <= p->vrows - vc->vc_rows) &&
		    (( !scroll_partial && (b-t == vc->vc_rows)) ||
		     ( scroll_partial  && (b-t-count > 3*vc->vc_rows>>2)))) {
		    if (vc->vc_rows-b > 0)
			fbcon_bmove(vc, b, 0, b-count, 0, vc->vc_rows - b,
				    vc->vc_cols);
		    ypan_down(vc, p, count);
		    if (t > 0)
			fbcon_bmove(vc, count, 0, 0, 0, t, vc->vc_cols);
		} else if (p->scrollmode & __SCROLL_YPANREDRAW)
		    goto redraw_down;
		else
		    fbcon_bmove(vc, t, 0, t+count, 0, b-t-count, vc->vc_cols);
		fbcon_clear(vc, t, 0, count, vc->vc_cols);
		break;
	    case __SCROLL_YREDRAW:
	    redraw_down:
		fbcon_redraw(vc, p, b - 1, b-t-count, -count * vc->vc_cols);
		p->dispsw->clear(vc, p, real_y(p, t), 0, count, vc->vc_cols);
	    	scr_memsetw((unsigned short *)(vc->vc_origin + 
	    		    vc->vc_size_row * t), 
	    		    vc->vc_video_erase_char,
	    		    vc->vc_size_row * count);
	    	return 1;
	    }
    }
    return 0;
}

static void fbcon_bmove(struct vc_data *vc, int sy, int sx, int dy, int dx,
			int height, int width)
{
    struct fbvt_data *par = vc->display_fg->data_hook;
    struct display *p = par->fb_display[vc->vc_num];
    
    if (!p->can_soft_blank && vc->display_fg->vt_blanked)
	return;

    if (!width || !height)
	return;

    if (((sy <= p->cursor_y) && (p->cursor_y < sy+height) &&
	  (sx <= p->cursor_x) && (p->cursor_x < sx+width)) ||
	 ((dy <= p->cursor_y) && (p->cursor_y < dy+height) &&
	  (dx <= p->cursor_x) && (p->cursor_x < dx+width)))
	fbcon_cursor(vc, CM_ERASE|CM_SOFTBACK);

    /*  Split blits that cross physical y_wrap case.
     *  Pathological case involves 4 blits, better to use recursive
     *  code rather than unrolled case
     *
     *  Recursive invocations don't need to erase the cursor over and
     *  over again, so we use fbcon_bmove_rec()
     */
    fbcon_bmove_rec(p, sy, sx, dy, dx, height, width, p->vrows-p->yscroll);
}

static void fbcon_bmove_rec(struct display *p, int sy, int sx, int dy, int dx,
			    int height, int width, u_int y_break)
{
    u_int b;

    if (sy < y_break && sy+height > y_break) {
	b = y_break-sy;
	if (dy < sy) {	/* Avoid trashing self */
	    fbcon_bmove_rec(p, sy, sx, dy, dx, b, width, y_break);
	    fbcon_bmove_rec(p, sy+b, sx, dy+b, dx, height-b, width, y_break);
	} else {
	    fbcon_bmove_rec(p, sy+b, sx, dy+b, dx, height-b, width, y_break);
	    fbcon_bmove_rec(p, sy, sx, dy, dx, b, width, y_break);
	}
	return;
    }

    if (dy < y_break && dy+height > y_break) {
	b = y_break-dy;
	if (dy < sy) {	/* Avoid trashing self */
	    fbcon_bmove_rec(p, sy, sx, dy, dx, b, width, y_break);
	    fbcon_bmove_rec(p, sy+b, sx, dy+b, dx, height-b, width, y_break);
	} else {
	    fbcon_bmove_rec(p, sy+b, sx, dy+b, dx, height-b, width, y_break);
	    fbcon_bmove_rec(p, sy, sx, dy, dx, b, width, y_break);
	}
	return;
    }
    p->dispsw->bmove(p, real_y(p, sy), sx, real_y(p, dy), dx, height, width);
}

static int fbcon_switch(struct vc_data *vc)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    struct fb_info *info = par->fb_info;

    if (par->softback_top) {
    	int l = par->fbcon_softback_size / vc->vc_size_row;
	if (par->softback_lines)
	    fbcon_set_origin(vc);
        par->softback_top = par->softback_curr = par->softback_in = par->softback_buf;
        par->softback_lines = 0;

	if (l > 5)
	    par->softback_end = par->softback_buf + l * vc->vc_size_row;
	else {
	    /* Smaller scrollback makes no sense, and 0 would screw
	       the operation totally */
	    par->softback_top = 0;
	}
    }
    if (logo_shown >= 0) {
    	struct vc_data *vc2 = find_vc(logo_shown);
    	
    	if (vc2->vc_top == logo_lines && vc2->vc_bottom == vc2->vc_rows)
    		vc2->vc_top = 0;
    	logo_shown = -1;
    }
    p->var.yoffset = p->yscroll = 0;
    switch (p->scrollmode & __SCROLL_YMASK) {
	case __SCROLL_YWRAP:
	    par->scrollback_phys_max = p->vrows - vc->vc_rows;
	    break;
	case __SCROLL_YPAN:
	    par->scrollback_phys_max = p->vrows-2*vc->vc_rows;
	    if (par->scrollback_phys_max < 0)
		par->scrollback_phys_max = 0;
	    break;
	default:
	    par->scrollback_phys_max = 0;
	    break;
    }
    par->scrollback_max = 0;
    par->scrollback_current = 0;

    if (info && info->switch_con)
	(*info->switch_con)(vc->vc_num, info);
    if (p->dispsw->clear_margins && vc->display_fg->vc_mode == KD_TEXT)
	p->dispsw->clear_margins(vc, p, 0);
    if (logo_shown == -2) {
	logo_shown = vc->display_fg->fg_console->vc_num;
	/* This is protected above by initmem_freed */
	fbcon_show_logo(vc->display_fg->fg_console); 
	update_region(vc->display_fg->fg_console,
		      vc->vc_origin + vc->vc_size_row * vc->vc_top,
		      vc->vc_size_row * (vc->vc_bottom - vc->vc_top) / 2);
	return 0;
    }
    return 1;
}

static int fbcon_blank(struct vc_data *vc, int blank)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    struct fb_info *info = par->fb_info;

    if (blank < 0)	/* Entering graphics mode */
	return 0;

    fbcon_cursor(vc, blank ? CM_ERASE : CM_DRAW);

    if (!p->can_soft_blank) {
	if (blank) {
	    if (p->visual == FB_VISUAL_MONO01) {
		if (p->screen_base)
		    fb_memset255(p->screen_base,
				 p->var.xres_virtual*p->var.yres_virtual*
				 p->var.bits_per_pixel>>3);
	    } else {
	    	unsigned short oldc;
	    	u_int height;
	    	u_int y_break;

	    	oldc = vc->vc_video_erase_char;
	    	vc->vc_video_erase_char &= p->charmask;
	    	height = vc->vc_rows;
		y_break = p->vrows-p->yscroll;
		if (height > y_break) {
			p->dispsw->clear(vc, p, real_y(p, 0), 0, y_break, vc->vc_cols);
			p->dispsw->clear(vc, p, real_y(p, y_break), 0, height-y_break, vc->vc_cols);
		} else
			p->dispsw->clear(vc, p, real_y(p, 0), 0, height, vc->vc_cols);
		vc->vc_video_erase_char = oldc;
	    }
	    return 0;
	} else {
	    /* Tell console.c that it has to restore the screen itself */
	    return 1;
	}
    }
    (*info->fbops->fb_blank)(blank, info);
    return 0;
}

static void fbcon_free_font(struct display *p)
{
    if (p->userfont && p->fontdata &&
        (--REFCOUNT(p->fontdata) == 0))
	kfree(p->fontdata - FONT_EXTRA_WORDS*sizeof(int));
    p->fontdata = NULL;
    p->userfont = 0;
}

static inline int fbcon_get_font(struct vc_data *vc, struct console_font_op *op)
{
    u8 *fontdata = vc->vc_font->data;
    u8 *data = op->data;	
    int i, j;

#ifdef CONFIG_FBCON_FONTWIDTH8_ONLY
    if (vc->vc_font->width != 8) return -EINVAL;
#endif
    op->width = vc->vc_font->width;
    op->height = vc->vc_font->height;
    op->charcount = vc->vc_font->charcount;
    if (!op->data) return 0;
    
    if (op->width <= 8) {
	j = vc->vc_font->height;
    	for (i = 0; i < op->charcount; i++) {
	    memcpy(data, fontdata, j);
	    memset(data+j, 0, 32-j);
	    data += 32;
	    fontdata += j;
	}
    }
#ifndef CONFIG_FBCON_FONTWIDTH8_ONLY
    else if (op->width <= 16) {
	j = vc->vc_font->height * 2;
	for (i = 0; i < op->charcount; i++) {
	    memcpy(data, fontdata, j);
	    memset(data+j, 0, 64-j);
	    data += 64;
	    fontdata += j;
	}
    } else if (op->width <= 24) {
	for (i = 0; i < op->charcount; i++) {
	    for (j = 0; j < vc->vc_font->height; j++) {
		*data++ = fontdata[0];
		*data++ = fontdata[1];
		*data++ = fontdata[2];
		fontdata += sizeof(u32);
	    }
	    memset(data, 0, 3*(32-j));
	    data += 3 * (32 - j);
	}
    } else {
	j = vc->vc_font->height * 4;
	for (i = 0; i < op->charcount; i++) {
	    memcpy(data, fontdata, j);
	    memset(data+j, 0, 128-j);
	    data += 128;
	    fontdata += j;
	}
    }
#endif
    return 0;
}

static int fbcon_do_set_font(struct vc_data *vc, struct console_font_op *op, 
			     u8 *data, int userfont)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    char *old_data = NULL;	
    int w = op->width;
    int h = op->height;
    int resize;	
    int cnt;

    if (!fontwidthvalid(p,w)) {
        if (userfont && op->op != KD_FONT_OP_COPY)
	    kfree(data - FONT_EXTRA_WORDS*sizeof(int));
	return -ENXIO;
    }

    if (IS_VISIBLE && par->softback_lines)
	fbcon_set_origin(vc);
	
    resize = (w != fontwidth(p)) || (h != fontheight(p));
    if (p->userfont)
        old_data = p->fontdata;
    if (userfont)
        cnt = FNTCHARCNT(data);
    else
    	cnt = 256;
    p->fontdata = data;
    if ((p->userfont = userfont))
        REFCOUNT(data)++;
    p->_fontwidth = w;
    p->_fontheight = h;
    if (vc->vc_hi_font_mask && cnt == 256) {
    	vc->vc_hi_font_mask = 0;
    	if (vc->vc_can_do_color)
	    vc->vc_complement_mask >>= 1;
    	p->fgshift--;
    	p->bgshift--;
    	p->charmask = 0xff;

	/* ++Edmund: reorder the attribute bits */
	if (vc->vc_can_do_color) {
	    unsigned short *cp = (unsigned short *) vc->vc_origin;
	    int count = vc->vc_screenbuf_size/2;
	    unsigned short c;
	    for (; count > 0; count--, cp++) {
	        c = scr_readw(cp);
		scr_writew(((c & 0xfe00) >> 1) | (c & 0xff), cp);
	    }
	    c = vc->vc_video_erase_char;
	    vc->vc_video_erase_char = ((c & 0xfe00) >> 1) | (c & 0xff);
	    vc->vc_attr >>= 1;
	}

    } else if (!vc->vc_hi_font_mask && cnt == 512) {
    	vc->vc_hi_font_mask = 0x100;
    	if (vc->vc_can_do_color)
	    vc->vc_complement_mask <<= 1;
    	p->fgshift++;
    	p->bgshift++;
    	p->charmask = 0x1ff;

	/* ++Edmund: reorder the attribute bits */
	{
	    unsigned short *cp = (unsigned short *) vc->vc_origin;
	    int count = vc->vc_screenbuf_size/2;
	    unsigned short c;
	    for (; count > 0; count--, cp++) {
	        unsigned short newc;
	        c = scr_readw(cp);
		if (vc->vc_can_do_color)
		    newc = ((c & 0xff00) << 1) | (c & 0xff);
		else
		    newc = c & ~0x100;
		scr_writew(newc, cp);
	    }
	    c = vc->vc_video_erase_char;
	    if (vc->vc_can_do_color) {
		vc->vc_video_erase_char = ((c & 0xff00) << 1) | (c & 0xff);
		vc->vc_attr <<= 1;
	    } else
	        vc->vc_video_erase_char = c & ~0x100;
	}

    }
    fbcon_font_widths(p);

    if (resize) {
	/* reset wrap/pan */
	p->var.xoffset = p->var.yoffset = p->yscroll = 0;
	p->vrows = p->var.yres_virtual/h;
	if ((p->var.yres % h) && (p->var.yres_virtual % h < p->var.yres % h))
	    p->vrows--;
	updatescrollmode(p);
	vc_resize(vc, p->var.yres/h, p->var.xres/w);
        if (IS_VISIBLE && par->softback_buf) {
	    int l = par->fbcon_softback_size / vc->vc_size_row;
	    if (l > 5)
		par->softback_end = par->softback_buf + l * vc->vc_size_row;
	    else {
		/* Smaller scrollback makes no sense, and 0 would screw
		   the operation totally */
		par->softback_top = 0;
    	    }
    	}
    } else if (IS_VISIBLE && vc->display_fg->vc_mode == KD_TEXT) {
	if (p->dispsw->clear_margins)
	    p->dispsw->clear_margins(vc, p, 0);
	update_screen(vc);
    }

    if (old_data && (--REFCOUNT(old_data) == 0))
	kfree(old_data - FONT_EXTRA_WORDS*sizeof(int));

    return 0;
}

static inline int fbcon_copy_font(struct vc_data *vc,struct console_font_op *op)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *od, *p = par->fb_display[vc->vc_num];
    int h = op->height;

    if (h < 0 || !vc)
        return -ENOTTY;
    if (h == vc->vc_num)
        return 0; /* nothing to do */
    od = par->fb_display[h];
    if (od->fontdata == p->fontdata)
        return 0; /* already the same font... */
    op->width = fontwidth(od);
    op->height = fontheight(od);
    return fbcon_do_set_font(vc, op, od->fontdata, od->userfont);
}

static inline int fbcon_set_font(struct vc_data *vc, struct console_font_op *op)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *disp = par->fb_display[vc->vc_num];	
    int w = op->width;
    int h = op->height;
    int size = h;
    int i, k;
    u8 *new_data, *data = op->data, *p;

#ifdef CONFIG_FBCON_FONTWIDTH8_ONLY
    if (w != 8)
    	return -EINVAL;
#endif
    if ((w <= 0) || (w > 32) || (op->charcount != 256 && op->charcount != 512))
        return -EINVAL;
    
    if (w > 8) { 
    	if (w <= 16)
    		size *= 2;
    	else
    		size *= 4;
    }
    size *= op->charcount;
       
    if (!(new_data = kmalloc(FONT_EXTRA_WORDS*sizeof(int)+size, GFP_USER)))
        return -ENOMEM;
    new_data += FONT_EXTRA_WORDS*sizeof(int);
    FNTSIZE(new_data) = size;
    FNTCHARCNT(new_data) = op->charcount;
    REFCOUNT(new_data) = 0; /* usage counter */
    p = new_data;
    if (w <= 8) {
	for (i = 0; i < op->charcount; i++) {
	    memcpy(p, data, h);
	    data += 32;
	    p += h;
	}
    }
#ifndef CONFIG_FBCON_FONTWIDTH8_ONLY
    else if (w <= 16) {
	h *= 2;
	for (i = 0; i < op->charcount; i++) {
	    memcpy(p, data, h);
	    data += 64;
	    p += h;
	}
    } else if (w <= 24) {
	for (i = 0; i < op->charcount; i++) {
	    int j;
	    for (j = 0; j < h; j++) {
	        memcpy(p, data, 3);
		p[3] = 0;
		data += 3;
		p += sizeof(u32);
	    }
	    data += 3*(32 - h);
	}
    } else {
	h *= 4;
	for (i = 0; i < op->charcount; i++) {
	    memcpy(p, data, h);
	    data += 128;
	    p += h;
	}
    }
#endif
    /* we can do it in u32 chunks because of charcount is 256 or 512, so
       font length must be multiple of 256, at least. And 256 is multiple
       of 4 */
    k = 0;
    while (p > new_data) k += *--(u32 *)p;
    FNTSUM(new_data) = k;
    /* Check if the same font is on some other console already */
    for (i = 0; i < MAX_NR_USER_CONSOLES; i++) {
    	if (disp[i].userfont &&
    	    disp[i].fontdata &&
    	    FNTSUM(disp[i].fontdata) == k &&
    	    FNTSIZE(disp[i].fontdata) == size &&
    	    fontwidth(&disp[i]) == w &&
	    !memcmp(disp[i].fontdata, new_data, size)) {
	    kfree(new_data - FONT_EXTRA_WORDS*sizeof(int));
	    new_data = disp[i].fontdata;
	    break;
    	}
    }
    return fbcon_do_set_font(vc, op, new_data, 1);
}

static __inline__ int fbcon_set_def_font(struct vc_data *vc, struct console_font_op *op)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];	
    struct fbcon_font_desc *f; 	
    char name[MAX_FONT_NAME];

    if (!op->data)
	f = fbcon_get_default_font(p->var.xres, p->var.yres);
    else if (strncpy_from_user(name, op->data, MAX_FONT_NAME-1) < 0)
	return -EFAULT;
    else {
	name[MAX_FONT_NAME-1] = 0;
	if (!(f = fbcon_find_font(name)))
	    return -ENOENT;
    }
    op->width = f->width;
    op->height = f->height;
    return fbcon_do_set_font(vc, op, f->data, 0);
}

static int fbcon_font_op(struct vc_data *vc, struct console_font_op *op)
{
    switch (op->op) {
	case KD_FONT_OP_SET:
	    return fbcon_set_font(vc, op);
	case KD_FONT_OP_GET:
	    return fbcon_get_font(vc, op);
	case KD_FONT_OP_SET_DEFAULT:
	    return fbcon_set_def_font(vc, op);
	case KD_FONT_OP_COPY:
	    return fbcon_copy_font(vc, op);
	default:
	    return -ENOSYS;
    }
}

static int fbcon_resize(struct vc_data *vc,unsigned int rows,unsigned int cols)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];
    int err, charcnt = 256;
    
    p->var.xoffset = p->var.yoffset = p->yscroll = 0;  /* reset wrap/pan */

    if (IS_VISIBLE && p->type != FB_TYPE_TEXT) {   
	if (par->softback_buf)
	    par->softback_in = par->softback_top = par->softback_curr 
			     = par->softback_buf;
	par->softback_lines = 0;
    }
   
    p->var.xres = cols * fontwidth(p);
    p->var.yres = rows * fontheight(p);
    err = par->fb_info->fbops->fb_set_var(&p->var, par->fb_info); 
    if (err)
        return err;
 
    if (p->dispsw->set_font)
    	p->dispsw->set_font(p, fontwidth(p), fontheight(p));
    updatescrollmode(p);
   
    /*
     *  ++guenther: console.c:vc_allocate() relies on initializing
     *  vc_{cols,rows}, but we must not set those if we are only
     *  resizing the console.
     */
    p->vrows = p->var.yres_virtual/fontheight(p);
    if ((p->var.yres % fontheight(p)) &&
	(p->var.yres_virtual % fontheight(p) < p->var.yres % fontheight(p)))
	p->vrows--;
    vc->vc_can_do_color = p->var.bits_per_pixel != 1;
    vc->vc_complement_mask = vc->vc_can_do_color ? 0x7700 : 0x0800;
    if (charcnt == 256) {
    	vc->vc_hi_font_mask = 0;
    	p->fgshift = 8;
    	p->bgshift = 12;
    	p->charmask = 0xff;
    } else {
    	vc->vc_hi_font_mask = 0x100;
    	if (vc->vc_can_do_color)
	    vc->vc_complement_mask <<= 1;
    	p->fgshift = 9;
    	p->bgshift = 13;
    	p->charmask = 0x1ff;
    }

    if (p->dispsw == &fbcon_dummy)
	printk(KERN_WARNING "fbcon_resize: type %d (aux %d, depth %d) not "
	       "supported\n", p->type, p->type_aux, p->var.bits_per_pixel);
    p->dispsw->setup(p);

    p->fgcol = p->var.bits_per_pixel > 2 ? 7 : (1<<p->var.bits_per_pixel)-1;
    p->bgcol = 0;

    if (IS_VISIBLE && vc->display_fg->vc_mode == KD_TEXT) {
	if (p->dispsw->clear_margins) 
	    p->dispsw->clear_margins(vc, p, 0);
    }	
	
    if (IS_VISIBLE && par->softback_buf) {
    	int l = par->fbcon_softback_size / vc->vc_size_row;
    	if (l > 5)
    	    par->softback_end = par->softback_buf + l * vc->vc_size_row;
    	else {
    	    /* Smaller scrollback makes no sense, and 0 would screw
    	       the operation totally */
    	    par->softback_top = 0;
    	}
    }
    return 0;
}

static int fbcon_set_palette(struct vc_data *vc, unsigned char *table)
{
    struct fbvt_data *par = vc->display_fg->data_hook; 	
    struct display *p = par->fb_display[vc->vc_num];
    struct fb_cmap palette_cmap;	
    int size, i, j, k;
    u8 val;

    if ((!p->can_soft_blank && vc->display_fg->vt_blanked) ||
	!vc->vc_can_do_color)
	return -EINVAL;
    if (p->var.bits_per_pixel <= 4)
    	palette_cmap.len = 1<<p->var.bits_per_pixel;
    else
        palette_cmap.len = 16;
    size = palette_cmap.len * sizeof(u16);
    palette_cmap.start = 0;
    if (!(palette_cmap.red = kmalloc(size, GFP_ATOMIC)))
    	return -1;
    if (!(palette_cmap.green = kmalloc(size, GFP_ATOMIC)))
        return -1;
    if (!(palette_cmap.blue = kmalloc(size, GFP_ATOMIC)))
        return -1;
    palette_cmap.transp = NULL;
	
    for (i = j = 0; i < palette_cmap.len; i++) {
    	k = table[i];
	val = vc->vc_palette[j++];
	palette_cmap.red[k] = (val<<8)|val;
	val = vc->vc_palette[j++];
	palette_cmap.green[k] = (val<<8)|val;
	val = vc->vc_palette[j++];
	palette_cmap.blue[k] = (val<<8)|val;
    }
    return fb_set_cmap(&palette_cmap, 1, par->fb_info);
}

static u16 *fbcon_screen_pos(struct vc_data *vc, int offset)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	   
    unsigned long p;
    int line;	

    if (IS_VISIBLE || !par->softback_lines)
    	return (u16 *)(vc->vc_origin + offset);
    line = offset / vc->vc_size_row;
    if (line >= par->softback_lines)
    	return (u16 *)(vc->vc_origin + offset - par->softback_lines*vc->vc_size_row);
    p = par->softback_curr + offset;
    if (p >= par->softback_end)
    	p += par->softback_buf - par->softback_end;
    return (u16 *)p;
}

static unsigned long fbcon_getxy(struct vc_data *vc, unsigned long pos, int *px,
				 int *py)
{
    struct fbvt_data *par = vc->display_fg->data_hook;
    unsigned long ret;	
    int x, y;
    
    if (pos >= vc->vc_origin && pos < vc->vc_scr_end) {
    	unsigned long offset = (pos - vc->vc_origin) / 2;
    	
    	x = offset % vc->vc_cols;
    	y = offset / vc->vc_cols;
    	if (IS_VISIBLE)
    	    y += par->softback_lines;
    	ret = pos + (vc->vc_cols - x) * 2;
    } else if (IS_VISIBLE && par->softback_lines) {
    	unsigned long offset = (pos - par->softback_curr) / 2;
    	
    	x = offset % vc->vc_cols;
    	y = offset / vc->vc_cols;
    	if (pos < par->softback_curr)
	    y += (par->softback_end - par->softback_buf) / vc->vc_size_row;
	ret = pos + (vc->vc_cols - x) * 2;
	if (ret == par->softback_end)
	    ret = par->softback_buf;
	if (ret == par->softback_in)
	    ret = vc->vc_origin;
    } else {
    	/* Should not happen */
    	x = y = 0;
    	ret = vc->vc_origin;
    }
    if (px) *px = x;
    if (py) *py = y;
    return ret;
}

/* As we might be inside of softback, we may work with non-contiguous buffer,
   that's why we have to use a separate routine. */
static void fbcon_invert_region(struct vc_data *vc, u16 *p, int cnt)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	

    while (cnt--) {
	if (!vc->vc_can_do_color)
	    *p++ ^= 0x0800;
	else if (vc->vc_hi_font_mask == 0x100) {
	    u16 a = *p;
	    a = ((a) & 0x11ff) | (((a) & 0xe000) >> 4) | (((a) & 0x0e00) << 4);
	    *p++ = a;
	} else {
	    u16 a = *p;
	    a = ((a) & 0x88ff) | (((a) & 0x7000) >> 4) | (((a) & 0x0700) << 4);
	    *p++ = a;
	}
	if (p == (u16 *) par->softback_end)
	    p = (u16 *) par->softback_buf;
	if (p == (u16 *)par->softback_in)
	    p = (u16 *)vc->vc_origin;
    }
}

static int fbcon_scrolldelta(struct vc_data *vc, int lines)
{
    struct fbvt_data *par = vc->display_fg->data_hook;	
    struct display *p = par->fb_display[vc->vc_num];	
    int offset, limit, scrollback_old;
    
    if (par->softback_top) {
    	if (!IS_VISIBLE || vc->display_fg->vc_mode != KD_TEXT || !lines)
    	    return 0;
    	if (logo_shown >= 0) {
		struct vc_data *vc2 = find_vc(logo_shown);
    	
		if (vc2->vc_top == logo_lines && vc2->vc_bottom == vc2->vc_rows)
    		    vc2->vc_top = 0;
    		if (logo_shown == vc->vc_num) {
    		    unsigned long p, q;
    		    int i;
    		    
    		    p = par->softback_in;
    		    q = vc->vc_origin + logo_lines * vc->vc_size_row;
    		    for (i = 0; i < logo_lines; i++) {
    		    	if (p == par->softback_top) break;
    		    	if (p == par->softback_buf) p = par->softback_end;
    		    	p -= vc->vc_size_row;
    		    	q -= vc->vc_size_row;
    		    	scr_memcpyw((u16 *)q, (u16 *)p, vc->vc_size_row);
    		    }
    		    par->softback_in = p;
    		    update_region(vc, vc->vc_origin, logo_lines * vc->vc_cols);
    		}
		logo_shown = -1;
	}
    	fbcon_cursor(vc, CM_ERASE|CM_SOFTBACK);
    	fbcon_redraw_softback(vc, p, lines);
    	fbcon_cursor(vc, CM_DRAW|CM_SOFTBACK);
    	return 0;
    }

    if (!par->scrollback_phys_max)
	return -ENOSYS;

    scrollback_old = par->scrollback_current;
    par->scrollback_current -= lines;
    if (par->scrollback_current < 0)
	par->scrollback_current = 0;
    else if (par->scrollback_current > par->scrollback_max)
	par->scrollback_current = par->scrollback_max;
    if (par->scrollback_current == scrollback_old)
	return 0;

    if (!p->can_soft_blank &&
	(vc->display_fg->vt_blanked || vc->display_fg->vc_mode != KD_TEXT || !lines))
	return 0;
    fbcon_cursor(vc, CM_ERASE);

    offset = p->yscroll - par->scrollback_current;
    limit = p->vrows;
    switch (p->scrollmode && __SCROLL_YMASK) {
	case __SCROLL_YWRAP:
	    p->var.vmode |= FB_VMODE_YWRAP;
	    break;
	case __SCROLL_YPAN:
	    limit -= vc->vc_rows;
	    p->var.vmode &= ~FB_VMODE_YWRAP;
	    break;
    }
    if (offset < 0)
	offset += limit;
    else if (offset >= limit)
	offset -= limit;
    p->var.xoffset = 0;
    p->var.yoffset = offset*fontheight(p);
    if (IS_VISIBLE)	
    	par->fb_info->updatevar(vc->vc_num, par->fb_info);
    if (!par->scrollback_current)
	fbcon_cursor(vc, CM_DRAW);
    return 0;
}

static int fbcon_set_origin(struct vc_data *vc)
{
    struct fbvt_data *par = (struct fbvt_data *) vc->display_fg->data_hook; 

    if (par->softback_lines && !vc->display_fg->vt_blanked)
        fbcon_scrolldelta(vc, par->softback_lines);
    return 0;
}

static inline unsigned safe_shift(unsigned d,int n)
{
    return n<0 ? d>>-n : d<<n;
}

static int __init fbcon_show_logo(struct vc_data *vc)
{
    /* draw to vt in foreground */
    struct fbvt_data *par = vc->display_fg->data_hook;		
    struct display *p = par->fb_display[vc->vc_num]; 
    int depth = p->var.bits_per_pixel;
    int line = p->next_line;
    struct fb_cmap palette_cmap;
    unsigned char *fb = p->screen_base;
    unsigned char *logo;
    unsigned char *dst, *src;
    int i, j, n, x1, y1, x;
    int logo_depth, done = 0;

    /* Return if the frame buffer is not mapped */
    if (!fb)
	return 0;
	
    /* Set colors if visual is PSEUDOCOLOR and we have enough colors, or for
     * DIRECTCOLOR */
    if ((p->visual == FB_VISUAL_PSEUDOCOLOR && depth >= 4) ||
	p->visual == FB_VISUAL_DIRECTCOLOR) {
	int is_truecolor = (p->visual == FB_VISUAL_DIRECTCOLOR);
	int use_256 = (!is_truecolor && depth >= 8) ||
		      (is_truecolor && depth >= 24);
	int first_col = use_256 ? 32 : depth > 4 ? 16 : 0;
	int num_cols = use_256 ? LINUX_LOGO_COLORS : 16;
	unsigned char *red, *green, *blue;
	
	if (use_256) {
	    red   = linux_logo_red;
	    green = linux_logo_green;
	    blue  = linux_logo_blue;
	}
	else {
	    red   = linux_logo16_red;
	    green = linux_logo16_green;
	    blue  = linux_logo16_blue;
	}

	for( i = 0; i < num_cols; i += n ) {
	    n = num_cols - i;
	    if (n > 16)
		/* palette_cmap provides space for only 16 colors at once */
		n = 16;
	    palette_cmap.start = first_col + i;
	    palette_cmap.len   = n;
	    for( j = 0; j < n; ++j ) {
		palette_cmap.red[j]   = (red[i+j] << 8) | red[i+j];
		palette_cmap.green[j] = (green[i+j] << 8) | green[i+j];
		palette_cmap.blue[j]  = (blue[i+j] << 8) | blue[i+j];
	    }
	    fb_set_cmap(&palette_cmap, 1, par->fb_info); 
	}
    }
	
    if (depth >= 8) {
	logo = linux_logo;
	logo_depth = 8;
    }
    else if (depth >= 4) {
	logo = linux_logo16;
	logo_depth = 4;
    }
    else {
	logo = linux_logo_bw;
	logo_depth = 1;
    }
    
    if (par->fb_info->fbops->fb_rasterimg)
    	par->fb_info->fbops->fb_rasterimg(par->fb_info, 1);

    for (x = 0; x < smp_num_cpus * (LOGO_W + 8) &&
    	 x < p->var.xres - (LOGO_W + 8); x += (LOGO_W + 8)) {
    	 
#if defined(CONFIG_FBCON_CFB16) || defined(CONFIG_FBCON_CFB24) || \
    defined(CONFIG_FBCON_CFB32) || defined(CONFIG_FB_SBUS)
        if (p->visual == FB_VISUAL_DIRECTCOLOR) {
	    unsigned int val;		/* max. depth 32! */
	    int bdepth;
	    int redshift, greenshift, blueshift;
		
	    /* Bug: Doesn't obey msb_right ... (who needs that?) */
	    redshift   = p->var.red.offset;
	    greenshift = p->var.green.offset;
	    blueshift  = p->var.blue.offset;

	    if (depth >= 24 && (depth % 8) == 0) {
		/* have at least 8 bits per color */
		src = logo;
		bdepth = depth/8;
		for( y1 = 0; y1 < LOGO_H; y1++ ) {
		    dst = fb + y1*line + x*bdepth;
		    for( x1 = 0; x1 < LOGO_W; x1++, src++ ) {
			val = (*src << redshift) |
			      (*src << greenshift) |
			      (*src << blueshift);
			if (bdepth == 4 && !((long)dst & 3)) {
			    /* Some cards require 32bit access */
			    fb_writel (val, dst);
			    dst += 4;
			} else {
#ifdef __LITTLE_ENDIAN
			    for( i = 0; i < bdepth; ++i )
#else
			    for( i = bdepth-1; i >= 0; --i )
#endif
			        fb_writeb (val >> (i*8), dst++);
			}
		    }
		}
	    }
	    else if (depth >= 15 && depth <= 23) {
	        /* have 5..7 bits per color, using 16 color image */
		unsigned int pix;
		src = linux_logo16;
		bdepth = (depth+7)/8;
		for( y1 = 0; y1 < LOGO_H; y1++ ) {
		    dst = fb + y1*line + x*bdepth;
		    for( x1 = 0; x1 < LOGO_W/2; x1++, src++ ) {
			pix = (*src >> 4) | 0x10; /* upper nibble */
			val = (pix << redshift) |
			      (pix << greenshift) |
			      (pix << blueshift);
#ifdef __LITTLE_ENDIAN
			for( i = 0; i < bdepth; ++i )
#else
			for( i = bdepth-1; i >= 0; --i )
#endif
			    fb_writeb (val >> (i*8), dst++);
			pix = (*src & 0x0f) | 0x10; /* lower nibble */
			val = (pix << redshift) |
			      (pix << greenshift) |
			      (pix << blueshift);
#ifdef __LITTLE_ENDIAN
			for( i = 0; i < bdepth; ++i )
#else
			for( i = bdepth-1; i >= 0; --i )
#endif
			    fb_writeb (val >> (i*8), dst++);
		    }
		}
	    }
	    done = 1;
        }
#endif
#if defined(CONFIG_FBCON_CFB16) || defined(CONFIG_FBCON_CFB24) || \
    defined(CONFIG_FBCON_CFB32) || defined(CONFIG_FB_SBUS)
	if ((depth % 8 == 0) && (p->visual == FB_VISUAL_TRUECOLOR)) {
	    /* Modes without color mapping, needs special data transformation... */
	    unsigned int val;		/* max. depth 32! */
	    int bdepth = depth/8;
	    unsigned char mask[9] = { 0,0x80,0xc0,0xe0,0xf0,0xf8,0xfc,0xfe,0xff };
	    unsigned char redmask, greenmask, bluemask;
	    int redshift, greenshift, blueshift;
		
	    /* Bug: Doesn't obey msb_right ... (who needs that?) */
	    redmask   = mask[p->var.red.length   < 8 ? p->var.red.length   : 8];
	    greenmask = mask[p->var.green.length < 8 ? p->var.green.length : 8];
	    bluemask  = mask[p->var.blue.length  < 8 ? p->var.blue.length  : 8];
	    redshift   = p->var.red.offset   - (8-p->var.red.length);
	    greenshift = p->var.green.offset - (8-p->var.green.length);
	    blueshift  = p->var.blue.offset  - (8-p->var.blue.length);

	    src = logo;
	    for( y1 = 0; y1 < LOGO_H; y1++ ) {
		dst = fb + y1*line + x*bdepth;
		for( x1 = 0; x1 < LOGO_W; x1++, src++ ) {
		    val = safe_shift((linux_logo_red[*src-32]   & redmask), redshift) |
		          safe_shift((linux_logo_green[*src-32] & greenmask), greenshift) |
		          safe_shift((linux_logo_blue[*src-32]  & bluemask), blueshift);
		    if (bdepth == 4 && !((long)dst & 3)) {
			/* Some cards require 32bit access */
			fb_writel (val, dst);
			dst += 4;
		    } else {
#ifdef __LITTLE_ENDIAN
			for( i = 0; i < bdepth; ++i )
#else
			for( i = bdepth-1; i >= 0; --i )
#endif
			    fb_writeb (val >> (i*8), dst++);
		    }
		}
	    }
	    done = 1;
	}
#endif
#if defined(CONFIG_FBCON_CFB4)
	if (depth == 4 && p->type == FB_TYPE_PACKED_PIXELS) {
		src = logo;
		for( y1 = 0; y1 < LOGO_H; y1++) {
			dst = fb + y1*line + x/2;
			for( x1 = 0; x1 < LOGO_W/2; x1++) {
				u8 q = *src++;
				q = (q << 4) | (q >> 4);
				fb_writeb (q, dst++);
			}
		}
		done = 1;
	}
#endif
#if defined(CONFIG_FBCON_CFB8) || defined(CONFIG_FB_SBUS)
	if (depth == 8 && p->type == FB_TYPE_PACKED_PIXELS) {
	    /* depth 8 or more, packed, with color registers */
		
	    src = logo;
	    for( y1 = 0; y1 < LOGO_H; y1++ ) {
		dst = fb + y1*line + x;
		for( x1 = 0; x1 < LOGO_W; x1++ )
		    fb_writeb (*src++, dst++);
	    }
	    done = 1;
	}
#endif
#if defined(CONFIG_FBCON_AFB) || defined(CONFIG_FBCON_ILBM) || \
    defined(CONFIG_FBCON_IPLAN2P2) || defined(CONFIG_FBCON_IPLAN2P4) || \
    defined(CONFIG_FBCON_IPLAN2P8)
	if (depth >= 2 && (p->type == FB_TYPE_PLANES ||
			   p->type == FB_TYPE_INTERLEAVED_PLANES)) {
	    /* planes (normal or interleaved), with color registers */
	    int bit;
	    unsigned char val, mask;
	    int plane = p->next_plane;

#if defined(CONFIG_FBCON_IPLAN2P2) || defined(CONFIG_FBCON_IPLAN2P4) || \
    defined(CONFIG_FBCON_IPLAN2P8)
	    int line_length = p->line_length;

	    /* for support of Atari interleaved planes */
#define MAP_X(x)	(line_length ? (x) : ((x) & ~1)*depth + ((x) & 1))
#else
#define MAP_X(x)	(x)
#endif
	    /* extract a bit from the source image */
#define	BIT(p,pix,bit)	(p[pix*logo_depth/8] & \
			 (1 << ((8-((pix*logo_depth)&7)-logo_depth) + bit)))
		
	    src = logo;
	    for( y1 = 0; y1 < LOGO_H; y1++ ) {
		for( x1 = 0; x1 < LOGO_LINE; x1++, src += logo_depth ) {
		    dst = fb + y1*line + MAP_X(x/8+x1);
		    for( bit = 0; bit < logo_depth; bit++ ) {
			val = 0;
			for( mask = 0x80, i = 0; i < 8; mask >>= 1, i++ ) {
			    if (BIT( src, i, bit ))
				val |= mask;
			}
			*dst = val;
			dst += plane;
		    }
		}
	    }
	
	    /* fill remaining planes
	     * special case for logo_depth == 4: we used color registers 16..31,
	     * so fill plane 4 with 1 bits instead of 0 */
	    if (depth > logo_depth) {
		for( y1 = 0; y1 < LOGO_H; y1++ ) {
		    for( x1 = 0; x1 < LOGO_LINE; x1++ ) {
			dst = fb + y1*line + MAP_X(x/8+x1) + logo_depth*plane;
			for( i = logo_depth; i < depth; i++, dst += plane )
			    *dst = (i == logo_depth && logo_depth == 4)
				   ? 0xff : 0x00;
		    }
		}
	    }
	    done = 1;
	    break;
	}
#endif
#if defined(CONFIG_FBCON_MFB) || defined(CONFIG_FBCON_AFB) || \
    defined(CONFIG_FBCON_ILBM) || defined(CONFIG_FBCON_HGA)

	if (depth == 1 && (p->type == FB_TYPE_PACKED_PIXELS ||
			   p->type == FB_TYPE_PLANES ||
			   p->type == FB_TYPE_INTERLEAVED_PLANES)) {

	    /* monochrome */
	    unsigned char inverse = p->inverse || p->visual == FB_VISUAL_MONO01
		? 0x00 : 0xff;

	    int is_hga = !strncmp(par->fb_info->modename, "HGA", 3);
	    /* can't use simply memcpy because need to apply inverse */
	    for( y1 = 0; y1 < LOGO_H; y1++ ) {
		src = logo + y1*LOGO_LINE;
		if (is_hga)
		    dst = fb + (y1%4)*8192 + (y1>>2)*line + x/8;
		else
		    dst = fb + y1*line + x/8;
		for( x1 = 0; x1 < LOGO_LINE; ++x1 )
		    fb_writeb(fb_readb(src++) ^ inverse, dst++);
	    }
	    done = 1;
	}
#endif
#if defined(CONFIG_FBCON_VGA_PLANES)
	if (depth == 4 && p->type == FB_TYPE_VGA_PLANES) {
		outb_p(1,0x3ce); outb_p(0xf,0x3cf);
		outb_p(3,0x3ce); outb_p(0,0x3cf);
		outb_p(5,0x3ce); outb_p(0,0x3cf);

		src = logo;
		for (y1 = 0; y1 < LOGO_H; y1++) {
			for (x1 = 0; x1 < LOGO_W / 2; x1++) {
				dst = fb + y1*line + x1/4 + x/8;

				outb_p(0,0x3ce);
				outb_p(*src >> 4,0x3cf);
				outb_p(8,0x3ce);
				outb_p(1 << (7 - x1 % 4 * 2),0x3cf);
				fb_readb (dst);
				fb_writeb (0, dst);

				outb_p(0,0x3ce);
				outb_p(*src & 0xf,0x3cf);
				outb_p(8,0x3ce);
				outb_p(1 << (7 - (1 + x1 % 4 * 2)),0x3cf);
				fb_readb (dst);
				fb_writeb (0, dst);

				src++;
			}
		}
		done = 1;
	}
#endif			
    }
    
    if (par->fb_info->fbops->fb_rasterimg)
    	par->fb_info->fbops->fb_rasterimg(par->fb_info, 0);

    /* Modes not yet supported: packed pixels with depth != 8 (does such a
     * thing exist in reality?) */

    return done ? (LOGO_H + fontheight(p) - 1) / fontheight(p) : 0 ;
}

/*
 *  The console `switch' structure for the frame buffer based console
 */
 
struct consw fb_con = {
    con_startup: 	fbcon_startup, 
    con_init: 		fbcon_init,
    con_deinit: 	fbcon_deinit,
    con_clear: 		fbcon_clear,
    con_putc: 		fbcon_putc,
    con_putcs: 		fbcon_putcs,
    con_cursor: 	fbcon_cursor,
    con_scroll: 	fbcon_scroll,
    con_bmove: 		fbcon_bmove,
    con_switch: 	fbcon_switch,
    con_blank: 		fbcon_blank,
    con_font_op:	fbcon_font_op,
    con_resize:		fbcon_resize,
    con_set_palette: 	fbcon_set_palette,
    con_scrolldelta: 	fbcon_scrolldelta,
    con_set_origin: 	fbcon_set_origin,
    con_invert_region:	fbcon_invert_region,
    con_screen_pos:	fbcon_screen_pos,
    con_getxy:		fbcon_getxy,
};

/*
 *  Dummy Low Level Operations
 */

static void fbcon_dummy_op(void) {}

#define DUMMY	(void *)fbcon_dummy_op

struct display_switch fbcon_dummy = {
    setup:	DUMMY,
    bmove:	DUMMY,
    clear:	DUMMY,
    putc:	DUMMY,
    putcs:	DUMMY,
    revc:	DUMMY,
};

/*
 *  Visible symbols for modules
 */

EXPORT_SYMBOL(fbcon_redraw_bmove);
EXPORT_SYMBOL(fbcon_redraw_clear);
EXPORT_SYMBOL(fbcon_dummy);
EXPORT_SYMBOL(fb_con);
