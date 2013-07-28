#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <linux/list.h>
#include <linux/slab.h>

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

/* Thread function */

static int unijoy_thread(void *);

/* Input handlers */

enum unijoy_inph_source_state {
  UNIJOY_SOURCE_ONLINE,
  UNIJOY_SOURCE_MERGED,
  UNIJOY_SOURCE_WASMERGED
};

static char *unijoy_inph_source_state_name[] = {
  "   ONLINE",
  "   MERGED",
  "WASMERGED"
};

static char *unijoy_inph_source_link_state_name[] = {
  " ONLINE",
  "OFFLINE"
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
  long id;
};

static struct {
  int axis_no;
  int buttons_no;
  struct unijoy_inph_source_map axis_map[ABS_CNT];
  struct unijoy_inph_source_map button_map[UNIJOY_MAX_BUTTONS];
  struct input_dev *idev;
  wait_queue_head_t wait;
  struct task_struct *kthread;
  spinlock_t buffer_lock;
  long buffer[UNIJOY_BUFFER_SIZE];
  int head;
  int tail;
  bool full;
} output;


static void unijoy_inph_event(struct input_handle *, unsigned int, 
                              unsigned int, int);
static bool unijoy_inph_match(struct input_handler *, struct input_dev *);
static int unijoy_inph_connect(struct input_handler *, struct input_dev *,
                               const struct input_device_id *);
static void unijoy_inph_disconnect(struct input_handle *);
static struct unijoy_inph_source *unijoy_inph_create(struct input_dev *);
static int unijoy_inph_correct(int, struct js_corr *);
static void unijoy_inph_refresh(void);
static void unijoy_inph_relink(struct unijoy_inph_source *, long);
static void unijoy_inph_enqueue(unsigned long);


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
static void unijoy_sysfs_clean(struct unijoy_inph_source *, bool);

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
                      99);
  
  for (i = 0; i < output.buttons_no; i++) {
    if (!output.button_map[i].source) 
      continue;
    offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "BTN #%3d -> %3d of %ld %s\n",
                      i, output.button_map[i].mapping,
                      output.button_map[i].id,
                      unijoy_inph_source_link_state_name[
                        (output.button_map[i].source ? 0 : 1)
                      ]);
  }
  for (i = 0; i < output.axis_no; i++) {
    if (!output.axis_map[i].source) 
      continue;
    offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "AXS #%3d -> %3d of %ld %s\n",
                      i, output.axis_map[i].mapping,
                      output.axis_map[i].id,
                      unijoy_inph_source_link_state_name[
                        (output.axis_map[i].source ? 0 : 1)
                      ]);
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
  long id = -1;
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
  output.button_map[dst_no].id      = source->id;

  unijoy_inph_refresh();
}

static void unijoy_sysfs_del_button(int dst_no) {
  int i;

  if (dst_no < 0 || dst_no >= output.buttons_no) 
    return;

  if (!output.button_map[dst_no].source)
    return;

  output.button_map[dst_no].source = 0;
  output.button_map[dst_no].id     = 0;

  if (dst_no+1 == output.buttons_no) {
    i = dst_no;
    
    while (!output.button_map[i].source)
      i--;

    output.buttons_no = i+1;
  }
  unijoy_inph_refresh();
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
  output.axis_map[dst_no].id      = source->id;
  unijoy_inph_refresh();
}

/* TODO: generalize with del_button */
static void unijoy_sysfs_del_axis(int dst_no) {
  int i;

  if (dst_no < 0 || dst_no >= output.axis_no) 
    return;

  if (!output.axis_map[dst_no].source)
    return;

  output.axis_map[dst_no].source = 0;
  output.axis_map[dst_no].id     = 0;

  if (dst_no+1 == output.axis_no) {
    i = dst_no;
    
    while (!output.axis_map[i].source)
      i--;

    output.axis_no = i+1;
  }
  unijoy_inph_refresh();
}

static void unijoy_sysfs_merge(struct unijoy_inph_source *source) {
  if (!source)
    return;

  if (source->state != UNIJOY_SOURCE_ONLINE && 
      source->state != UNIJOY_SOURCE_WASMERGED)
    return;

  input_open_device(&source->handle);

  source->state = UNIJOY_SOURCE_MERGED;
  unijoy_inph_refresh();
}

static void unijoy_sysfs_clean(struct unijoy_inph_source *source,
                               bool force) {
  int i;

  if (!source)
    return;

  for (i = 0; i < output.axis_no; i++) {
    if (output.axis_map[i].source == source) {
      output.axis_map[i].source = 0;
      if (force) {
        output.axis_map[i].id = 0;
      }
    }
  }
  i--;
  while (!output.axis_map[i].id)
    i--;
  output.axis_no = i+1;

  for (i = 0; i < output.buttons_no; i++) {
    if (output.button_map[i].source == source) {
      output.button_map[i].source = 0;
      if (force) {
        output.button_map[i].id = 0;
      }
    }
  }
  i--;
  while (!output.button_map[i].id)
    i--;
  output.buttons_no = i+1;
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
  
  unijoy_sysfs_clean(source, true);
  unijoy_inph_refresh();
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

static void unijoy_inph_relink(struct unijoy_inph_source *source, long id) {
  int i;
  for (i = 0; i < output.buttons_no; i++) {
    if (output.button_map[i].id == id) {
      output.button_map[i].source = source;
    }
  }
  for (i = 0; i < output.axis_no; i++) {
    if (output.axis_map[i].id == id) {
      output.axis_map[i].source = source;
    }
  }
}

static int unijoy_inph_connect(struct input_handler *handler, 
                               struct input_dev *dev,
                               const struct input_device_id *in_id) {
  struct unijoy_inph_source *source;
  int error;
  long id;
  
  id = unijoy_make_id(dev->id);

  if (id == 0) {
    return 0;
  }

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
    unijoy_inph_relink(source,id);
  }

  return 0;
}

static void unijoy_inph_disconnect(struct input_handle *handle) {
  struct unijoy_inph_source *source = handle->private;

  if (!source) 
    return;

  if (source->state == UNIJOY_SOURCE_MERGED) {
    unijoy_sysfs_suspend(source);
  } else {
    unijoy_sysfs_remove(source);
  }

  input_unregister_handle(handle);

  unijoy_sysfs_clean(source, false);
}

static int unijoy_thread(void *nulldata) {
  unsigned long data = 0;
  int value;
  int number;
  int type;

  while (1) {
    wait_event_interruptible(output.wait, 
        ((output.head != output.tail) || kthread_should_stop()));
    if (kthread_should_stop()) 
      return 0;
    while (output.head != output.tail) {
      if (kthread_should_stop()) 
        return 0;
      spin_lock_irq(&output.buffer_lock);
      if (output.head != output.tail && !output.full) {
        data = output.buffer[output.tail++];
        output.tail &= UNIJOY_BUFFER_SIZE - 1;
        if (output.full) 
          output.full = false;
      }
      spin_unlock_irq(&output.buffer_lock);
      value = (int)(data & 0xFFFFFFFF); 
      number = (int)((data>>32) & 0xFFFF);
      type   = (int)((data>>48) & 0xFFFF);

      switch (type) {
        case 0:
          input_report_key(output.idev, number, value);
          break;
        case 1:
          input_report_abs(output.idev, number, value);
          break;
      }
      input_sync(output.idev);
    }
  }
  return 0;
}

static void unijoy_inph_enqueue(unsigned long data) {
  spin_lock(&output.buffer_lock);
  if (output.full)
    goto unlock_exit;

  output.buffer[output.head] = data;

  output.head++;
  output.head &= UNIJOY_BUFFER_SIZE - 1;

  if (output.head == output.tail)
    output.full = true;

unlock_exit:
  spin_unlock(&output.buffer_lock);
  wake_up_interruptible(&output.wait);
}

static void unijoy_inph_event(struct input_handle *handle,
                              unsigned int type,
                              unsigned int code,
                              int value) {
  struct unijoy_inph_source *source = handle->private;
  unsigned int number;
  unsigned long data;
  int i;

  if (!source)
    return;
  
  switch (type) {
    case EV_KEY:
      if (code < BTN_MISC || value == 2) {
        return;
      }
      number = source->button_map[code - BTN_MISC];
      for (i = 0; i < output.buttons_no; i++) {
        if (output.button_map[i].source == source &&
            output.button_map[i].mapping == number) {
          data = ((long)0<<48) 
               + ((long)(i+BTN_JOYSTICK)<<32)
               + ((unsigned int)value);
          unijoy_inph_enqueue(data);
        }
      }
      break;
    case EV_ABS:
      number = source->axis_map[code];
      value = unijoy_inph_correct(value, &source->corrections[number]);
      for (i = 0; i < output.axis_no; i++) {
        if (output.axis_map[i].source == source &&
            output.axis_map[i].mapping == number) {
          data = ((long)1<<48) 
               + ((long)i<<32)
               + ((unsigned int)value);
          unijoy_inph_enqueue(data);
        }
      }

      break;
    default:
      return;
  }
}

static void unijoy_inph_refresh(void) {
  int i;
  struct input_dev *idev = 0;

  if (output.axis_no < 0 && output.buttons_no < 0)
    return;
  
  idev = input_allocate_device();
  idev->name = "unijoy v0.3";
  input_alloc_absinfo(idev);


  if (output.buttons_no > 0) {
    set_bit(EV_KEY, idev->evbit);
    for (i = 0; i < output.buttons_no; i++) {
      if (i+BTN_JOYSTICK < KEY_MAX) {
        set_bit(i+BTN_JOYSTICK, idev->keybit);
      } else {
        set_bit((i+BTN_JOYSTICK) - KEY_MAX + BTN_MISC, idev->keybit);
      }
    }
  }

  if (output.axis_no > 0) {
    set_bit(EV_ABS, idev->evbit);
    for (i = 0; i < output.axis_no; i++) {
      set_bit(i, idev->absbit);
    }
  }

  if (output.idev) {
    input_unregister_device(output.idev);
    input_free_device(output.idev);
    output.idev = 0;
  }

  if (input_register_device(idev)) {
    input_free_device(idev);
  } else {
    output.idev = idev;
  }

}

/* Main entry points */

int __init unijoy_init(void) {
  int error;

  error = unijoy_sysfs_setup();

  if (error)
    return error;

  error = input_register_handler(&unijoy_inph);

  if (error)
    goto err_free_sysfs; 

  spin_lock_init(&output.buffer_lock);
  init_waitqueue_head(&output.wait);
  output.kthread = kthread_create(unijoy_thread,0,"unijoy_thread");

  wake_up_process(output.kthread);  

  return 0;
  
err_free_sysfs:
  unijoy_sysfs_free();
  
  return error;
}

void __exit unijoy_exit(void) {
  kthread_stop(output.kthread);
  
  if (output.idev) {
    input_unregister_device(output.idev);
    input_free_device(output.idev);
    output.idev = 0;
  }

  input_unregister_handler(&unijoy_inph);
  unijoy_sysfs_free();
}

module_init(unijoy_init);
module_exit(unijoy_exit);


