/*
 *	$Id$
 *
 *	Procfs interface for the VT-handler.
 *
 *	Aivils Stoss <>
 */

#include <linux/config.h>

#ifdef CONFIG_PROC_FS
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/vt_kern.h>
#include <linux/input.h>
#include <asm/uaccess.h>
#include <linux/module.h>

#define VT_PROC_DIR "console"
#define WRITE_BUF_MAX_LEN 256

typedef struct _vt_proc_entry {
        char *name;
        read_proc_t *read_proc;
        write_proc_t *write_proc;
        unsigned long offset;
} vt_proc_entry;

static int
generic_read(char *page, char **start, off_t off, int count, int *eof, int len)
{
        if (len <= off + count)
                *eof = 1;

        *start = page + off;
        len -= off;
        if (len > count)
                len = count;

        if (len < 0)
                len = 0;

        return len;
}

static int
read_kbd_phys(char *page, char **start, off_t off, int count, int *eof, void *data)
{
        struct vt_struct *vt = (struct vt_struct*) data;
        int len;

	if(!vt || !vt->keyboard) return 0;

        len = sprintf(page, "%s\n", vt->keyboard->dev->phys ? vt->keyboard->dev->phys : "");

        return generic_read(page, start, off, count, eof, len);
}

static int
write_kbd_phys(struct file *file, const char *buffer,
	       unsigned long count, void *data)
{
        struct vt_struct *vt = (struct vt_struct*) data;
	struct input_handle *handle;
	char phys_descr[WRITE_BUF_MAX_LEN + 1];
	int add_next = 0;

        if (!vt || !buffer)
                return -EINVAL;

        if (count > WRITE_BUF_MAX_LEN) {
                count = WRITE_BUF_MAX_LEN;
        }
        if (copy_from_user(phys_descr, buffer, count))
                return -EFAULT;
        
	if(phys_descr[count-1] == 0x0A) phys_descr[count-1] = '\0';
	else phys_descr[count] = '\0';

	if(phys_descr[0] == 0x2B) //1st is "+" sign
		add_next = 1;
	handle = input_find_handle(phys_descr+add_next);
	if(handle) {
	        if(handle->private) ((struct vt_struct*)handle->private)->keyboard = NULL;
		if(!add_next) {
	        	if(vt->keyboard) vt->keyboard->private = NULL;
			vt->keyboard = handle;
		}
		handle->private = vt;
	}
	return count;
}

static vt_proc_entry vt_proc_list[] = {
        {"keyboard",           read_kbd_phys,    write_kbd_phys, 0},
        {"", 0, 0, 0}
};

static struct proc_dir_entry *
create_proc_rw(char *name, void *data, struct proc_dir_entry *parent,
               read_proc_t * read_proc, write_proc_t * write_proc)
{
        struct proc_dir_entry *pdep;
        mode_t mode = S_IFREG;

        if (write_proc) {
                mode |= S_IWUSR;
                if (read_proc) {
                        mode |= S_IRUGO;
                }

        } else if (read_proc) {
                mode |= S_IRUGO;
        }

        if (!(pdep = create_proc_entry(name, mode, parent)))
                return NULL;

        pdep->read_proc = read_proc;
        pdep->write_proc = write_proc;
        pdep->data = data;
        return pdep;
}


struct proc_dir_entry *proc_bus_console_dir;

int vt_proc_attach(struct vt_struct *vt)
{
	struct proc_dir_entry *de;
        vt_proc_entry *pe;
	char name[16];

	if (!(de = vt->procdir)) {
		sprintf(name, "%02x", vt->vt_num);
		de = vt->procdir = proc_mkdir(name, proc_bus_console_dir);
		if (!de)
			return -ENOMEM;
	}

        for (pe = vt_proc_list; pe->name[0]; pe++) {
                if (pe->name[0] == '\n')
                        continue;
                if (!(create_proc_rw(pe->name, (void*) vt, vt->procdir,
                                     pe->read_proc, pe->write_proc))) {
                        return -ENOMEM;
                }
	}

	return 0;
}
EXPORT_SYMBOL(vt_proc_attach);

int vt_proc_detach(struct vt_struct *vt)
{
	struct proc_dir_entry *e;
        vt_proc_entry *pe;

	if ((e = vt->procdir)) {
                for (pe = vt_proc_list; pe->name[0]; pe++) {
                        if (pe->name[0] == '\n')
                                continue;

                        remove_proc_entry(pe->name, vt->procdir);
			vt->procdir = NULL;
		}

		remove_proc_entry(e->name, proc_bus_console_dir);
	}

	return 0;
}
EXPORT_SYMBOL(vt_proc_detach);

int __init vt_proc_init(void)
{
        struct vt_struct *vt;
        
        if (list_empty(&vt_list))
                return 0;
        
        proc_bus_console_dir = proc_mkdir(VT_PROC_DIR, proc_bus);
        list_for_each_entry (vt, &vt_list, node) {
                vt_proc_attach(vt);
        }

        return 0;
}

#endif /* CONFIG_PROC_FS */
