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

#define UNIJOY_MINOR_BASE 0
#define UNIJOY_MINORS 16
#define UNIJOY_BUFFER_SIZE 64
#define UNIJOY_MAX_KEYS (KEY_MAX - BTN_MISC + 1)

struct unijoy_client {
	struct js_event buffer[UNIJOY_BUFFER_SIZE];
	int head;
	int tail;
	int startup;
	spinlock_t buffer_lock;
	struct fasync_struct *fasync;
	struct list_head list;
};

enum unijoy_source_state {
	UNIJOY_ONLINE,
	UNIJOY_MERGED,
	UNIJOY_WASMERGED
};

static char *unijoy_source_state_names[] = {
	"ONLINE",
	"MERGED",
	"WASMERGED"
};

struct unijoy_source {
	struct list_head list;
	long input_id;
	const char *name;
	int axes;
	int buttons;
	enum unijoy_source_state state;
	struct input_handle handle;
	__u16 keymap[UNIJOY_MAX_KEYS];
	__u16 keypam[UNIJOY_MAX_KEYS];
	__u8 absmap[ABS_CNT];
	__u8 abspam[ABS_CNT];
	__s16 axis[ABS_CNT];
	struct js_corr corr[ABS_CNT];
};

struct unijoy_axis {
	struct unijoy_source *source;
	int axis;
};

struct unijoy_button {
	struct unijoy_source *source;
	int button;
};

struct unijoy_dest {
	int axes;
	int buttons;
	__u8 abspam[ABS_CNT];
	__u16 keypam[UNIJOY_MAX_KEYS];
	struct unijoy_axis axis_map[ABS_CNT];
	struct unijoy_button button_map[UNIJOY_MAX_KEYS];
};

struct unijoy_attr {
	struct attribute attr;
	struct unijoy_source sources;
};

static struct list_head client_list; 
static spinlock_t client_lock; 
static wait_queue_head_t wait;
static struct mutex mutex; 
static struct cdev cdev;
static dev_t devt;
static struct unijoy_dest output;
struct JS_DATA_SAVE_TYPE glue;

static struct unijoy_attr unijoy_ctl_merger = {
	.attr.name = "merger",
	.attr.mode = 0666
};

static struct attribute * unijoy_ctl_attrs[] = {
	&unijoy_ctl_merger.attr,
	0
};

static long unijoy_make_input_id(struct input_id id) {
	long result = ((long)id.bustype << 48)
							+ ((long)id.vendor  << 32)
							+ ((long)id.product << 16)
							+ ((long)id.version);
	return result;
}

static ssize_t unijoy_ctl_merger_show(struct kobject *kobj,
	                                    struct attribute *attr,
	                                    char *buf) {
	int offset = 0;
	struct unijoy_source *source;

	list_for_each_entry(source, &unijoy_ctl_merger.sources.list, list) {
		offset += scnprintf(buf+offset, PAGE_SIZE-offset,
		                    "%ld\t%s\t\%d\t%d\t%s\n", 
												source->input_id, 
												unijoy_source_state_names[source->state], 
												source->axes, source->buttons, source->name);
	}

	offset += scnprintf(buf+offset, PAGE_SIZE-offset,
			"Operating as /dev/input/js%d\n", MINOR(devt));
	return offset;
}

static struct unijoy_source * unijoy_ctl_merger_find(long input_id) {
	struct unijoy_source *source, *result = 0;

	list_for_each_entry(source, &unijoy_ctl_merger.sources.list, list) {
		if (source->input_id == input_id) {
			result = source;
			break;
		}
	}
	return result;
}

static int unijoy_correct(int value, struct js_corr *corr) {
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

static struct unijoy_source * unijoy_ctl_merger_add(struct input_dev *dev) {
	long input_id = unijoy_make_input_id(dev->id);
	struct unijoy_source *source = unijoy_ctl_merger_find(input_id);
	int i, j, t;

	if (source) {
		return 0;
	}

	source = kzalloc(sizeof(struct unijoy_source), GFP_KERNEL);
	if (!source) {
		return 0;
	}

	INIT_LIST_HEAD(&source->list);
	list_add(&source->list, &unijoy_ctl_merger.sources.list);

	source->input_id = unijoy_make_input_id(dev->id);
	source->name = dev->name;

	for (i = 0; i < ABS_CNT; i++) {
		if (test_bit(i, dev->absbit)) {
			source->absmap[i] = source->axes;
			source->abspam[source->axes] = i;
			source->axes++;
		}
	}

	for (i = BTN_JOYSTICK - BTN_MISC; i < UNIJOY_MAX_KEYS; i++) {
		if (test_bit(i + BTN_MISC, dev->keybit)) {
			source->keymap[i] = source->buttons;
			source->keypam[source->buttons] = i + BTN_MISC;
			source->buttons++;
		}
	}

	for (i = 0; i < BTN_JOYSTICK - BTN_MISC; i++) {
		if (test_bit(i + BTN_MISC, dev->keybit)) {
			source->keymap[i] = source->buttons;
			source->keypam[source->buttons] = i + BTN_MISC;
			source->buttons++;
		}
	}

	for (i = 0; i < source->axes; i++) {
		j = source->abspam[i];
		if (input_abs_get_max(dev, j) == input_abs_get_min(dev, j)) {
			source->corr[i].type = JS_CORR_NONE;
			source->axis[i] = input_abs_get_val(dev, j);
			continue;
		}
		source->corr[i].type = JS_CORR_BROKEN;
		source->corr[i].prec = input_abs_get_fuzz(dev, j);

		t = (input_abs_get_max(dev, j) + input_abs_get_min(dev, j)) / 2;
		source->corr[i].coef[0] = t - input_abs_get_flat(dev, j);
		source->corr[i].coef[1] = t + input_abs_get_flat(dev, j);
		t = (input_abs_get_max(dev, j) - input_abs_get_min(dev, j)) / 2
			- 2 * input_abs_get_flat(dev, j);
		if (t) {
			source->corr[i].coef[2] = (1 << 29) / t;
			source->corr[i].coef[3] = (1 << 29) / t;

			source->axis[i] = unijoy_correct(input_abs_get_val(dev, j),
			                                         source->corr + i);
		}
	}

	return source;
}

static void unijoy_ctl_merger_remove(long input_id) {
	struct unijoy_source *source = unijoy_ctl_merger_find(input_id);

	if (source == 0) {
		return;
	}
	
	list_del(&source->list);

	kfree(source);
}

static void unijoy_ctl_merger_merge(struct unijoy_source *source) {
	int retval;

	if (!source) {
		return;
	}

	if (source->state != UNIJOY_ONLINE) {
		return;
	}
	
	retval = mutex_lock_interruptible(&mutex);
	if(retval) {
		return;
	}

	retval = input_open_device(&source->handle);

	source->state = UNIJOY_MERGED;

	mutex_unlock(&mutex);
}

static void unijoy_ctl_merger_unmerge(struct unijoy_source *source) {
	int retval;
	if (!source) {
		return;
	}
	if (source->state == UNIJOY_WASMERGED) {
		unijoy_ctl_merger_remove(source->input_id);
		return;
	}

	if (source->state != UNIJOY_MERGED) {
		return;
	}

	retval = mutex_lock_interruptible(&mutex);
	if (retval) {
		return;
	}
	input_close_device(&source->handle);
	source->state = UNIJOY_ONLINE;
	mutex_unlock(&mutex);
}

static void unijoy_ctl_merger_suspend(struct unijoy_source *source) {
	int retval;
	if (!source) {
		return;
	}
	retval = mutex_lock_interruptible(&mutex);
	if (retval) {
		return;
	}
	input_close_device(&source->handle);
	source->state = UNIJOY_WASMERGED;
	mutex_unlock(&mutex);
}

static void unijoy_ctl_merger_add_button(struct unijoy_source *source, int source_button, int dest_button) {
	if (!source) {
		return;
	}

	if (source_button < 0) {
		return;
	}

	if (source->buttons <= source_button) {
		return;
	}
	
	if (dest_button < 0) {
		dest_button = output.buttons;
	}

	if (dest_button+1 < UNIJOY_MAX_KEYS) {
		if (dest_button >= output.buttons) {
			output.buttons = dest_button+1;
		}

		output.button_map[dest_button].source = source;
		output.button_map[dest_button].button = source_button;
	}
}

static void unijoy_ctl_merger_del_button(int dest_button) {
	int i;

	if (dest_button < 0) {
		return;
	}

	if (dest_button >= UNIJOY_MAX_KEYS) {
		return;
	}

	if (!output.button_map[dest_button].source)	{
		return;
	}

	output.button_map[dest_button].source = 0;
	output.button_map[dest_button].button = 0;

	if (dest_button+1 == output.buttons) {
		i = dest_button;
		while(output.button_map[i].source == 0) {
			i--;
		}
		output.buttons = i+1;
	}
}


static void unijoy_ctl_merger_add_axis(struct unijoy_source *source, int source_axis, int dest_axis) {
	if (!source) {
		return;
	}

	if (source_axis < 0) {
		return;
	}

	if (source->axes <= source_axis) {
		return;
	}
	
	if (dest_axis < 0) {
		dest_axis = output.axes;
	}

	if (dest_axis+1 < ABS_CNT) {
		if (dest_axis >= output.axes) {
			output.axes = dest_axis+1;
		}

		output.axis_map[dest_axis].source = source;
		output.axis_map[dest_axis].axis = source_axis;
	}
}

static void unijoy_ctl_merger_del_axis(int dest_axis) {
	int i;

	if (dest_axis < 0) {
		return;
	}

	if (dest_axis >= ABS_CNT) {
		return;
	}

	if (!output.axis_map[dest_axis].source) {
		return;
	}

	output.axis_map[dest_axis].source = 0;
	output.axis_map[dest_axis].axis = 0;

	if (dest_axis+1 == output.axes) {
		i = dest_axis;
		while(output.axis_map[i].source == 0) {
			i--;
		}
		output.axes = i+1;
	}
}

static ssize_t unijoy_ctl_merger_store(struct kobject *kobj, 
	                                     struct attribute *attr,
																			 const char *in_buf,
																			 size_t in_len) {
	struct unijoy_source *source;
	char *buf = kzalloc(in_len+1, GFP_KERNEL);
	char *ptr = buf;
	int op = 0;
	int len = in_len;
	char *word;
	int wordlen;
	long input_id = 0;
	int p1 = -1, p2 = -1;

	while (len--) {
		*ptr++ = *in_buf++;
	}

	len = in_len;
	ptr = buf;

	while (*ptr && *ptr == ' ' && len) {
		ptr++;
		len--;
	}

#define wordtest(word_literal, outcome) \
	word = word_literal; \
	wordlen = strlen(word); \
	if (op == 0 && len > wordlen && strncmp(word,ptr,wordlen) == 0) { \
		op = outcome; \
		ptr += wordlen; \
	}

	wordtest("merge", 1);
	wordtest("unmerge", 2);
	wordtest("add_button", 3);
	wordtest("del_button", 4);
	wordtest("add_axis", 5);
	wordtest("del_axis", 6);

	if (len == 0 || op == 0) {
		kfree(buf);
		return in_len;
	}

	switch (op) {
		case 1:
			sscanf(ptr, "%ld", &input_id);
			source = unijoy_ctl_merger_find(input_id);
			unijoy_ctl_merger_merge(source);
			break;
		case 2:
			sscanf(ptr, "%ld", &input_id);
			source = unijoy_ctl_merger_find(input_id);
			unijoy_ctl_merger_unmerge(source);
			break;
		case 3:
			sscanf(ptr, "%ld %d %d", &input_id, &p1, &p2);
			source = unijoy_ctl_merger_find(input_id);
			unijoy_ctl_merger_add_button(source, p1, p2);
			break;
		case 4:
			sscanf(ptr, "%d", &p1);
			unijoy_ctl_merger_del_button(p1);
			break;
		case 5:
			sscanf(ptr, "%ld %d %d", &input_id, &p1, &p2);
			source = unijoy_ctl_merger_find(input_id);
			unijoy_ctl_merger_add_axis(source, p1, p2);
			break;
		case 6:
			sscanf(ptr, "%d", &p1);
			unijoy_ctl_merger_del_axis(p1);
			break;
	}

	kfree(buf);
	return in_len;
}

static struct sysfs_ops unijoy_ctl_merger_ops = {
	.show = unijoy_ctl_merger_show,
	.store = unijoy_ctl_merger_store
};

static struct kobj_type unijoy_ctl_merger_type = {
	.sysfs_ops = &unijoy_ctl_merger_ops,
	.default_attrs = unijoy_ctl_attrs
};

static struct kobject *unijoy_kobject;

static int unijoy_setup_ctl(void) {
	unijoy_kobject = kzalloc(sizeof(struct kobject), GFP_KERNEL);

	if (!unijoy_kobject) {
		return -ENOMEM;
	}

	kobject_init(unijoy_kobject, &unijoy_ctl_merger_type);
	if (kobject_add(unijoy_kobject, 0, "unijoy_ctl")) {
		kobject_put(unijoy_kobject);
		kfree(unijoy_kobject);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&unijoy_ctl_merger.sources.list);
	return 0;
}

static void unijoy_free_ctl(void) {
	struct list_head *pos, *q;
	struct unijoy_source *source;

	list_for_each_safe(pos,q,&unijoy_ctl_merger.sources.list) {
		source = list_entry(pos, struct unijoy_source, list);
		list_del(pos);
		kfree(source);
	}

	kobject_put(unijoy_kobject);
	kfree(unijoy_kobject);
}

static void unijoy_attach_client(struct unijoy_client *client) {
	spin_lock(&client_lock);
	list_add_tail_rcu(&client->list, &client_list);
	spin_unlock(&client_lock);
}

static void unijoy_detach_client(struct unijoy_client *client) {
	spin_lock(&client_lock);
	list_del_rcu(&client->list);
	spin_unlock(&client_lock);
	synchronize_rcu();
}

static int unijoy_open(struct inode *i, struct file *f) {
	struct unijoy_client *client;

	client = kzalloc(sizeof(struct unijoy_client), GFP_KERNEL);
	if (!client) {
		return -ENOMEM;
	}

	spin_lock_init(&client->buffer_lock);
	unijoy_attach_client(client);

	f->private_data = client;
	nonseekable_open(i,f);

	return 0;
}

static int unijoy_release(struct inode *i, struct file *f) {
	struct unijoy_client *client = f->private_data;
	unijoy_detach_client(client);
	kfree(client);
	return 0;
}

static int unijoy_generate_startup_event(struct unijoy_client *client, 
	                                       struct js_event *event) {
	int have_event;
	struct unijoy_button unijoy_button;
	struct unijoy_axis unijoy_axis;
	struct input_dev *input;
	
	int button;

	spin_lock_irq(&client->buffer_lock);
	have_event = client->startup < (output.axes + output.buttons);

	if (have_event) {
		event->time = jiffies_to_msecs(jiffies);
		if (client->startup < output.buttons) {
			event->type = JS_EVENT_BUTTON | JS_EVENT_INIT;
			event->number = client->startup;
			unijoy_button = output.button_map[event->number];
			if (!unijoy_button.source) {
				event->value = 0;
			} else {
				button = unijoy_button.source->keypam[unijoy_button.button];
				input = unijoy_button.source->handle.dev;
				event->value = !!test_bit(button, input->key);
			}
		} else {
			event->type = JS_EVENT_AXIS | JS_EVENT_INIT;
			event->number = client->startup - output.buttons;

			unijoy_axis = output.axis_map[event->number];

			if (!unijoy_axis.source) {
				event->value = 0;
			} else {
				event->value = unijoy_axis.source->axis[unijoy_axis.axis];
			}
		}
		client->startup++;
	}

	
	spin_unlock_irq(&client->buffer_lock);
	return have_event;
}

static int unijoy_fetch_next_event(struct unijoy_client *client,
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

static inline int unijoy_data_pending(struct unijoy_client *client)
{
	int result;
	result = client->startup < (output.axes + output.buttons) ||
		client->head != client->tail;
	return result;
}

static ssize_t unijoy_read(struct file *f, char __user *buf, 
	                         size_t count, loff_t *ppos) {
	struct unijoy_client *client = f->private_data;
	struct js_event event;
	int retval;


	if (count < sizeof(struct js_event)) {
		return -EINVAL;
	}

	if (count == sizeof(struct JS_DATA_TYPE)) {
		// old interface TODO
		return -EINVAL;
	}

	if (!unijoy_data_pending(client) && (f->f_flags & O_NONBLOCK)) {
		return -EAGAIN;
	}

	retval = wait_event_interruptible(wait, unijoy_data_pending(client));
	if (retval) {
		return retval;
	}

	while (retval + sizeof(struct js_event) <= count && 
	       unijoy_generate_startup_event(client, &event)) {
		if (copy_to_user(buf + retval, &event, sizeof(struct js_event))) {
			return -EFAULT;
		}

		retval += sizeof(struct js_event);
	}

	while (retval + sizeof(struct js_event) <= count &&
	       unijoy_fetch_next_event(client, &event)) {

		if (copy_to_user(buf + retval, &event, sizeof(struct js_event))) {
			return -EFAULT;
		}

		retval += sizeof(struct js_event);
	}

	return retval;
}


static ssize_t unijoy_write(struct file *f, const char __user *buf, 
	                          size_t len, loff_t *off) {
	return len;
}


static long unijoy_ioctl_common(unsigned int cmd, void __user *argp) {
	size_t len;
	const char *name;
	long ret;

	switch (cmd) {
		case JS_SET_CAL:
			return copy_from_user(&glue.JS_CORR, argp,
			                      sizeof(glue.JS_CORR)) ? -EFAULT : 0;
		case JS_GET_CAL:
			return copy_to_user(argp, &glue.JS_CORR,
			                    sizeof(glue.JS_CORR)) ? -EFAULT : 0;
		case JS_SET_TIMEOUT:
			return get_user(glue.JS_TIMEOUT, (s32 __user *) argp);

		case JS_GET_TIMEOUT:
			return put_user(glue.JS_TIMEOUT, (s32 __user *) argp);

		case JSIOCGVERSION:
			return put_user(JS_VERSION, (__u32 __user *) argp);

		case JSIOCGAXES:
			ret = put_user(output.axes, (__u8 __user *) argp);
			return ret;

		case JSIOCGBUTTONS:
			ret = put_user(output.buttons, (__u8 __user *) argp);
			return ret;

		case JSIOCSCORR:
		case JSIOCGCORR:
			return -EINVAL;
	}
	
	switch (cmd & ~IOCSIZE_MASK) {
	  case (JSIOCGAXMAP & ~IOCSIZE_MASK):
			len = min_t(size_t, _IOC_SIZE(cmd), sizeof(output.abspam));
			ret = copy_to_user(argp, output.abspam, len) ? -EFAULT : len;
			return ret;

		case (JSIOCGBTNMAP & ~IOCSIZE_MASK):
			len = min_t(size_t, _IOC_SIZE(cmd), sizeof(output.keypam));
			ret = copy_to_user(argp, output.keypam, len) ? -EFAULT : len;
			return ret;

		case (JSIOCSAXMAP & ~IOCSIZE_MASK):
		case (JSIOCSBTNMAP & ~IOCSIZE_MASK):
			return -EINVAL;

		case JSIOCGNAME(0):
			name = "unijoy v0.1";
			len = min_t(size_t, _IOC_SIZE(cmd), strlen(name) + 1);
			return copy_to_user(argp, name, len) ? -EFAULT : len;
	}

	return -EINVAL;
}


static long unijoy_ioctl(struct file *file,
			 unsigned int cmd, unsigned long arg) {
	void __user *argp = (void __user *)arg;
	int retval;

	retval = mutex_lock_interruptible(&mutex);
	if (retval) {
		return retval;
	}

	switch (cmd) {
		case JS_SET_TIMELIMIT:
			retval = get_user(glue.JS_TIMELIMIT, (long __user *) arg);
			break;
		case JS_GET_TIMELIMIT:
			retval = put_user(glue.JS_TIMELIMIT, (long __user *) arg);
			break;
		case JS_SET_ALL:
			retval = copy_from_user(&glue, argp, 
			                        sizeof(glue)) ? -EFAULT : 0;
			break;
		case JS_GET_ALL:
			retval = copy_to_user(argp, &glue,
			                      sizeof(glue)) ? -EFAULT : 0;
			break;
		default:
			retval = unijoy_ioctl_common(cmd, argp);
			break;
	}

	mutex_unlock(&mutex);

	return retval;
}

static int unijoy_fasync(int fd, struct file *file, int on) {
	struct unijoy_client *client = file->private_data;

	return fasync_helper(fd, file, on, &client->fasync);
}

static struct file_operations unijoy_fops = {
	.owner = THIS_MODULE,
	.open = unijoy_open,
	.release = unijoy_release,
	.read = unijoy_read,
	.write = unijoy_write,
	.unlocked_ioctl = unijoy_ioctl,
	.fasync = unijoy_fasync,
	.llseek = no_llseek
};

static void unijoy_pass_event(struct unijoy_client *client, 
	                            struct js_event *event) {
	spin_lock(&client->buffer_lock);
	client->buffer[client->head] = *event;
	if (client->startup == output.axes + output.buttons) {
		client->head++;
		client->head &= UNIJOY_BUFFER_SIZE - 1;
		if (client->tail == client->head) {
			client->startup = 0;
		}
	}
	spin_unlock(&client->buffer_lock);
	kill_fasync(&client->fasync, SIGIO, POLL_IN);
}

static void unijoy_distribute_event(struct js_event *event) {
	struct unijoy_client *client;
	rcu_read_lock();
	list_for_each_entry_rcu(client, &client_list, list) {
		unijoy_pass_event(client, event);
	}
	rcu_read_unlock();
}

static void unijoy_event(struct input_handle *handle,
	                       unsigned int type, unsigned int code, int value) {
	struct unijoy_source *source = handle->private;
	struct js_event event;
	struct js_event pass;
	int i;

	switch (type) {
		case EV_KEY:
			if (code < BTN_MISC || value == 2) {
				return;
			}
			event.type = JS_EVENT_BUTTON;
			event.number = source->keymap[code - BTN_MISC];
			event.value = value;
			break;
		case EV_ABS:
			event.type = JS_EVENT_AXIS;
			event.number = source->absmap[code];
			event.value = unijoy_correct(value, &source->corr[event.number]);
			if (event.value == source->axis[event.number]) {
				return;
			}
			source->axis[event.number] = event.value;
			break;
		default:
			return;
	}


	event.time = jiffies_to_msecs(jiffies);

	switch (event.type) {
		case JS_EVENT_BUTTON:
			for (i = 0; i < output.buttons; i++) {
				if (output.button_map[i].source == source && 
						output.button_map[i].button == event.number ) {
					pass = event;
					pass.number = i;
					unijoy_distribute_event(&pass);
				}
			}
			break;
		case JS_EVENT_AXIS:
			for (i = 0; i < output.axes; i++) {
				if (output.axis_map[i].source == source &&
						output.axis_map[i].axis == event.number ) {
					pass = event;
					pass.number = i;
					unijoy_distribute_event(&pass);
				}
			}
			break;
	}	

	wake_up_interruptible(&wait);
}


static bool unijoy_match(struct input_handler *handler, struct input_dev *dev)
{
	/* Avoid touchpads and touchscreens */
	if (test_bit(EV_KEY, dev->evbit) && test_bit(BTN_TOUCH, dev->keybit))
		return false;

	/* Avoid tablets, digitisers and similar devices */
	if (test_bit(EV_KEY, dev->evbit) && test_bit(BTN_DIGI, dev->keybit))
		return false;

	return true;
}

static int unijoy_connect(struct input_handler *handler, struct input_dev *dev,
	                        const struct input_device_id *id) {
	struct unijoy_source *source;
	int error;
	long input_id;
	
	input_id = unijoy_make_input_id(dev->id);
	source = unijoy_ctl_merger_find(input_id);

	if (!source) {
		source = unijoy_ctl_merger_add(dev);
		if (!source) {
			return -EINVAL;
		}
	} else {
		if (source->state == UNIJOY_WASMERGED) {
			unijoy_ctl_merger_merge(source);
		}
	}

	source->handle.dev = input_get_device(dev);
	source->handle.name = "jsN";
	source->handle.handler = handler;
	source->handle.private = source;	

	error = input_register_handle(&source->handle);
	
	if (error) {
		return error;
	}

	return 0;
}

static void unijoy_disconnect(struct input_handle *handle) {
	struct unijoy_source *source = handle->private;
	
	if (source->state == UNIJOY_MERGED) {
		unijoy_ctl_merger_suspend(source);
	} else {
		unijoy_ctl_merger_remove(source->input_id);
	}

	input_unregister_handle(handle);
}


static const struct input_device_id unijoy_ids[] = {
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
	{ }	/* Terminating entry */
};

MODULE_DEVICE_TABLE(input, unijoy_ids);

static struct input_handler unijoy_handler = {
	.event = unijoy_event,
	.match = unijoy_match,
	.connect = unijoy_connect,
	.disconnect = unijoy_disconnect,
	.legacy_minors = true,
	.minor = UNIJOY_MINOR_BASE,
	.name = "unijoy",
	.id_table = unijoy_ids
};


int __init unijoy_init(void) 
{
	int i;
	int minor;
	int error;
	
	minor = input_get_new_minor(UNIJOY_MINOR_BASE, UNIJOY_MINORS, true);
	
	if (minor < 0) {
		return -EINVAL;
	}

	devt = MKDEV(INPUT_MAJOR, minor);
	
	if (device_create(&input_class, NULL, devt, NULL, "js%d", minor) == NULL) {
		error = -EINVAL;
		goto unijoy_input_free_minor;
	}

	cdev_init(&cdev, &unijoy_fops);
	error = cdev_add(&cdev, devt, 1);
	
	if (error) {
		goto unijoy_cdev_destroy;
	}

	error = unijoy_setup_ctl();
	
	if (error) {
		goto unijoy_cdev_del;
	}
	
	INIT_LIST_HEAD(&client_list);
	spin_lock_init(&client_lock);
	mutex_init(&mutex);
	init_waitqueue_head(&wait);

	for (i = 0; i < ABS_CNT; i++) {
		output.abspam[i] = i;
	}
	for (i = 0; i < UNIJOY_MAX_KEYS; i++) {
		output.keypam[i] = BTN_MISC + i;
	}


	error = input_register_handler(&unijoy_handler);

	return error;
	
unijoy_cdev_del:
	cdev_del(&cdev);
	
unijoy_cdev_destroy:
	device_destroy(&input_class, devt);

unijoy_input_free_minor:
	input_free_minor(minor);
	return error;
}

void __exit unijoy_exit(void) {
	input_unregister_handler(&unijoy_handler);
	unijoy_free_ctl();
	cdev_del(&cdev);
	device_destroy(&input_class, devt);
	input_free_minor(MINOR(devt));
}

module_init(unijoy_init);
module_exit(unijoy_exit);
