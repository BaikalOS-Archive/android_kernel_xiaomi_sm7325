// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/kernfs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/pm_qos.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/thermal.h>
#include <linux/soc/qcom/panel_event_notifier.h>

#include <net/netlink.h>
#include <net/genetlink.h>

#include "thermal_core.h"
#include "../base/base.h"

#if defined(CONFIG_DRM_PANEL)
static struct drm_panel *active_panel;
#endif

struct mi_thermal_device {
	struct device *dev;
	struct class *class;
	struct attribute_group attrs;
};

struct freq_table {
	u32 frequency;
};

struct usb_monitor  {
	struct notifier_block psy_nb;
	struct work_struct usb_state_work;
	int usb_online;
};
static struct usb_monitor usb_state;

struct cpufreq_device {
	int id;
	unsigned int cpufreq_state;
	unsigned int max_level;
	struct freq_table *freq_table;	/* In descending order */
	struct cpufreq_policy *policy;
	struct list_head node;
	struct freq_qos_request *qos_req;
};

static int screen_state = 0;
static int screen_last_status = 0;

static void *cookie = NULL;

static atomic_t temp_state = ATOMIC_INIT(0);
static atomic_t sconfig = ATOMIC_INIT(-1);
static atomic_t balance_mode = ATOMIC_INIT(0);
static atomic_t charger_temp = ATOMIC_INIT(-1);
static atomic_t modem_limit = ATOMIC_INIT(0);
static atomic_t market_download_limit = ATOMIC_INIT(0);
static atomic_t flash_state = ATOMIC_INIT(0);
static atomic_t wifi_limit = ATOMIC_INIT(0);
static atomic_t poor_modem_limit = ATOMIC_INIT(0);

const char *board_sensor;
static char boost[128];
static char board_sensor_temp[128];
static char board_sensor_second_temp[128];
static struct mi_thermal_device mi_thermal_dev;

static struct workqueue_struct *screen_state_wq;
static struct delayed_work screen_state_dw;

static LIST_HEAD(cpufreq_dev_list);
static DEFINE_MUTEX(cpufreq_list_lock);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req);

static int cpufreq_set_level(struct cpufreq_device *cdev, unsigned long state)
{
	/* Request state should be less than max_level */
	if (WARN_ON(state > cdev->max_level))
		return -EINVAL;

	/* Check if the old cooling action is same as new cooling action */
	if (cdev->cpufreq_state == state)
		return 0;

	cdev->cpufreq_state = state;
	return freq_qos_update_request(cdev->qos_req,
				       cdev->freq_table[state].frequency);
}

void cpu_limits_set_level(unsigned int cpu, unsigned int max_freq)
{
	struct cpufreq_device *cpufreq_dev;
	unsigned int level = 0;

	list_for_each_entry(cpufreq_dev, &cpufreq_dev_list, node) {
		if (cpufreq_dev->id == cpu) {
			for (level = 0; level <= cpufreq_dev->max_level; level++) {
				if (max_freq >= cpufreq_dev->freq_table[level].frequency) {
					cpufreq_set_level(cpufreq_dev, level);
					break;
				}
			}
			break;
		}
	}
}

static unsigned int find_next_max(struct cpufreq_frequency_table *table,
				  unsigned int prev_max)
{
	struct cpufreq_frequency_table *pos;
	unsigned int max = 0;

	cpufreq_for_each_valid_entry (pos, table) {
		if (pos->frequency > max && pos->frequency < prev_max)
			max = pos->frequency;
	}

	return max;
}

static int cpu_thermal_init(void)
{
	int cpu, ret;
	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	for_each_possible_cpu (cpu) {
		unsigned int i;
		unsigned int freq;
		struct cpufreq_device *cpufreq_dev;

		req = &per_cpu(qos_req, cpu);
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: cpufreq policy not found for cpu%d\n",  __func__, cpu);
			return -ESRCH;
		}
		pr_debug("%s cpu=%d\n", __func__, cpu);

		i = cpufreq_table_count_valid_entries(policy);
		if (!i) {
			pr_err("%s: CPUFreq table not found or has no valid entries\n", __func__);
			return -ENODEV;
		}

		cpufreq_dev = kzalloc(sizeof(*cpufreq_dev), GFP_KERNEL);
		if (!cpufreq_dev)
			return -ENOMEM;

		cpufreq_dev->policy = policy;
		cpufreq_dev->qos_req = req;

		/* max_level is an index, not a counter */
		cpufreq_dev->max_level = i - 1;
		cpufreq_dev->id = policy->cpu;

		cpufreq_dev->freq_table = kmalloc_array(i, sizeof(*cpufreq_dev->freq_table), GFP_KERNEL);
		if (!cpufreq_dev->freq_table)
			return -ENOMEM;

		/* Fill freq-table in descending order of frequencies */
		for (i = 0, freq = -1; i <= cpufreq_dev->max_level; i++) {
			freq = find_next_max(policy->freq_table, freq);
			cpufreq_dev->freq_table[i].frequency = freq;

			/* Warn for duplicate entries */
			if (!freq)
				pr_warn("%s: table has duplicate entries\n", __func__);
			else
				pr_debug("%s: freq:%u KHz\n", __func__, freq);
		}

		ret = freq_qos_add_request(&policy->constraints, cpufreq_dev->qos_req,
				FREQ_QOS_MAX, cpufreq_dev->freq_table[0].frequency);
		if (ret < 0) {
			pr_err("%s: Failed to add freq constraint (%d)\n",
			       __func__, ret);
			return ret;
		}
		mutex_lock(&cpufreq_list_lock);
		list_add(&cpufreq_dev->node, &cpufreq_dev_list);
		mutex_unlock(&cpufreq_list_lock);
	}
	return ret;
}

static void destory_thermal_cpu(void)
{
	struct cpufreq_device *priv, *tmp;

	list_for_each_entry_safe(priv, tmp, &cpufreq_dev_list, node) {
		freq_qos_remove_request(priv->qos_req);
		list_del(&priv->node);
		kfree(priv->freq_table);
		kfree(priv);
	}
}

#define THERMAL_SHOW(name)\
static ssize_t thermal_##name##_show(struct device *dev,	\
		struct device_attribute *attr, char *buf)			\
{											\
	return snprintf(buf, PAGE_SIZE, name);	\
}

#define THERMAL_STORE(name)\
static ssize_t thermal_##name##_store(struct device *dev,			\
		struct device_attribute *attr, const char *buf, size_t len)	\
{								\
	int val = -1;				\
								\
	val = simple_strtol(buf, NULL, 10);\
								\
	atomic_set(&name, val);	\
								\
	return len;					\
}

#define THERMAL_ATTR(name) static DEVICE_ATTR(name, 0664, thermal_##name##_show, thermal_##name##_store)

static ssize_t cpu_limits_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t cpu_limits_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int cpu;
	unsigned int max;

	if (sscanf(buf, "cpu%u %u", &cpu, &max) != 2) {
		pr_err("input param error, can not prase param\n");
		return -EINVAL;
	}

	cpu_limits_set_level(cpu, max);

	return len;
}
static DEVICE_ATTR(cpu_limits, 0664, cpu_limits_show, cpu_limits_store);

static ssize_t thermal_board_sensor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (!board_sensor)
		board_sensor = "invalid";

	return snprintf(buf, PAGE_SIZE, "%s", board_sensor);
}
static DEVICE_ATTR(board_sensor, 0664, thermal_board_sensor_show, NULL);

static ssize_t thermal_balance_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&balance_mode));
}

THERMAL_STORE(balance_mode);
THERMAL_ATTR(balance_mode);

THERMAL_SHOW(board_sensor_temp);

static ssize_t thermal_board_sensor_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(board_sensor_temp, PAGE_SIZE, buf);

	return len;
}
THERMAL_ATTR(board_sensor_temp);

THERMAL_SHOW(board_sensor_second_temp);

static ssize_t thermal_board_sensor_second_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(board_sensor_second_temp, PAGE_SIZE, buf);

	return len;
}
THERMAL_ATTR(board_sensor_second_temp);

THERMAL_SHOW(boost);

static ssize_t thermal_boost_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(boost, PAGE_SIZE, buf);

	return len;
}
THERMAL_ATTR(boost);

static ssize_t thermal_charger_temp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&charger_temp));
}

THERMAL_STORE(charger_temp);
THERMAL_ATTR(charger_temp);

static ssize_t thermal_modem_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&modem_limit));
}

THERMAL_STORE(modem_limit);
THERMAL_ATTR(modem_limit);

static ssize_t thermal_market_download_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&market_download_limit));
}

THERMAL_STORE(market_download_limit);
THERMAL_ATTR(market_download_limit);

static ssize_t thermal_sconfig_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&sconfig));
}

THERMAL_STORE(sconfig);
THERMAL_ATTR(sconfig);

static ssize_t thermal_screen_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", screen_state);
}
static DEVICE_ATTR(screen_state, 0664, thermal_screen_state_show, NULL);

static ssize_t thermal_temp_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&temp_state));
}

THERMAL_STORE(temp_state);
THERMAL_ATTR(temp_state);

static ssize_t thermal_usb_online_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", usb_state.usb_online);
}
static DEVICE_ATTR(usb_online, 0664, thermal_usb_online_show, NULL);

static ssize_t thermal_flash_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&flash_state));
}
THERMAL_STORE(flash_state);
THERMAL_ATTR(flash_state);

static ssize_t thermal_wifi_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&wifi_limit));
}

THERMAL_STORE(wifi_limit);
THERMAL_ATTR(wifi_limit);

static ssize_t thermal_poor_modem_limit_show(struct device *dev,
                                      struct device_attribute *attr, char *buf)
{
        return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&poor_modem_limit));
}

THERMAL_STORE(poor_modem_limit);
THERMAL_ATTR(poor_modem_limit);

static struct attribute *mi_thermal_dev_attr_group[] = {
	&dev_attr_balance_mode.attr,
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr,
	&dev_attr_board_sensor_second_temp.attr,
	&dev_attr_boost.attr,
	&dev_attr_charger_temp.attr,
	&dev_attr_cpu_limits.attr,
	&dev_attr_flash_state.attr,
	&dev_attr_market_download_limit.attr,
	&dev_attr_modem_limit.attr,
	&dev_attr_poor_modem_limit.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_temp_state.attr,
	&dev_attr_usb_online.attr,
	&dev_attr_wifi_limit.attr,
	NULL,
};

static void create_thermal_message_node(void)
{
	int ret = 0;
	struct class *cls = NULL;
	struct kernfs_node *class_sd = NULL;
	struct kernfs_node *thermal_sd = NULL;
	struct kernfs_node *sysfs_sd = NULL;
	struct kobject *kobj_tmp = NULL;
	struct subsys_private *cp = NULL;

	sysfs_sd = kernel_kobj->sd->parent;
	if (sysfs_sd) {
		class_sd = kernfs_find_and_get(sysfs_sd, "class");
		if (class_sd) {
			thermal_sd = kernfs_find_and_get(class_sd, "thermal");
			if (thermal_sd) {
				kobj_tmp = (struct kobject *)thermal_sd->priv;
				if (kobj_tmp) {
					cp = to_subsys_private(kobj_tmp);
					cls = cp->class;
				} else
					pr_err("%s: can not find thermal kobj\n", __func__);
			} else
				pr_err("%s: can not find thermal_sd\n", __func__);
		} else
			pr_err("%s: can not find class_sd\n", __func__);
	} else
		pr_err("%s: sysfs_sd is NULL\n", __func__);

	if (!mi_thermal_dev.class && cls) {
		mi_thermal_dev.class = cls;
		mi_thermal_dev.dev = device_create(mi_thermal_dev.class, NULL, 'H', NULL, "thermal_message");
		if (!mi_thermal_dev.dev) {
			pr_err("%s create device dev err\n", __func__);
			return;
		}

		mi_thermal_dev.attrs.attrs = mi_thermal_dev_attr_group;
		ret = sysfs_create_group(&mi_thermal_dev.dev->kobj, &mi_thermal_dev.attrs);
		if (ret) {
			pr_err("%s ERROR: Cannot create sysfs structure!:%d\n", __func__, ret);
			return;
		}
	}
}

static void destroy_thermal_message_node(void)
{
	sysfs_remove_group(&mi_thermal_dev.dev->kobj, &mi_thermal_dev.attrs);
	if (mi_thermal_dev.class != NULL) {
		device_destroy(mi_thermal_dev.class,'H');
		mi_thermal_dev.class = NULL;
	}
}

#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
static const char *get_screen_state_name(int mode)
{
	if (mode == DRM_PANEL_EVENT_UNBLANK)
		return "On";
	else if (mode == DRM_PANEL_EVENT_BLANK_LP)
		return "Doze";
	else if (mode == DRM_PANEL_EVENT_BLANK)
		return "Off";
	else
		return "Unknown";
}

static void screen_state_for_thermal_callback(enum panel_event_notifier_tag tag,
		struct panel_event_notification *notification, void *client_data)
{
	if (!notification) {
		pr_err("%s:Invalid notification\n", __func__);
		return;
	}

	if (notification->notif_data.early_trigger)
		return;

	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_UNBLANK:
		screen_state = 1;
		break;
	case DRM_PANEL_EVENT_BLANK:
	case DRM_PANEL_EVENT_BLANK_LP:
		screen_state = 0;
		break;
	default:
		return;
	}

	pr_debug("%s: mode: %s, screen_state = %d\n", __func__,
			get_screen_state_name(notification->notif_type),
			screen_state);

	if (screen_last_status != screen_state) {
		sysfs_notify(&mi_thermal_dev.dev->kobj, NULL, "screen_state");
		screen_last_status = screen_state;
	}
}

static int thermal_check_panel(struct device_node *np)
{
	int i;
	int count;
	struct device_node *node;
	struct drm_panel *panel;

	count = of_count_phandle_with_args(np, "qcom,display-panels", NULL);
	if (count <= 0)
		return 0;

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "qcom,display-panels", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			active_panel = panel;
			return 0;
		} else
			active_panel = NULL;
	}

	return PTR_ERR(panel);
}

static void screen_state_check(struct work_struct *work)
{
	struct device_node *node;
	void *pvt_data = NULL;
	int error = 0;
	static int retry_count = 10;

	node = of_find_node_by_name(NULL, "mi-thermal-interface");
	if (!node) {
		pr_err("%s ERROR: Cannot find node with panel!", __func__);
		return;
	}

	error = thermal_check_panel(node);
	if (error == -EPROBE_DEFER)
		pr_err("%s ERROR: Cannot fine panel of node!", __func__);

	if (active_panel) {
		cookie = panel_event_notifier_register(
				PANEL_EVENT_NOTIFICATION_PRIMARY,
				PANEL_EVENT_NOTIFIER_CLIENT_MI_THERMAL,
				active_panel,
				screen_state_for_thermal_callback,
				pvt_data);
		if (IS_ERR(cookie))
			pr_err("%s:Failed to register for panel events\n", __func__);
		else
			pr_info("%s: panel_event_notifier_register succeed\n", __func__);
	} else if (retry_count > 0) {
		pr_err("%s:active_panel is NULL Failed to register for panel events\n", __func__);
		retry_count--;
		queue_delayed_work(screen_state_wq, &screen_state_dw, 5 * HZ);
	}
}
#endif

static int usb_online_callback(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct power_supply *psy = data;

	if (strcmp(psy->desc->name, "usb"))
		return NOTIFY_OK;
	schedule_work(&usb_state.usb_state_work);

	return NOTIFY_OK;
}

static void usb_online_work(struct work_struct *work)
{
	static struct power_supply *usb_psy;
	union power_supply_propval ret = { 0,};
	int err = 0;

	if (!usb_psy)
		usb_psy = power_supply_get_by_name("usb");

	if (usb_psy) {
		err = power_supply_get_property(usb_psy,
				POWER_SUPPLY_PROP_ONLINE, &ret);
		if (err) {
			pr_err("usb online read error:%d\n",err);
			return;
		}
		usb_state.usb_online = ret.intval;
		if (mi_thermal_dev.dev)
			sysfs_notify(&mi_thermal_dev.dev->kobj, NULL, "usb_online");
	}
}

static int of_parse_thermal_message(void)
{
	struct device_node *np;

	np = of_find_node_by_name(NULL, "mi-thermal-interface");
	if (!np)
		return -EINVAL;

	if (of_property_read_string(np, "board-sensor", &board_sensor))
		return -EINVAL;

	pr_info("%s board sensor: %s\n", __func__, board_sensor);

	return 0;
}

static int __init mi_thermal_interface_init(void)
{
	int ret;

#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
	screen_state_wq = create_singlethread_workqueue("screen_state_wq");
	if (screen_state_wq) {
		INIT_DELAYED_WORK(&screen_state_dw, screen_state_check);
		queue_delayed_work(screen_state_wq, &screen_state_dw, 5 * HZ);
	}
#endif

	cpu_thermal_init();

	ret = of_parse_thermal_message();
	if (ret)
		pr_err("%s: Can not parse thermal message node: %d\n", __func__, ret);

	create_thermal_message_node();

	INIT_WORK(&usb_state.usb_state_work, usb_online_work);
	usb_state.psy_nb.notifier_call=usb_online_callback;
	ret = power_supply_reg_notifier(&usb_state.psy_nb);
	if (ret < 0)
		pr_err("%s: usb online notifier registration failed err: %d\n",ret);

	return 0;
}
module_init(mi_thermal_interface_init);

static void __exit mi_thermal_interface_exit(void)
{
#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
	if (screen_state_wq) {
		cancel_delayed_work_sync(&screen_state_dw);
		destroy_workqueue(screen_state_wq);
	}

	if (active_panel && !IS_ERR(cookie))
		panel_event_notifier_unregister(cookie);
	else
		pr_err("%s:panel_event_notifier_unregister failed\n", __func__);
#endif

	destroy_thermal_message_node();
	destory_thermal_cpu();
}
module_exit(mi_thermal_interface_exit);

MODULE_AUTHOR("Xiaomi thermal team");
MODULE_DESCRIPTION("Xiaomi thermal control interface");
MODULE_LICENSE("GPL v2");
