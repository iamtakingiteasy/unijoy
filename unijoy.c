/**
 * This module is providing virtual joystick device interface composed
 * of N real devices.
 *
 * It uses /sys/unijoy_ctl/merger file interface to control itself.
 *
 * The main design flaw is that it uses kthread to escape deadlocks from
 * unregistering and re-registering devices upon connecting and disconnecting
 * of real deices and reporting events upon receiving events from real 
 * devices.
 *
 * You can used following commands (simply echoed strings to control file as
 * in "echo merge ID > /sys/unijoy_ctl/merger").
 *
 * echo merge ID
 *     adds a specified ID to merge (union of devices)
 *
 * echo unmerge ID
 *     removes a specified ID from merge
 *  
 * add_button ID SOURCE_BUTTON_NO [DEST_BUTTON_NO]
 *     adds a button from source real device under dest button # number
 *     dest button no argument is optional, first free would be used 
 *     if not speciied.
 *
 * del_button DEST_BUTTON_NO
 *     removes a button with specified number from virtual device
 *
 * add_axis ID SOURCE_AXIS_NO [DEST_AXIS_NO]
 *     likewise add_button, only for axis
 *
 * del_axis DEST_AXIS_NO
 *     likewise del_button, only for axis
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/ctype.h>

#define UNIJOY_MINOR_BASE 0
#define UNIJOY_MINORS 16
#define UNIJOY_BUFFER_SIZE 128
#define UNIJOY_MAX_BUTTONS (KEY_MAX - BTN_MISC + 1)

/* Threads */

static int unijoy_thread(void *);
static __u64 unijoy_thread_nextdata(void);
static int unijoy_thread_wakeup_condition(void);

enum unijoy_thread_action {
  UNIJOY_ACTION_EMMIT_BUTTON,
  UNIJOY_ACTION_EMMIT_AXIS,
  UNIJOY_ACTION_REFRESH
};

/* Input handlers */

enum unijoy_inph_state {
  UNIJOY_SOURCE_ONLINE,
  UNIJOY_SOURCE_MERGED,
  UNIJOY_SOURCE_DISCONNECTED
};

static char *unijoy_inph_state_names[] = {
  "      ONLINE",
  "      MERGED",
  "DISCONNECTED"
};

static char *unijoy_inph_mapping_names[] = {
  " ONLINE",
  "OFFLINE"
};

struct unijoy_inph_source {
  struct list_head list;
  __u64 id;
  const char *name;
  int axis_total;
  int buttons_total;
  enum unijoy_inph_state state;
  struct input_handle handle;
  __u16 button_map[UNIJOY_MAX_BUTTONS];
  __u16 button_revmap[UNIJOY_MAX_BUTTONS];
  __u8 axis_map[ABS_CNT];
  __u8 axis_revmap[ABS_CNT];
  struct js_corr corrections[ABS_CNT];
};

struct unijoy_inph_map {
  struct unijoy_inph_source *source;
  int value;
  __u64 id;
};

static struct {
  int axis_total;
  int buttons_total;
  struct unijoy_inph_map source_axis_map[ABS_CNT];
  struct unijoy_inph_map source_buttons_map[UNIJOY_MAX_BUTTONS];
  struct input_dev *idev;
  wait_queue_head_t wait;
  struct task_struct *thread;
  spinlock_t buffer_lock;
  __u64 buffer[UNIJOY_BUFFER_SIZE];
  int head;
  int tail;
  bool full;
} output;

static void unijoy_inph_event(struct input_handle *, 
                              unsigned int,
                              unsigned int,
                              int);

static bool unijoy_inph_match(struct input_handler *,
                              struct input_dev *);

static int unijoy_inph_connect(struct input_handler *,
                               struct input_dev *,
                               const struct input_device_id *);

static struct unijoy_inph_source *unijoy_inph_create(struct input_dev *, 
                                                     __u64);

static void unijoy_inph_disconnect(struct input_handle *);
static int unijoy_inph_correct(int, struct js_corr *);
static void unijoy_inph_unregister(void);
static void unijoy_inph_register(void);
static void unijoy_inph_refresh(void);
static void unijoy_inph_relink(struct unijoy_inph_source *, __u64);
static void unijoy_inph_enqueue(__u64);

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

/* Sysfs operations */

static int unijoy_sysfs_setup(void);
static void unijoy_sysfs_free(void);
static struct unijoy_inph_source *unijoy_sysfs_find(__u64);
static void unijoy_sysfs_remove(struct unijoy_inph_source *);
static void unijoy_sysfs_suspend(struct unijoy_inph_source *);
static void unijoy_sysfs_merge(struct unijoy_inph_source *);
static void unijoy_sysfs_unmerge(struct unijoy_inph_source *);
static void unijoy_sysfs_add_button(struct unijoy_inph_source *, int, int);
static void unijoy_sysfs_del_button(int);
static void unijoy_sysfs_add_axis(struct unijoy_inph_source *, int, int);
static void unijoy_sysfs_del_axis(int);
static void unijoy_sysfs_clean(struct unijoy_inph_source *, bool);
static ssize_t unijoy_sysfs_show(struct kobject *, struct attribute *, char *);
static ssize_t unijoy_sysfs_store(struct kobject *, struct attribute *,
                                  const char *, size_t);

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

/* Sysfs Implementation */

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
                        "%llu\t%s\t%3d\t%3d\t%s\n",
                        source->id,
                        unijoy_inph_state_names[source->state],
                        source->axis_total, source->buttons_total,
                        source->name);
  }
  offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "Current mappings:\n");
  
  for (i = 0; i < output.buttons_total; i++) {
    if (output.source_buttons_map[i].id == ULLONG_MAX)
      continue;
    offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "BTN #%3d -> %3d of %llu %s\n",
                      output.source_buttons_map[i].value, i,
                      output.source_buttons_map[i].id,
                      unijoy_inph_mapping_names[
                        (output.source_buttons_map[i].source ? 0 : 1)
                      ]);
  }
  for (i = 0; i < output.axis_total; i++) {
    if (output.source_buttons_map[i].id == ULLONG_MAX)
      continue;
    offset += scnprintf(buf+offset, PAGE_SIZE-offset,
                      "AXS #%3d -> %3d of %llu %s\n",
                      output.source_axis_map[i].value, i,
                      output.source_axis_map[i].id,
                      unijoy_inph_mapping_names[
                        (output.source_axis_map[i].source ? 0 : 1)
                      ]);
  }
  spin_unlock(&unijoy_sysfs.sources_lock);
  return offset;
}

static ssize_t unijoy_sysfs_store(struct kobject *kobj, struct attribute *attr,
                                  const char *in_buf, size_t in_len) {
  struct unijoy_inph_source *source;
  int error = 0;
  char *buf = kzalloc(in_len+1, GFP_KERNEL);
  char *ptr = buf;
  char *rptr;
  int op = 0;
  int len = in_len;
  __u64 id = ULLONG_MAX;
  int arg1 = -1, arg2 = -1;

  if (!buf) 
    return in_len;
  
  memmove(buf, in_buf, in_len);

  for (; 
       *ptr && *ptr == ' ' && len; 
       ptr++, len--);

#define OPWORDTEST(word, outcome)  \
  do { \
    char *opword; \
    int opwordlen; \
    opword = word; \
    opwordlen = strlen(opword); \
    if (op == 0 && len > opwordlen && strncmp(opword,ptr,opwordlen) == 0) { \
      op = outcome; \
      ptr += opwordlen; \
      len -= opwordlen; \
    } \
  } while(0)

  OPWORDTEST("merge", 1);
  OPWORDTEST("unmerge", 2);
  OPWORDTEST("add_button", 3);
  OPWORDTEST("del_button", 4);
  OPWORDTEST("add_axis", 5);
  OPWORDTEST("del_axis", 6);

  if (len == 0 || op == 0) error = 1;

  if (!error) {
    for (rptr = ptr; 
         *rptr && (isspace(*rptr) || isdigit(*rptr)) && len; 
         rptr++, len--);
  
    error = rptr != (buf+in_len);
  }

  if (error) {
    kfree(buf);
    return in_len;
  }

  switch (op) {
    case 1:
      sscanf(ptr, "%llu", &id);
      source = unijoy_sysfs_find(id);
      unijoy_sysfs_merge(source);
      break;
    case 2:
      sscanf(ptr, "%llu", &id);
      source = unijoy_sysfs_find(id);
      unijoy_sysfs_unmerge(source);
      break;
    case 3:
      sscanf(ptr, "%llu %d %d", &id, &arg1, &arg2);
      source = unijoy_sysfs_find(id);
      unijoy_sysfs_add_button(source, arg1, arg2);
      break;
    case 4:
      sscanf(ptr, "%d", &arg1);
      unijoy_sysfs_del_button(arg1);
      break;
    case 5:
      sscanf(ptr, "%llu %d %d", &id, &arg1, &arg2);
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

static struct unijoy_inph_source *unijoy_sysfs_find(__u64 id) {
  struct unijoy_inph_source *source, *result = 0;
  
  if (id == ULLONG_MAX)
    return 0;

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

#define UNIJOY_ADD_RESOURCE(single, name, MAX_VALUE) \
  static void unijoy_sysfs_add_ ## single (struct unijoy_inph_source *source, \
                                           int src_no, int dst_no) { \
    int i; \
    if (!source) \
      return; \
    if (source->state != UNIJOY_SOURCE_MERGED) \
      return; \
    if (src_no < 0 || src_no > source-> name ## _total) \
      return; \
    if (dst_no < 0) { \
      for (i = 0; i < output. name ## _total ; i++) { \
        if (output.source_ ## name ## _map[i].id == ULLONG_MAX) { \
          dst_no = i; \
          break; \
        } \
      } \
      if (dst_no < 0 && output. name ## _total < MAX_VALUE) { \
        dst_no = output. name ## _total; \
      } \
    } \
    if (dst_no >= output. name ## _total) { \
      if (dst_no >= MAX_VALUE) \
        return; \
      output. name ## _total = dst_no + 1; \
    } \
    output.source_ ## name ## _map[dst_no].source = source; \
    output.source_ ## name ## _map[dst_no].value  = src_no; \
    output.source_ ## name ## _map[dst_no].id     = source->id; \
    unijoy_inph_refresh(); \
  }

UNIJOY_ADD_RESOURCE(button, buttons, UNIJOY_MAX_BUTTONS);
UNIJOY_ADD_RESOURCE(axis, axis, ABS_CNT);

#define UNIJOY_DEL_RESOURCE(single, name) \
  static void unijoy_sysfs_del_ ## single (int dst_no) { \
    int i; \
    if (dst_no < 0 || dst_no >= output. name ## _total) \
      return; \
    if (output.source_ ## name ##_map[dst_no].id == ULLONG_MAX) \
      return; \
    output.source_ ## name ## _map[dst_no].source = 0; \
    output.source_ ## name ## _map[dst_no].id     = ULLONG_MAX; \
    if (dst_no+1 == output. name ## _total) { \
      i = dst_no; \
      while (output.source_ ## name ## _map[i].id == ULLONG_MAX) \
        i--; \
      output. name ## _total = i+1; \
    } \
    unijoy_inph_refresh(); \
  }

UNIJOY_DEL_RESOURCE(button, buttons);
UNIJOY_DEL_RESOURCE(axis, axis);

static void unijoy_sysfs_merge(struct unijoy_inph_source *source) {
  if (!source)
    return;

  if (source->state == UNIJOY_SOURCE_MERGED)
    return;

  input_open_device(&source->handle);

  source->state = UNIJOY_SOURCE_MERGED;
  unijoy_inph_refresh();
}

static void unijoy_sysfs_clean(struct unijoy_inph_source *source,
                               bool forever) {
  int i;
  
  if (!source)
    return;

#define CLEAN_RESOURCE(name) \
  do { \
    for (i = 0; i < output. name ## _total; i++) { \
      if (output.source_ ## name ## _map[i].id == source->id) { \
        output.source_ ## name ## _map[i].source = 0; \
        if (forever) \
          output.source_ ## name ## _map[i].id = ULLONG_MAX; \
      } \
    } \
    i--; \
    while (output.source_ ## name ## _map[i].id == ULLONG_MAX) \
      i--; \
   output. name ## _total = i+1; \
  } while (0) 
  CLEAN_RESOURCE(buttons);
  CLEAN_RESOURCE(axis);
}

static void unijoy_sysfs_unmerge(struct unijoy_inph_source *source) {
  if (!source)
    return;

  if (source->state == UNIJOY_SOURCE_DISCONNECTED) {
    unijoy_sysfs_remove(source);
    return;
  }

  if (source->state != UNIJOY_SOURCE_MERGED)
    return;
  
  input_close_device(&source->handle);
  unijoy_sysfs_clean(source, true);
  unijoy_inph_refresh();
  source->state = UNIJOY_SOURCE_ONLINE;
}

static void unijoy_sysfs_remove(struct unijoy_inph_source *source) {
  if (!source)
    return;

  unijoy_sysfs_clean(source, true);
  unijoy_inph_refresh();
  
  spin_lock(&unijoy_sysfs.sources_lock);
  list_del(&source->list);
  spin_unlock(&unijoy_sysfs.sources_lock);
  
  kfree(source);
}

static void unijoy_sysfs_suspend(struct unijoy_inph_source *source) {
  if (!source)
    return;

  unijoy_sysfs_clean(source, false);
  unijoy_inph_refresh();
  source->state = UNIJOY_SOURCE_DISCONNECTED;
}

/* Input handlers implementation */

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

	return value < SHRT_MIN ? SHRT_MIN : (value > SHRT_MAX ? SHRT_MAX : value);
}

static struct unijoy_inph_source *unijoy_inph_create(struct input_dev *dev,
                                                     __u64 id) {
  struct unijoy_inph_source *source;
  int i, j, t;

  source = kzalloc(sizeof(struct unijoy_inph_source), GFP_KERNEL);
  if (!source) {
    return 0;
  }

  source->id = id;
  source->name = dev->name;

  for (i = 0; i < ABS_CNT; i++) {
    if (test_bit(i, dev->absbit)) {
      source->axis_map[i] = source->axis_total;
      source->axis_revmap[source->axis_total] = i;
      source->axis_total++;
    }
  }

  for (i = BTN_JOYSTICK - BTN_MISC; i < UNIJOY_MAX_BUTTONS; i++) {
    if (test_bit(i + BTN_MISC, dev->keybit)) {
      source->button_map[i] = source->buttons_total;
      source->button_revmap[source->buttons_total] = i + BTN_MISC;
      source->buttons_total++;
    }
  }

  for (i = 0; i < BTN_JOYSTICK - BTN_MISC; i++) {
    if (test_bit(i + BTN_MISC, dev->keybit)) {
      source->button_map[i] = source->buttons_total;
      source->button_revmap[source->buttons_total] = i + BTN_MISC;
      source->buttons_total++;
    }
  }

  for (i = 0; i < source->axis_total; i++) {
    j = source->axis_revmap[i];
    if (input_abs_get_max(dev, j) == input_abs_get_min(dev, j)) {
      source->corrections[i].type = JS_CORR_NONE;
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
    }
  }

  spin_lock(&unijoy_sysfs.sources_lock);
  INIT_LIST_HEAD(&source->list);
  list_add(&source->list, &unijoy_sysfs.sources.list);
  spin_unlock(&unijoy_sysfs.sources_lock);

  return source;
}

static void unijoy_inph_relink(struct unijoy_inph_source *source, __u64 id) {
  int i;
  for (i = 0; i < output.buttons_total; i++) {
    if (output.source_buttons_map[i].id == id) {
      output.source_buttons_map[i].source = source;
    }
  }
  for (i = 0; i < output.axis_total; i++) {
    if (output.source_axis_map[i].id == id) {
      output.source_axis_map[i].source = source;
    }
  }
}

static int unijoy_inph_connect(struct input_handler *handler,
                               struct input_dev *dev,
                               const struct input_device_id *in_id) {
  struct unijoy_inph_source *source;
  int error;
  __u64 id;

  id = ((long)dev->id.bustype << 48)
     | ((long)dev->id.vendor  << 32)
     | ((long)dev->id.product << 16)
     | ((long)dev->id.version      );

  if (id == 0)
    return 0;

  source = unijoy_sysfs_find(id);

  if (!source) {
    source = unijoy_inph_create(dev, id);
    if (!source)
      return -ENOMEM;
  }


  
  source->handle.dev     = input_get_device(dev);
  source->handle.handler = handler;
  source->handle.private = source;

  error = input_register_handle(&source->handle);

  if (error)
    return error;

  if (source->state == UNIJOY_SOURCE_DISCONNECTED) {
    unijoy_inph_relink(source, id);
    unijoy_sysfs_merge(source);
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
}

static int unijoy_thread_wakeup_condition(void) {
  return (output.head != output.tail || output.full)
      || kthread_should_stop();
}

static __u64 unijoy_thread_nextdata(void) {
  __u64 data = ULLONG_MAX;

  spin_lock_irq(&output.buffer_lock);
  data = output.buffer[output.tail++];
  output.tail &= UNIJOY_BUFFER_SIZE - 1;
  if (output.full)
    output.full = false;
  spin_unlock_irq(&output.buffer_lock);

  return data;
}

static int unijoy_thread(void *nulldata) {
  __u64 data;
  int action;
  int number;
  int value;

  while (1) {
    wait_event_interruptible(output.wait, unijoy_thread_wakeup_condition());
    
    if (kthread_should_stop()) 
      return 0;

    while (output.head != output.tail || output.full) {
      if (kthread_should_stop()) 
        return 0;
        
      data = unijoy_thread_nextdata();

      if (data == ULLONG_MAX) 
        continue;

      action = (int)(data & 0xFFFF);
      number = (int)((data>>16) & 0xFFFF);
      value  = (int)(data>>32);

      switch (action) {
        case UNIJOY_ACTION_EMMIT_BUTTON:
          input_report_key(output.idev, number, value);
          input_sync(output.idev);
          break;
        case UNIJOY_ACTION_EMMIT_AXIS:
          input_report_abs(output.idev, number, value);
          input_sync(output.idev);
          break;
        case UNIJOY_ACTION_REFRESH:
          unijoy_inph_unregister();
          unijoy_inph_register();
          break;
      }
    }
  }
  return 0;
}

static void unijoy_inph_enqueue(__u64 data) {
  unsigned long flags = 0;
  if (output.full)
    goto unlock_exit;

  
  spin_lock_irqsave(&output.buffer_lock, flags);
  
  output.buffer[output.head] = data;

  output.head++;
  output.head &= UNIJOY_BUFFER_SIZE - 1;

  if (output.head == output.tail )
    output.full = true;

  spin_unlock_irqrestore(&output.buffer_lock, flags);
unlock_exit:
  wake_up_interruptible(&output.wait);
}

static void unijoy_inph_event(struct input_handle *handle,
                              unsigned int type,
                              unsigned int code,
                              int value) {

  struct unijoy_inph_source *source = handle->private;
  int number;
  __u64 data;
  int i;
  int subcode;

  if (!source)
    return;

  switch (type) {
    case EV_KEY:
      if (code < BTN_MISC || value == 2)
        return;
      number = source->button_map[code - BTN_MISC];
      for (i = 0; i < output.buttons_total; i++) {
        if (output.source_buttons_map[i].source == source &&
            output.source_buttons_map[i].value == number) {
          subcode = i+BTN_JOYSTICK;
          if (subcode >= KEY_MAX) {
            subcode = i+BTN_MISC;
          }
          data = ((__u64)((__u32)value)<<32)
               | ((__u16)subcode<<16)
               | ((__u16)UNIJOY_ACTION_EMMIT_BUTTON);
          unijoy_inph_enqueue(data);
        }
      }
      break;
    case EV_ABS:
      number = source->axis_map[code];
      value = unijoy_inph_correct(value, &source->corrections[number]);
      for (i = 0; i < output.axis_total; i++) {
        if (output.source_axis_map[i].source == source &&
            output.source_axis_map[i].value == number) {
          data = ((__u64)((__u32)value)<<32)
               | ((__u16)i<<16)
               | ((__u16)UNIJOY_ACTION_EMMIT_AXIS);
          unijoy_inph_enqueue(data);
        }
      }
      break;
    default:
      return;
  }
}

static void unijoy_inph_refresh(void) {
  unijoy_inph_enqueue((__u64)((__u16)UNIJOY_ACTION_REFRESH));
}

static void unijoy_inph_unregister(void) {
  if (output.idev) {
    input_unregister_device(output.idev);
    input_free_device(output.idev);
    output.idev = 0;
  }
}
static void unijoy_inph_register(void) {
  int i;
  struct input_dev *idev = 0;

  if (output.axis_total <0 && output.buttons_total < 0)
    return;

  idev = input_allocate_device();
  idev->name = "unijoy v0.3";
  input_alloc_absinfo(idev);

  if (output.buttons_total > 0) {
    set_bit(EV_KEY, idev->evbit);
    for (i = 0; i < output.buttons_total; i++) {
      if (i+BTN_JOYSTICK < KEY_MAX) {
        set_bit(i+BTN_JOYSTICK, idev->keybit);
      } else {
        set_bit((i+BTN_JOYSTICK) - KEY_MAX + BTN_MISC, idev->keybit);
      }
    }
  }

  if (output.axis_total > 0) {
    set_bit(EV_ABS, idev->evbit);
    for (i = 0; i < output.axis_total; i++) {
      set_bit(i, idev->absbit);
    }
  }

  if (input_register_device(idev)) {
    input_free_device(idev);
  } else {
    output.idev = idev;
  }
}

/* Main entry points */

int __init unijoy_init(void) {
  int i;
  int error;

  error = unijoy_sysfs_setup();

  if (error)
    return error;

  error = input_register_handler(&unijoy_inph);

  if (error)
    goto err_free_sysfs; 

  for (i = 0; i < ABS_CNT; i++) {
    output.source_axis_map[i].id = ULLONG_MAX;
  }
  for (i = 0; i < UNIJOY_MAX_BUTTONS; i++) {
    output.source_buttons_map[i].id = ULLONG_MAX;
  }

  unijoy_inph_register();
  spin_lock_init(&output.buffer_lock);
  init_waitqueue_head(&output.wait);
  output.thread = kthread_create(unijoy_thread,0,"unijoy_thread");

  wake_up_process(output.thread);  

  return 0;
  
err_free_sysfs:
  unijoy_sysfs_free();
  
  return error;
}

void __exit unijoy_exit(void) {
  kthread_stop(output.thread);
  unijoy_inph_unregister();
  input_unregister_handler(&unijoy_inph);
  unijoy_sysfs_free();
}

module_init(unijoy_init);
module_exit(unijoy_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Alexander <iamtaingiteasy> Tumin");
MODULE_DESCRIPTION("Makes a union of N other input/js devices");
MODULE_SUPPORTED_DEVICE("input/jsN");

