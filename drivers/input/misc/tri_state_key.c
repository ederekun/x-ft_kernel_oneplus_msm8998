/*
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/gpio.h>
#include <linux/gpio_keys.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>

#include <linux/switch.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>

#include <linux/regulator/consumer.h>

#include <linux/timer.h>

#define DRV_NAME "tri-state-key"
#define OPLUS_TRI_KEY "oplus,tri-state-key"

#define KEYCODE_BASE 600
#define TOTAL_KEYCODES 6

/*
 * Switch/input dev values by OP
 */
typedef enum {
	MODE_UNKNOWN,
	MODE_MUTE,
	MODE_DO_NOT_DISTURB,
	MODE_NORMAL,
	MODE_MAX_NUM
} tri_mode_t;

struct switch_dev_data {
	int irq_key1;
	int irq_key2;
	int irq_key3;

	int key1_gpio;
	int key2_gpio;
	int key3_gpio;

	short stored_current_mode;

	struct workqueue_struct *wq;
	struct work_struct work;

	struct device *dev;
	struct input_dev *oplus_input;
	struct switch_dev sdev;

	struct timer_list s_timer;

	struct pinctrl *key_pinctrl;
	struct pinctrl_state *set_state;
};

static struct switch_dev_data *switch_data;

static void switch_dev_report_input(struct input_dev **input, 
		int keyCode, int mode)
{
	input_report_key(*input, keyCode, mode);
	input_sync(*input);
	input_report_key(*input, keyCode, 0);
	input_sync(*input);
}

static void switch_dev_work(struct work_struct *work)
{
	int retry_count = 0, i, mode;
	bool switch_state[MODE_MAX_NUM];

	/* This will retain current mode if the GPIO state is indeterminate */
	mode = switch_data->stored_current_mode;

retry:
	switch_state[MODE_MUTE] = !gpio_get_value(switch_data->key1_gpio);
	switch_state[MODE_DO_NOT_DISTURB] = !gpio_get_value(switch_data->key2_gpio);
	switch_state[MODE_NORMAL] = !gpio_get_value(switch_data->key3_gpio);

	for (i = MODE_MUTE; i < MODE_MAX_NUM; i++) {
		if (!switch_state[i])
			continue;

		/* Try again if tri-state is transitioning */
		if (++retry_count > 1) {
			retry_count = 0;
			cpu_relax();
			goto retry;
		}

		mode = i;
	}

	if (mode == switch_data->stored_current_mode)
		return;

	switch_data->stored_current_mode = mode;

	switch_set_state(&switch_data->sdev, mode);
	switch_dev_report_input(&switch_data->oplus_input, KEY_F3, mode);
	printk("%s ,tristate set to state(%d) \n", __func__, switch_data->sdev.state);
}

irqreturn_t switch_dev_interrupt(int irq, void *_dev)
{
	queue_work(switch_data->wq, &switch_data->work);
	return IRQ_HANDLED;
}

static void timer_handle(unsigned long arg)
{
	queue_work(switch_data->wq, &switch_data->work);
}

#ifdef CONFIG_OF
static int switch_dev_get_devtree_pdata(struct device *dev)
{
	struct device_node *node;

	node = dev->of_node;
	if (!node)
		return -EINVAL;

	switch_data->key1_gpio = of_get_named_gpio(node, "tristate,gpio_key1", 0);
	if (!gpio_is_valid(switch_data->key1_gpio))
		return -EINVAL;

	switch_data->key2_gpio = of_get_named_gpio(node, "tristate,gpio_key2", 0);
	if (!gpio_is_valid(switch_data->key2_gpio))
		return -EINVAL;

	switch_data->key3_gpio = of_get_named_gpio(node, "tristate,gpio_key3", 0);
	if (!gpio_is_valid(switch_data->key3_gpio))
		return -EINVAL;

	return 0;
}

static struct of_device_id tristate_dev_of_match[] = {
	{ .compatible = "oneplus,tri-state-key", },
	{ },
};
MODULE_DEVICE_TABLE(of, tristate_dev_of_match);
#else
static inline int
switch_dev_get_devtree_pdata(struct device *dev)
{
	return 0;
}
#endif

static ssize_t proc_tri_state_read(struct file *file, char __user *user_buf,
		size_t count, loff_t *ppos)
{
	int ret = 0;
	char page[6] = {0};
	snprintf(page, sizeof(page), "%d\n", switch_data->stored_current_mode);
	ret = simple_read_from_buffer(user_buf, count, ppos, page, strlen(page));
	return ret;
}

static const struct file_operations proc_tri_state_ops = {
	.read  = proc_tri_state_read,
	.open  = simple_open,
	.owner = THIS_MODULE,
};

static int tristate_dev_init_trikey_proc(struct device *dev)
{
	int ret = 0;
	struct proc_dir_entry *prEntry_trikey = NULL;
	struct proc_dir_entry *prEntry_tmp = NULL;

	prEntry_trikey = proc_mkdir("tristatekey", NULL);
	if (prEntry_trikey == NULL) {
		ret = -ENOMEM;
		dev_err(dev, "Couldn't create trikey proc entry\n");
		goto err;
	}

	prEntry_tmp = proc_create("tri_state", 0644, prEntry_trikey, &proc_tri_state_ops);
	if (prEntry_tmp == NULL) {
		ret = -ENOMEM;
		dev_err(dev, "Couldn't create proc entry, %d\n");
	}

err:
	return ret;
}

static int tristate_dev_register_input(struct device *dev, 
		struct input_dev **input, const char *name)
{
	int error = 0;

	*input = input_allocate_device();
	(*input)->name = name;
	(*input)->dev.parent = dev;

	/* Oplus-specific properties */
	(*input)->id.vendor = 0x22d9;
	set_bit(EV_SYN, (*input)->evbit);
	set_bit(KEY_F3, (*input)->keybit);

	set_bit(EV_KEY, (*input)->evbit);

	input_set_drvdata(*input, switch_data);

	error = input_register_device(*input);
	if (error)
		dev_err(dev, "Failed to register input device\n");

	return error;
}

static void tristate_dev_unregister_input(struct input_dev **input)
{
	input_unregister_device(*input);
	input_free_device(*input);
}

static int tristate_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int error = 0;

	switch_data = kzalloc(sizeof(struct switch_dev_data), GFP_KERNEL);
	switch_data->dev = dev;

	// init input devices

	error = tristate_dev_register_input(dev, &switch_data->oplus_input, 
			OPLUS_TRI_KEY);
	if (error) {
		dev_err(dev, "Failed to register oplus input device\n");
		goto err_input_device_register;
	}

	// init pinctrl

	switch_data->key_pinctrl = devm_pinctrl_get(switch_data->dev);
	if (IS_ERR_OR_NULL(switch_data->key_pinctrl)) {
		dev_err(dev, "Failed to get pinctrl\n");
		goto err_switch_dev_register;
	}

	switch_data->set_state = pinctrl_lookup_state(switch_data->key_pinctrl,
		"pmx_tri_state_key_active");
	if (IS_ERR_OR_NULL(switch_data->set_state)) {
		dev_err(dev, "Failed to lookup_state\n");
		goto err_switch_dev_register;
	}

	pinctrl_select_state(switch_data->key_pinctrl, switch_data->set_state);

	// parse gpios from dt

	error = switch_dev_get_devtree_pdata(dev);
	if (error) {
		dev_err(dev, "parse device tree fail!!!\n");
		goto err_switch_dev_register;
	}

	// irqs and work, timer stuffs
	// config irq gpio and request irq

	switch_data->irq_key1 = gpio_to_irq(switch_data->key1_gpio);
	if (switch_data->irq_key1 <= 0) {
		printk("%s, irq number is not specified, irq #= %d, int pin=%d\n\n",
			__func__, switch_data->irq_key1, switch_data->key1_gpio);
		goto err_detect_irq_num_failed;
	} else {
		error = gpio_request(switch_data->key1_gpio,"tristate_key1-int");
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_request, err=%d", __func__, error);
			goto err_request_gpio;
		}
		error = gpio_direction_input(switch_data->key1_gpio);
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_direction_input, err=%d", __func__, error);
			goto err_set_gpio_input;
		}
		error = request_irq(switch_data->irq_key1, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING, "tristate_key1", switch_data);
		if (error) {
			dev_err(dev, "request_irq %i failed.\n",
				switch_data->irq_key1);
			switch_data->irq_key1 = -EINVAL;
			goto err_request_irq;
		}
	}

	switch_data->irq_key2 = gpio_to_irq(switch_data->key2_gpio);
	if (switch_data->irq_key2 <= 0) {
		printk("%s, irq number is not specified, irq #= %d, int pin=%d\n\n",
			__func__, switch_data->irq_key2, switch_data->key2_gpio);
		goto err_detect_irq_num_failed;
	} else {
		error = gpio_request(switch_data->key2_gpio,"tristate_key2-int");
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_request, err=%d", __func__, error);
			goto err_request_gpio;
		}
		error = gpio_direction_input(switch_data->key2_gpio);
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_direction_input, err=%d", __func__, error);
			goto err_set_gpio_input;
		}
		error = request_irq(switch_data->irq_key2, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING, "tristate_key2", switch_data);
		if (error) {
			dev_err(dev, "request_irq %i failed.\n",
				switch_data->irq_key2);
			switch_data->irq_key2 = -EINVAL;
			goto err_request_irq;
		}
	}

	switch_data->irq_key3 = gpio_to_irq(switch_data->key3_gpio);
	if (switch_data->irq_key3 <= 0) {
		printk("%s, irq number is not specified, irq #= %d, int pin=%d\n\n",
			__func__, switch_data->irq_key3, switch_data->key3_gpio);
		goto err_detect_irq_num_failed;
	} else {
		error = gpio_request(switch_data->key3_gpio,"tristate_key3-int");
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_request, err=%d", __func__, error);
			goto err_request_gpio;
		}
		error = gpio_direction_input(switch_data->key3_gpio);
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_direction_input, err=%d", __func__, error);
			goto err_set_gpio_input;
		}
		error = request_irq(switch_data->irq_key3, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING, "tristate_key3", switch_data);
		if (error) {
			dev_err(dev, "request_irq %i failed.\n",
				switch_data->irq_key3);
			switch_data->irq_key3 = -EINVAL;
			goto err_request_irq;
		}
	}

	// init single thread unbound workqueue

	switch_data->wq = alloc_workqueue("tsk_switch_wq", WQ_HIGHPRI, 1);
	if (!switch_data->wq)
		switch_data->wq = system_highpri_wq;

	INIT_WORK(&switch_data->work, switch_dev_work);

	init_timer(&switch_data->s_timer);
	switch_data->s_timer.function = &timer_handle;
	switch_data->s_timer.expires = jiffies + 5*HZ;

	add_timer(&switch_data->s_timer);

	enable_irq_wake(switch_data->irq_key1);
	enable_irq_wake(switch_data->irq_key2);
	enable_irq_wake(switch_data->irq_key3);

	// init switch device

	switch_data->sdev.name = DRV_NAME;
	error = switch_dev_register(&switch_data->sdev);
	if (error < 0) {
		dev_err(dev, "Failed to register switch dev\n");
		goto err_request_gpio;
	}

	// init proc fs

	error = tristate_dev_init_trikey_proc(dev);
	if (error < 0) {
		dev_err(dev, "Failed to init trikey proc\n");
		goto err_request_gpio;
	}

	return 0;

err_request_gpio:
	switch_dev_unregister(&switch_data->sdev);
err_request_irq:
err_detect_irq_num_failed:
err_set_gpio_input:
	gpio_free(switch_data->key1_gpio);
	gpio_free(switch_data->key2_gpio);
	gpio_free(switch_data->key3_gpio);
err_switch_dev_register:
	kfree(switch_data);
err_input_device_register:
	tristate_dev_unregister_input(&switch_data->oplus_input);
	dev_err(dev, "%s error: %d\n", __func__, error);
	return error;
}

static int tristate_dev_remove(struct platform_device *pdev)
{
	cancel_work_sync(&switch_data->work);
	destroy_workqueue(switch_data->wq);
	gpio_free(switch_data->key1_gpio);
	gpio_free(switch_data->key2_gpio);
	gpio_free(switch_data->key3_gpio);
	switch_dev_unregister(&switch_data->sdev);
	kfree(switch_data);
	return 0;
}

static struct platform_driver tristate_dev_driver = {
	.probe	= tristate_dev_probe,
	.remove	= tristate_dev_remove,
	.driver	= {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(tristate_dev_of_match),
	},
};
module_platform_driver(tristate_dev_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("switch Profiles by this triple key driver");
