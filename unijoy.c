#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/major.h>
#include <linux/cdev.h>
#include <linux/signal.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <linux/list.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/sched.h>

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Alexander <iamtaingiteasy> Tumin");
MODULE_DESCRIPTION("Makes a union of N other input/js devices");
MODULE_SUPPORTED_DEVICE("input/jsN");

/**
 * This module is providing a virtual joystick device which is composed
 * of N real devices.
 *
 * It uses sysfs special file to configure itself and provide some feedback
 * to potential users.
 *
 * The main idea of how it works is in the following flow:
 *
 * -> *Initialization, leads to creating a new dummy joystick device*
 * -> Registering input handlers in order to communicate with real devices
 * -> When new real device showin up, this module is storing in internal
 *    registry
 * -> When user sends a special string (command with parameters) to a sysfs
 *    device, this module reads it and acts correspondly.
 *    Possible commands are:
 *      * merge <ID>
 *            tells module to merge device with specified ID
 *
 *      * unmerge <ID>       
 *            tells module to unmerge device with specified ID
 *     
 *      * add_button <ID> <SOURCE BUTTON #> [DEST BUTTON #]
 *            tells module to map a SOURCE BUTTON # of real device identified
 *            by ID to a DEST BUTTON # of virtual merge device.
 *            DEST BUTTON # argument is optional, if ommitted, last unused #
 *            will be used.
 *
 *      * del_button <DEST BUTTON #>
 *            tells module to unmap a DEST BUTTON # from virtual merge device
 *
 *      * add_axis <ID> <SOURCE AXIS #> [DEST AXIS #]
 *            same as add_button, only acts for axes
 *
 *      * del_axis <DEST_AXIS #>
 *            same as del_button, only acts for axes
 *
 * -> When input kernel subsystem fires an event, this module captures it,
 *    finds out under which (if any) destination button or axis it should
 *    look like and then passes it to each of clients (readers if virtual
 *    merge device)
 *
 * -> If merged device goes away (unplugged), it is not wiped from internal
 *    registry of this module, instead it is marked as suspended in case
 *    it will show up again, to merge it upon this case.
 *
 * 
 * NOTE: you can map the same real axis or button to any number of virtual
 *       merge device axis or buttons. Not sure if this is useful, but it 
 *       may have it's use-cases.
 */

#define UNIJOY_MINOR_BASE 0
#define UNIJOY_MINORS 16
#define UNIJOY_BUFFER_SIZE 64
#define UNIJOY_MAX_BUTTONS (KEY_MAX - BTN_MISC + 1)

struct unijoy_fops_client {
  struct js_event buffer[UNIJOY_MAX_BUTTONS];
  int head;
  int tail;
  int startup;
  spinlock_t buffer_lock;
  struct fasync_struct *fasync;
  struct list_head list;
};

/* File operations */

static int unijoy_fops_open(struct inode *, struct file *);
static int unijoy_fops_release(struct inode *, struct file *);
static ssize_t unijoy_fops_read(struct file *, char __user *, 
                                size_t, loff_t *);
static long unijoy_fops_ioctl(struct file *, unsigned int, unsigned long);
static long unijoy_fops_ioctl_common(unsigned int, void __user *);
static int unijoy_fops_fasync(int, struct file *, int);
static void unijoy_fops_attach(struct unijoy_fops_client *);
static void unijoy_fops_detach(struct unijoy_fops_client *);
static inline int unijoy_fops_pending(struct unijoy_fops_client *);
static int unijoy_fops_startup(struct unijoy_fops_client *, struct js_event *);
static int unijoy_fops_next(struct unijoy_fops_client *, struct js_event *);

static struct file_operations unijoy_fops = {
  .owner          = THIS_MODULE,
  .open           = unijoy_fops_open,
  .release        = unijoy_fops_release,
  .read           = unijoy_fops_read,
  .unlocked_ioctl = unijoy_fops_ioctl,
  .fasync         = unijoy_fops_fasync,
  .llseek         = no_llseek
};

/* Input handlers */

enum unijoy_inph_source_state {
  UNIJOY_SOURCE_ONLINE,
  UNIJOY_SOURCE_MERGED,
  UNIJOY_SOURCE_WASMERGED
};

static char *unijoy_inph_source_state_name[] = {
  "ONLINE",
  "MERGED",
  "WASMERGED"
};

struct unijoy_inph_source {
  struct list_head list;
  long id;
  const char *name;
  int axis_no;
  int buttons_no;
  enum unijoy_inph_source_state state;
  struct input_handle handle;
  __u16 button_map[UNIJOY_MAX_BUTTONS];
  __u16 button_revmap[UNIJOY_MAX_BUTTONS];
  __u8 axis_map[ABS_CNT];
  __u8 axis_revmap[ABS_CNT];
  __s16 axis[ABS_CNT];
  struct js_corr corrections[ABS_CNT];
};

struct unijoy_inph_source_map {
  struct unijoy_inph_source *source;
  int mapping;
};

static struct {
  int axis_no;
  int buttons_no;
  __u8 axis_revmap[ABS_CNT];
  __u16 button_revmap[UNIJOY_MAX_BUTTONS];
  struct unijoy_inph_source_map axis_map[ABS_CNT];
  struct unijoy_inph_source_map button_map[UNIJOY_MAX_BUTTONS];
  struct list_head client_list;
  spinlock_t client_lock;
  wait_queue_head_t wait;
  struct mutex mutex;
  struct cdev cdev;
  struct device dev;
  int minor;
  struct JS_DATA_SAVE_TYPE glue;
} output;

static void unijoy_inph_event(struct input_handle *, unsigned int, 
                              unsigned int, int);
static bool unijoy_inph_match(struct input_handler *, struct input_dev *);
static int unijoy_inph_connect(struct input_handler *, struct input_dev *,
                               const struct input_device_id *);
static void unijoy_inph_disconnect(struct input_handle *);
static struct unijoy_inph_source *unijoy_inph_create(struct input_dev *);
static int unijoy_inph_correct(int, struct js_corr *);
static void unijoy_inph_distribute(struct js_event *);

static const struct input_device_id unijoy_inph_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { BIT_MASK(ABS_X) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { BIT_MASK(ABS_WHEEL) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { BIT_MASK(ABS_THROTTLE) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = {[BIT_WORD(BTN_JOYSTICK)] = BIT_MASK(BTN_JOYSTICK) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(BTN_GAMEPAD)] = BIT_MASK(BTN_GAMEPAD) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(BTN_TRIGGER_HAPPY)] = BIT_MASK(BTN_TRIGGER_HAPPY) },
	},
	{ }
};

MODULE_DEVICE_TABLE(input, unijoy_inph_ids);

static struct input_handler unijoy_inph = {
  .event         = unijoy_inph_event,
  .match         = unijoy_inph_match,
  .connect       = unijoy_inph_connect,
  .disconnect    = unijoy_inph_disconnect,
  .legacy_minors = true,
  .minor         = UNIJOY_MINOR_BASE,
  .name          = "unijoy",
  .id_table      = unijoy_inph_ids
};

/* Sysfs */

static ssize_t unijoy_sysfs_show(struct kobject *, struct attribute *, char *);
static ssize_t unijoy_sysfs_store(struct kobject *, struct attribute *,
                                  const char *, size_t);
static int unijoy_sysfs_setup(void);
static void unijoy_sysfs_free(void);
static struct unijoy_inph_source *unijoy_sysfs_find(long);
static void unijoy_sysfs_remove(struct unijoy_inph_source *);
static void unijoy_sysfs_suspend(struct unijoy_inph_source *);
static void unijoy_sysfs_merge(struct unijoy_inph_source *);
static void unijoy_sysfs_unmerge(struct unijoy_inph_source *);
static void unijoy_sysfs_add_button(struct unijoy_inph_source *, int, int);
static void unijoy_sysfs_del_button(int);
static void unijoy_sysfs_add_axis(struct unijoy_inph_source *, int, int);
static void unijoy_sysfs_del_axis(int);

struct unijoy_sysfs_attr_type {
  struct attribute attr;
  struct unijoy_inph_source sources;
  spinlock_t sources_lock;
};

static struct unijoy_sysfs_attr_type unijoy_sysfs = {
  .attr.name = "merger",
  .attr.mode = 0666
};

static struct sysfs_ops unijoy_sysfs_ops = {
  .show = unijoy_sysfs_show,
  .store = unijoy_sysfs_store
};

static struct attribute * unijoy_sysfs_attrs[] = {
  &unijoy_sysfs.attr,
  0
};

static struct kobj_type unijoy_sysfs_kobj_type = {
  .sysfs_ops = &unijoy_sysfs_ops,
  .default_attrs = unijoy_sysfs_attrs
};

static struct kobject *unijoy_sysfs_kobject;


/* Implementation */

/* Helpers */

static inline long unijoy_make_id(struct input_id id) {
  return ((long)id.bustype << 48)
       + ((long)id.vendor  << 32)
       + ((long)id.product << 16)
       + ((long)id.version      );
}

/* Sysfs */

static int unijoy_sysfs_setup(void) {
  unijoy_sysfs_kobject = kzalloc(sizeof(struct kobject), GFP_KERNEL);
  
  if (!unijoy_sysfs_kobject) 
    return -ENOMEM;

  kobject_init(unijoy_sysfs_kobject, &unijoy_sysfs_kobj_type);
  if (kobject_add(unijoy_sysfs_kobject, 0, "unijoy_ctl")) {
    kobject_put(unijoy_sysfs_kobject);
    kfree(unijoy_sysfs_kobject);
    return -EINVAL;
  }

  INIT_LIST_HEAD(&unijoy_sysfs.sources.list);
  spin_lock_init(&unijoy_sysfs.sources_lock);
  
  return 0;
}
static void unijoy_sysfs_free(void) {
  struct list_head *pos, *q;
  struct unijoy_inph_source *source;

  list_for_each_safe(pos, q, &unijoy_sysfs.sources.list) {
    source = list_entry(pos, struct unijoy_inph_source, list);

//    unijoy_sysfs_unmerge(source);

    list_del(pos);
    kfree(source);
  }

  kobject_put(unijoy_sysfs_kobject);
  kfree(unijoy_sysfs_kobject);
}

static ssize_t unijoy_sysfs_show(struct kobject *kobj, struct attribute *attr, 
                                 char *buf) {
  int i;
  int offset = 0;
  struct unijoy_inph_source *source;
  spin_lock(&unijoy_sysfs.sources_lock);
  list_for_each_entry(source, &unijoy_sysfs.sources.list, list) {
    offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                        "%ld\t%s\t%3d\t%3d\t%s\n",
                        source->id,
                        unijoy_inph_source_state_name[source->state],
                        source->axis_no, source->buttons_no,
                        source->name);
  }
  offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "Operating as /dev/input/js%d\nCurrent mappings:\n",
                      output.minor);
  
  for (i = 0; i < output.buttons_no; i++) {
    if (!output.button_map[i].source) 
      continue;
    offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "BTN #%3d -> %3d of %ld\n",
                      i, output.button_map[i].mapping,
                      output.button_map[i].source->id);
  }
  for (i = 0; i < output.axis_no; i++) {
    if (!output.axis_map[i].source) 
      continue;
    offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "AXS #%3d -> %3d of %ld\n",
                      i, output.axis_map[i].mapping,
                      output.axis_map[i].source->id);
  }
  spin_unlock(&unijoy_sysfs.sources_lock);
  return offset;
}
static ssize_t unijoy_sysfs_store(struct kobject *kobj, struct attribute *attr,
                                  const char *in_buf, size_t in_len) {
  struct unijoy_inph_source *source;
  char *buf = kzalloc(in_len+1, GFP_KERNEL);
  char *ptr = buf;
  int op = 0;
  int len = in_len;
  char *opword;
  int opwordlen;
  long id = 0;
  int arg1 = -1, arg2 = -1;

  memmove(buf, in_buf, in_len);

  while(*ptr && *ptr == ' ' && len) {
    ptr++;
    len--;
  }

#define opwordtest(word, outcome)  \
  opword = word; \
  opwordlen = strlen(opword); \
  if (op == 0 && len > opwordlen && strncmp(opword,ptr,opwordlen) == 0) { \
    op = outcome; \
    ptr += opwordlen; \
  }

  opwordtest("merge", 1);
  opwordtest("unmerge", 2);
  opwordtest("add_button", 3);
  opwordtest("del_button", 4);
  opwordtest("add_axis", 5);
  opwordtest("del_axis", 6);

  if (len == 0 || op == 0) {
    kfree(buf);
    return in_len;
  }

  switch (op) {
    case 1:
      sscanf(ptr, "%ld", &id);
      source = unijoy_sysfs_find(id);
      unijoy_sysfs_merge(source);
      break;
    case 2:
      sscanf(ptr, "%ld", &id);
      source = unijoy_sysfs_find(id);
      unijoy_sysfs_unmerge(source);
      break;
    case 3:
      sscanf(ptr, "%ld %d %d", &id, &arg1, &arg2);
      source = unijoy_sysfs_find(id);
      unijoy_sysfs_add_button(source, arg1, arg2);
      break;
    case 4:
      sscanf(ptr, "%d", &arg1);
      unijoy_sysfs_del_button(arg1);
      break;
    case 5:
      sscanf(ptr, "%ld %d %d", &id, &arg1, &arg2);
      source = unijoy_sysfs_find(id);
      unijoy_sysfs_add_axis(source, arg1, arg2);
      break;
    case 6:
      sscanf(ptr, "%d", &arg1);
      unijoy_sysfs_del_axis(arg1);
      break;
  }

  kfree(buf);
  return in_len;
}

static struct unijoy_inph_source *unijoy_sysfs_find(long id) {
  struct unijoy_inph_source *source, *result = 0;
  
  spin_lock(&unijoy_sysfs.sources_lock);

  list_for_each_entry(source, &unijoy_sysfs.sources.list, list) {
    if (source->id == id) {
      result = source;
      break;
    }
  }

  spin_unlock(&unijoy_sysfs.sources_lock);

  return result;
}

static void unijoy_sysfs_add_button(struct unijoy_inph_source *source,
                                    int src_no, int dst_no) {
  int i;

  if (!source) 
    return;

  if (source->state != UNIJOY_SOURCE_MERGED)
    return;

  if (src_no < 0 || src_no > source->buttons_no)
    return;

  if (dst_no < 0) {
    for (i = 0; i < output.buttons_no; i++) {
      if (!output.button_map[i].source) {
        dst_no = i;
        break;
      }
    }
    if (dst_no < 0 && output.buttons_no < UNIJOY_MAX_BUTTONS) {
      dst_no = output.buttons_no;
    }
  }

  if (dst_no >= output.buttons_no) {
    if (dst_no >= UNIJOY_MAX_BUTTONS)
      return;
    output.buttons_no = dst_no+1;
  }

  output.button_map[dst_no].source  = source;
  output.button_map[dst_no].mapping = src_no;
}

static void unijoy_sysfs_del_button(int dst_no) {
  int i;

  if (dst_no < 0 || dst_no >= output.buttons_no) 
    return;

  if (!output.button_map[dst_no].source)
    return;

  output.button_map[dst_no].source = 0;

  if (dst_no+1 == output.buttons_no) {
    i = dst_no;
    
    while (!output.button_map[i].source)
      i--;

    output.buttons_no = i+1;
  }
}

/* TODO: generalize with add_button */
static void unijoy_sysfs_add_axis(struct unijoy_inph_source *source,
                                  int src_no, int dst_no) {
  int i;

  if (!source) 
    return;
  
  if (source->state != UNIJOY_SOURCE_MERGED)
    return;

  if (src_no < 0 || src_no > source->axis_no)
    return;

  if (dst_no < 0) {
    for (i = 0; i < output.axis_no; i++) {
      if (!output.axis_map[i].source) {
        dst_no = i;
        break;
      }
    }
    if (dst_no < 0 && output.axis_no < ABS_CNT) {
      dst_no = output.axis_no;
    }
  }

  if (dst_no >= output.axis_no) {
    if (dst_no >= ABS_CNT)
      return;
    output.axis_no = dst_no+1;
  }

  output.axis_map[dst_no].source  = source;
  output.axis_map[dst_no].mapping = src_no;
}

/* TODO: generalize with del_button */
static void unijoy_sysfs_del_axis(int dst_no) {
  int i;

  if (dst_no < 0 || dst_no >= output.axis_no) 
    return;

  if (!output.axis_map[dst_no].source)
    return;

  output.axis_map[dst_no].source = 0;

  if (dst_no+1 == output.axis_no) {
    i = dst_no;
    
    while (!output.axis_map[i].source)
      i--;

    output.axis_no = i+1;
  }
}

static void unijoy_sysfs_merge(struct unijoy_inph_source *source) {
  if (!source)
    return;

  if (source->state != UNIJOY_SOURCE_ONLINE && 
      source->state != UNIJOY_SOURCE_WASMERGED)
    return;

  input_open_device(&source->handle);

  source->state = UNIJOY_SOURCE_MERGED;
}

static void unijoy_sysfs_unmerge(struct unijoy_inph_source *source) {
  if (!source) 
    return;

  if (source->state == UNIJOY_SOURCE_WASMERGED) {
    unijoy_sysfs_remove(source);
    return;
  }

  if (source->state != UNIJOY_SOURCE_MERGED)
    return;

  input_close_device(&source->handle);

  source->state = UNIJOY_SOURCE_ONLINE;
}

static void unijoy_sysfs_remove(struct unijoy_inph_source *source) {
  if (!source)
    return;
  
  list_del(&source->list);

  kfree(source);
}

static void unijoy_sysfs_suspend(struct unijoy_inph_source *source) {
  if (!source)
    return;

  input_close_device(&source->handle);

  source->state = UNIJOY_SOURCE_WASMERGED;
}

/* Input handlers */

static bool unijoy_inph_match(struct input_handler *handler, 
                              struct input_dev *dev) {
  if (test_bit(EV_KEY, dev->evbit) && test_bit(BTN_TOUCH, dev->keybit))
    return false;

  if (test_bit(EV_KEY, dev->evbit) && test_bit(BTN_DIGI, dev->keybit))
    return false;

  return true;
}

static int unijoy_inph_correct(int value, struct js_corr *corr) {
	switch (corr->type) {

		case JS_CORR_NONE:
			break;

		case JS_CORR_BROKEN:
			value = value > corr->coef[0] ? (value < corr->coef[1] ? 0 :
					((corr->coef[3] * (value - corr->coef[1])) >> 14)) :
				((corr->coef[2] * (value - corr->coef[0])) >> 14);
			break;

		default:
			return 0;
	}

	return value < -32767 ? -32767 : (value > 32767 ? 32767 : value);
}

static struct unijoy_inph_source *unijoy_inph_create(struct input_dev *dev) {
  long id = unijoy_make_id(dev->id);
  struct unijoy_inph_source *source = unijoy_sysfs_find(id);
  int i, j, t;

  if (source) {
    return 0;
  }

  source = kzalloc(sizeof(struct unijoy_inph_source), GFP_KERNEL);
  if (!source) {
    return 0;
  }

  source->id = id;
  source->name = dev->name;

  for (i = 0; i < ABS_CNT; i++) {
    if (test_bit(i, dev->absbit)) {
      source->axis_map[i] = source->axis_no;
      source->axis_revmap[source->axis_no] = i;
      source->axis_no++;
    }
  }

  for (i = BTN_JOYSTICK - BTN_MISC; i < UNIJOY_MAX_BUTTONS; i++) {
    if (test_bit(i + BTN_MISC, dev->keybit)) {
      source->button_map[i] = source->buttons_no;
      source->button_revmap[source->buttons_no] = i + BTN_MISC;
      source->buttons_no++;
    }
  }

  for (i = 0; i < BTN_JOYSTICK - BTN_MISC; i++) {
    if (test_bit(i + BTN_MISC, dev->keybit)) {
      source->button_map[i] = source->buttons_no;
      source->button_revmap[source->buttons_no] = i + BTN_MISC;
      source->buttons_no++;
    }
  }

  for (i = 0; i < source->axis_no; i++) {
    j = source->axis_revmap[i];
    if (input_abs_get_max(dev, j) == input_abs_get_min(dev, j)) {
      source->corrections[i].type = JS_CORR_NONE;
      source->axis[i] = input_abs_get_val(dev, j);
      continue;
    }

    source->corrections[i].type = JS_CORR_BROKEN;
    source->corrections[i].prec = input_abs_get_fuzz(dev, j);

    t = (input_abs_get_max(dev, j) + input_abs_get_min(dev, j)) / 2;
    source->corrections[i].coef[0] = t - input_abs_get_flat(dev, j);
    source->corrections[i].coef[1] = t + input_abs_get_flat(dev, j);
    t = (input_abs_get_max(dev, j) - input_abs_get_min(dev, j)) / 2
      - 2 * input_abs_get_flat(dev, j);
    if (t) {
      source->corrections[i].coef[2] = (1 << 29) / t;
      source->corrections[i].coef[3] = (1 << 29) / t;

      source->axis[i] = unijoy_inph_correct(input_abs_get_val(dev, j), 
          source->corrections + i);
    }
  }

  spin_lock(&unijoy_sysfs.sources_lock);
  INIT_LIST_HEAD(&source->list);
  list_add(&source->list, &unijoy_sysfs.sources.list);
  spin_unlock(&unijoy_sysfs.sources_lock);

  return source;
}

static void unijoy_inph_distribute(struct js_event *event) {
  struct unijoy_fops_client *client;
  rcu_read_lock();
  list_for_each_entry_rcu(client, &output.client_list, list) {
    spin_lock(&client->buffer_lock);
    client->buffer[client->head] = *event;
    if (client->startup == output.axis_no + output.buttons_no) {
      client->head++;
      client->head &= UNIJOY_BUFFER_SIZE - 1;
      if (client->tail == client->head) {
        client->startup = 0;
      }
    }
    spin_unlock(&client->buffer_lock);
    kill_fasync(&client->fasync, SIGIO, POLL_IN);
  }
  rcu_read_unlock();
}

static int unijoy_inph_connect(struct input_handler *handler, 
                               struct input_dev *dev,
                               const struct input_device_id *in_id) {
  struct unijoy_inph_source *source;
  int error;
  long id;

  id = unijoy_make_id(dev->id);
  source = unijoy_sysfs_find(id);

  if (!source) {
    source = unijoy_inph_create(dev);
    if (!source) 
      return -EINVAL;
  } 

  source->handle.dev = input_get_device(dev);
  source->handle.name = "jsN";
  source->handle.handler = handler;
  source->handle.private = source;

  error = input_register_handle(&source->handle);

  if (error)
    return error;

  if (source->state == UNIJOY_SOURCE_WASMERGED) {
    unijoy_sysfs_merge(source);
  }

  return 0;
}

static void unijoy_inph_disconnect(struct input_handle *handle) {
  struct unijoy_inph_source *source = handle->private;

  if (source->state == UNIJOY_SOURCE_MERGED) {
    unijoy_sysfs_suspend(source);
  } else {
    unijoy_sysfs_remove(source);
  }

  input_unregister_handle(handle);
}

static void unijoy_inph_event(struct input_handle *handle,
                              unsigned int type,
                              unsigned int code,
                              int value) {
  struct unijoy_inph_source *source = handle->private;
  struct js_event source_event;
  struct js_event passing_event;
  int i;

  switch (type) {
    case EV_KEY:
      if (code < BTN_MISC || value == 2) 
        return;

      source_event.type = JS_EVENT_BUTTON;
      source_event.number = source->button_map[code - BTN_MISC];
      source_event.value = value;
      break;
    case EV_ABS:
      source_event.type = JS_EVENT_AXIS;
      source_event.number = source->axis_map[code];
      source_event.value = 
        unijoy_inph_correct(value, &source->corrections[source_event.number]);
      
      if (source_event.value == source->axis[source_event.number]) 
        return;

      source->axis[source_event.number] = source_event.value;
      break;
    default:
      return;
  }

  source_event.time = jiffies_to_msecs(jiffies);
  passing_event = source_event;

  switch (source_event.type) {
    case JS_EVENT_BUTTON:
      for (i = 0; i < output.buttons_no; i++) {
        if (output.button_map[i].source == source &&
            output.button_map[i].mapping == source_event.number) {
          passing_event.number = i;
          unijoy_inph_distribute(&passing_event);
        }
      }
      break;
    case JS_EVENT_AXIS:
      for (i = 0; i < output.axis_no; i++) {
        if (output.axis_map[i].source == source &&
            output.axis_map[i].mapping == source_event.number) {
          passing_event.number = i;
          unijoy_inph_distribute(&passing_event);
        }
      }
      break;
  }

  wake_up_interruptible(&output.wait);
}

/* File I/O */
static void unijoy_fops_attach(struct unijoy_fops_client *client) {
	spin_lock(&output.client_lock);
	list_add_tail_rcu(&client->list, &output.client_list);
	spin_unlock(&output.client_lock);
}

static void unijoy_fops_detach(struct unijoy_fops_client *client) {
	spin_lock(&output.client_lock);
	list_del_rcu(&client->list);
	spin_unlock(&output.client_lock);
	synchronize_rcu();
}

static int unijoy_fops_open(struct inode *inode, struct file *file) {
  struct unijoy_fops_client *client;

  client = kzalloc(sizeof(struct unijoy_fops_client), GFP_KERNEL);
  if (!client)
    return -ENOMEM;

  spin_lock_init(&client->buffer_lock);
  unijoy_fops_attach(client);

  file->private_data = client;
  nonseekable_open(inode, file);

  return 0;
}

static int unijoy_fops_release(struct inode *inode, struct file *file) {
	struct unijoy_fops_client *client = file->private_data;
	unijoy_fops_detach(client);
	kfree(client);
	return 0;
}

static int unijoy_fops_fasync(int fd, struct file *file, int on) {
	struct unijoy_fops_client *client = file->private_data;

	return fasync_helper(fd, file, on, &client->fasync);
}


static long unijoy_fops_ioctl(struct file *file, 
                              unsigned int cmd, unsigned long arg) {
  void __user *argp = (void __user *)arg;
  int retval;

  retval = mutex_lock_interruptible(&output.mutex);
  
  if (retval)
    return retval;

  switch (cmd) {
    case JS_SET_TIMELIMIT:
      retval = get_user(output.glue.JS_TIMELIMIT, (long __user *)arg);
      break;
    case JS_GET_TIMELIMIT:
      retval = put_user(output.glue.JS_TIMELIMIT, (long __user *)arg);
      break;
    case JS_SET_ALL:
      retval = copy_from_user(&output.glue, argp, sizeof(output.glue)) 
        ? -EFAULT : 0;
      break;
    case JS_GET_ALL:
      retval = copy_to_user(argp, &output.glue, sizeof(output.glue))
        ? -EFAULT : 0;
      break;
    default:
      retval = unijoy_fops_ioctl_common(cmd, argp);
      break;
  }

  mutex_unlock(&output.mutex);
  return retval;
}

static long unijoy_fops_ioctl_common(unsigned int cmd, void __user *argp) {
  size_t len;
  const char *name;

  switch (cmd) {
    case JS_SET_CAL:
      return copy_from_user(&output.glue.JS_CORR, argp, 
                            sizeof(output.glue.JS_CORR)) ? -EFAULT : 0;
    case JS_GET_CAL:
      return copy_to_user(argp, &output.glue.JS_CORR,
                          sizeof(output.glue.JS_CORR)) ? -EFAULT : 0;

    case JS_SET_TIMEOUT:
      return get_user(output.glue.JS_TIMEOUT, (s32 __user *)argp);

    case JS_GET_TIMEOUT:
      return put_user(output.glue.JS_TIMEOUT, (s32 __user *)argp);

    case JSIOCGVERSION:
      return put_user(JS_VERSION, (__u32 __user *)argp);

    case JSIOCGAXES:
      return put_user(output.axis_no, (__u8 __user *)argp);

    case JSIOCGBUTTONS:
      return put_user(output.buttons_no, (__u8 __user *)argp);
    
    /* TODO: implement */
    case JSIOCSCORR:
    case JSIOCGCORR:
      return -EINVAL;
  }

  switch (cmd & ~IOCSIZE_MASK) {
    case (JSIOCGAXMAP & ~IOCSIZE_MASK):
      len = min_t(size_t, _IOC_SIZE(cmd), sizeof(output.axis_revmap));
      return copy_to_user(argp, output.axis_revmap, len) ? -EFAULT : len;

    case (JSIOCGBTNMAP & ~IOCSIZE_MASK):
      len = min_t(size_t, _IOC_SIZE(cmd), sizeof(output.button_revmap));
      return copy_to_user(argp, output.button_revmap, len) ? -EFAULT : len;

    /* TODO: implement */
    case (JSIOCSAXMAP & ~IOCSIZE_MASK):
    case (JSIOCSBTNMAP & ~IOCSIZE_MASK):
      return -EINVAL;

    case JSIOCGNAME(0):
      name = "unijoy v0.2";
      len = min_t(size_t, _IOC_SIZE(cmd), strlen(name) + 1);
      return copy_to_user(argp, name, len) ? -EFAULT : len;
  }

  return -EINVAL;
}

static inline int unijoy_fops_pending(struct unijoy_fops_client *client) {
  return client->startup < (output.axis_no + output.buttons_no) ||
         client->head != client->tail;
}

static int unijoy_fops_startup(struct unijoy_fops_client *client,
                               struct js_event *event) {
  int have_event;
  struct unijoy_inph_source_map map;
  struct input_dev *input;
  int button;


  spin_lock_irq(&client->buffer_lock);
  have_event = client->startup < (output.axis_no + output.buttons_no);

  if (have_event) {
    
    event->time = jiffies_to_msecs(jiffies);
    if (client->startup < output.buttons_no) {
      event->type = JS_EVENT_BUTTON | JS_EVENT_INIT;
      event->number = client->startup;
      map = output.button_map[event->number];
      if (map.source) {
        event->value = 0;
      } else {
        button = map.source->button_revmap[map.mapping];
        input = map.source->handle.dev;
        event->value = !!test_bit(button, input->key);
      }
    } else {
      event->type = JS_EVENT_AXIS | JS_EVENT_INIT;
      event->number = client->startup - output.buttons_no;
      map = output.axis_map[event->number];
      if (!map.source) {
        event->value = 0;
      } else {
        event->value = map.source->axis[map.mapping];
      }
    }

    client->startup++;
  }

  spin_unlock_irq(&client->buffer_lock);
  return have_event;
}

static int unijoy_fops_next(struct unijoy_fops_client *client,
                            struct js_event *event) {
  int have_event;

  spin_lock_irq(&client->buffer_lock);

  have_event = client->head != client->tail;
  
  if (have_event) {
    *event = client->buffer[client->tail++];
    client->tail &= UNIJOY_BUFFER_SIZE - 1;
  }

  spin_unlock_irq(&client->buffer_lock);

  return have_event;
}

static ssize_t unijoy_fops_read(struct file *file, char __user *buf, 
                                size_t count, loff_t *ppos) {

  struct unijoy_fops_client *client = file->private_data;
  struct js_event event;
  int retval;

  if (count < sizeof(struct js_event))
    return -EINVAL;

  if (count == sizeof(struct JS_DATA_TYPE)) {
    // TODO: implement old interface
    return -EINVAL;
  }

  if (!unijoy_fops_pending(client) && (file->f_flags & O_NONBLOCK))
    return -EAGAIN;

  retval = wait_event_interruptible(output.wait, unijoy_fops_pending(client));
  if (retval)
    return retval;

  while (retval + sizeof(struct js_event) <= count &&
         unijoy_fops_startup(client, &event)) {
    if (copy_to_user(buf+retval, &event, sizeof(struct js_event))) {
      return -EFAULT;
    }
    retval += sizeof(struct js_event);
  }

  while (retval + sizeof(struct js_event) <= count &&
         unijoy_fops_next(client, &event)) {
    if (copy_to_user(buf+retval, &event, sizeof(struct js_event))) {
      return -EFAULT;
    }
    retval += sizeof(struct js_event);
  }
  
  return retval;
}

/* Main entry points */

int __init unijoy_init(void) {
  int i;
  int error;
  
  output.minor = input_get_new_minor(UNIJOY_MINOR_BASE, UNIJOY_MINORS, true);
  
  if (output.minor < 0)
    return -EINVAL;
  
  output.dev.devt = MKDEV(INPUT_MAJOR, output.minor);
  output.dev.class = &input_class;
  device_initialize(&output.dev);
  
  if (device_create(&input_class, 0, output.dev.devt, 0, "js%d", output.minor) == 0) {
    error = -EINVAL;
    goto err_input_free_minor;
  }

  cdev_init(&output.cdev, &unijoy_fops);
  output.cdev.kobj.parent = &output.dev.kobj;
  error = cdev_add(&output.cdev, output.dev.devt, 1);
  
  if (error)
    goto err_cdev_destroy;

  error = unijoy_sysfs_setup();

  if (error)
    goto err_cdev_del;

  INIT_LIST_HEAD(&output.client_list);
  spin_lock_init(&output.client_lock);
  mutex_init(&output.mutex);
  init_waitqueue_head(&output.wait);

  for (i = 0; i < ABS_CNT; i++)
    output.axis_revmap[i] = i;

  for (i = 0; i < UNIJOY_MAX_BUTTONS; i++)
    output.button_revmap[i] = BTN_MISC + i;

  error = input_register_handler(&unijoy_inph);

  if (error)
    goto err_free_sysfs;   

  return 0;

err_free_sysfs:
  unijoy_sysfs_free();
err_cdev_del:
  cdev_del(&output.cdev);
err_cdev_destroy:
  device_destroy(&input_class, output.dev.devt);
err_input_free_minor:
  input_free_minor(output.minor);

  return error;
}

void __exit unijoy_exit(void) {
  input_unregister_handler(&unijoy_inph);
  unijoy_sysfs_free();
  cdev_del(&output.cdev);
  device_destroy(&input_class, output.dev.devt);
  input_free_minor(output.minor);
}

module_init(unijoy_init);
module_exit(unijoy_exit);


