/*
 *	JACK device detection driver.
 *
 *	Copyright (C) 2009 Samsung Electronics, Inc.
 *
 *	Authors:
 *		Uk Kim <w0806.kim@samsung.com>
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; version 2 of the License.
 */

#include <linux/module.h>
#include <linux/sysdev.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/switch.h>
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/wakelock.h>
#include <linux/slab.h>

#include <mach/hardware.h>
#include <mach/gpio.h>
//#include <plat/regs-gpio.h>
#include <plat/gpio-cfg.h>
#include <plat/irqs.h>
#include <asm/mach-types.h>

#include <mach/sec_jack.h>

#define CONFIG_DEBUG_SEC_JACK
#define SUBJECT "JACK_DRIVER"

#ifdef CONFIG_DEBUG_SEC_JACK
#define SEC_JACKDEV_DBG(format,...)\
	printk ("[ "SUBJECT " (%s,%d) ] " format "\n", __func__, __LINE__, ## __VA_ARGS__);
#else
#define SEC_JACKDEV_DBG(format,...)
#define DEBUG_LOG(format,...)
#endif

#define DETECTION_CHECK_COUNT	2
#define	DETECTION_CHECK_TIME	get_jiffies_64() + (HZ/10)// 1000ms / 10 = 100ms
#define	SEND_END_ENABLE_TIME	get_jiffies_64() + (HZ*1)// 1000ms * 1 = 1sec

#define SEND_END_CHECK_COUNT	3
#define SEND_END_CHECK_TIME     get_jiffies_64() + (HZ * 2) //2000ms 

#define WAKELOCK_DET_TIMEOUT	HZ * 5 //5 sec


static struct platform_driver sec_jack_driver;

struct class *jack_class;
EXPORT_SYMBOL(jack_class);

static struct device *jack_selector_fs;
EXPORT_SYMBOL(jack_selector_fs);

extern int s3c_adc_get_adc_data(int channel);

struct sec_jack_info {
	struct sec_jack_port port;
	struct input_dev *input;
};

static struct sec_jack_info *hi;

struct switch_dev switch_jack_detection = {
		.name = "h2w",
};

/* To support AT+FCESTEST=1 */
struct switch_dev switch_sendend = {
		.name = "send_end",
};
static struct timer_list send_end_key_event_timer;
static struct timer_list delay_work_timer;

static unsigned int current_jack_type_status;

static unsigned int key_enable;
static unsigned int key_pressed;
static unsigned int key_pressed_code;

static struct wake_lock jack_sendend_wake_lock;
static int recording_status;
static int send_end_irq_token;
static int jack_irq_token;

extern void McDrv_Ctrl_MICBIAS2(int enable);


unsigned int get_headset_status(void)
{
	SEC_JACKDEV_DBG(" headset_status %d", current_jack_type_status);
	return current_jack_type_status;
}

void set_recording_status(int value)
{
	recording_status = value;
}

static int get_recording_status(void)
{
	SEC_JACKDEV_DBG("recording_status : %d\n", recording_status);
	return recording_status;
}

EXPORT_SYMBOL(get_headset_status);

static void jack_input_selector(int jack_type_status)
{
	SEC_JACKDEV_DBG("jack_type_status = 0X%x", jack_type_status);
}


static void jack_type_detect_change(struct work_struct *ignored)
{
	int adc = 0;
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
	struct sec_gpio_info   *send_end = &hi->port.send_end;
	int state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;
	int count_pole=0;
	bool bQuit = false;

	while(!bQuit)
	{
		state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

		if(state)
		{
			adc = s3c_adc_get_adc_data(SEC_HEADSET_ADC_CHANNEL);
			printk(KERN_INFO "[ JACK_DRIVER : adc = %d, state = %d\n", adc, state);			

			/* 4 pole zone */
			if(adc < 3200 && adc >= 2500)
			{
				current_jack_type_status = SEC_HEADSET_4_POLE_DEVICE;
				printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 4 pole  headset attached : adc = %d\n",__func__,__LINE__, adc);
				bQuit = true;
				if(send_end_irq_token==0)
				{
					enable_irq(send_end->eint);
                                        enable_irq_wake(send_end->eint);
					send_end_irq_token=1;
				}
				McDrv_Ctrl_MICBIAS2(1);
			}
			/* 3 pole zone */
			else if(!adc)
			{
				/* detect 3pole or tv-out cable */
				current_jack_type_status = SEC_HEADSET_3_POLE_DEVICE;
				printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 3 pole headset or TV-out attatched : adc = %d\n", __func__,__LINE__,adc);
				bQuit=true;
				if(send_end_irq_token==1)
				{
					disable_irq(send_end->eint);
                                        disable_irq_wake(send_end->eint);
					send_end_irq_token=0;
				}
				if(!get_recording_status())
				{
					McDrv_Ctrl_MICBIAS2(0);
				}
			}
			/* unstable zone */
			else
			{	
				/* unknown cable or unknown case */
				if(count_pole == 10)
				{
					/* detect 3pole or tv-out cable */
					printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 3 pole headset or TV-out attatched : adc = %d\n", __func__,__LINE__,adc);
					count_pole = 0;
					if(send_end_irq_token==1)
					{
						disable_irq(send_end->eint);
                                                disable_irq_wake(send_end->eint);
						send_end_irq_token=0;
					}
					current_jack_type_status = SEC_HEADSET_3_POLE_DEVICE;
					if(!get_recording_status())
					{
						McDrv_Ctrl_MICBIAS2(0);
					}
					bQuit=true;
				}
				/* If 4 pole is inserted slowly, ADC value should be lower than 250.
			 	* So, check again.
				 */
				else
				{
					++count_pole;
					/* Todo : to prevent unexpected reset bug.
					 * 		  is it msleep bug? need wakelock.
					 */
					wake_lock_timeout(&jack_sendend_wake_lock, WAKELOCK_DET_TIMEOUT);
					msleep(20);
				}
			}
		} /* if(state) */
		else
		{
			bQuit=true;

			current_jack_type_status = SEC_JACK_NO_DEVICE;
			if(!get_recording_status())
			{
				McDrv_Ctrl_MICBIAS2(0);
    			}
			SEC_JACKDEV_DBG("JACK dev detached  \n");			

		}
                
		switch_set_state(&switch_jack_detection, current_jack_type_status);
		jack_input_selector(current_jack_type_status);
	}
}

static DECLARE_DELAYED_WORK(detect_jack_type_work, jack_type_detect_change);

static void jack_detect_change(struct work_struct *ignored)
{
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
	struct sec_gpio_info   *send_end = &hi->port.send_end;
	int state,count=20;

	cancel_delayed_work_sync(&detect_jack_type_work);
	state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

	wake_lock_timeout(&jack_sendend_wake_lock, WAKELOCK_DET_TIMEOUT);

	/* block send/end key event */
	mod_timer(&send_end_key_event_timer,SEND_END_CHECK_TIME);

	if(state)
	{
		/* check pin state repeatedly */
		while(count--)
		{
			if(state != (gpio_get_value(det_jack->gpio) ^ det_jack->low_active))
			{
				if(!get_recording_status())
				{
					McDrv_Ctrl_MICBIAS2(0);
				}
				return;
			}
			msleep(10);
		}
		McDrv_Ctrl_MICBIAS2(1);
		schedule_delayed_work(&detect_jack_type_work, 60);
	}
	else if(!state && current_jack_type_status != SEC_JACK_NO_DEVICE)
	{
		if(send_end_irq_token==1)
		{
			disable_irq(send_end->eint);
                        disable_irq_wake(send_end->eint);
			send_end_irq_token=0;
		}

		current_jack_type_status = SEC_JACK_NO_DEVICE;
        	
		if(!get_recording_status())
		{
			McDrv_Ctrl_MICBIAS2(0);
		}

		switch_set_state(&switch_jack_detection, current_jack_type_status);
		SEC_JACKDEV_DBG("JACK dev detached  \n");			

	}
}

static void sendend_switch_change(struct work_struct *ignored)
{
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
	struct sec_gpio_info   *send_end = &hi->port.send_end;
	int state, headset_state;
	int count=6;
	int adc = 0;;
	
	headset_state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;
	state = gpio_get_value(send_end->gpio) ^ send_end->low_active;
	

	wake_lock_timeout(&jack_sendend_wake_lock,WAKELOCK_DET_TIMEOUT);
		
	/* check pin state repeatedly */
	while(count-- && !key_pressed)
	{
		if(state != (gpio_get_value(send_end->gpio) ^ send_end->low_active) || !headset_state || current_jack_type_status == SEC_HEADSET_3_POLE_DEVICE)
		{
			printk(KERN_INFO "[ JACK_DRIVER] (%s,%d) ] SEND/END key is ignored. State is unstable.\n",__func__,__LINE__);
			return;
		}
                
		msleep(10);
	}

	if(state)
	{
		adc = s3c_adc_get_adc_data(SEC_HEADSET_ADC_CHANNEL);
		SEC_JACKDEV_DBG("state = %d, adc = %d\r\n", state, adc);

		if((adc >= 0 && adc <= 150))
		{
                        key_pressed_code = KEY_MEDIA;
			key_pressed = 1;
		}
		else if(adc >= 200 && adc <= 400)
		{
                        key_pressed_code = KEY_VOLUMEUP;
                        key_pressed = 1;
		}
		else if(adc >= 600 && adc <= 800)
		{
			key_pressed_code = KEY_VOLUMEDOWN;
                        key_pressed = 1;

		}

                if(key_pressed)
                {
                	SEC_JACKDEV_DBG("Pressd key = %d", key_pressed_code);

                	input_report_key(hi->input, key_pressed_code, 1);
                        switch_set_state(&switch_sendend, 1);
                	input_sync(hi->input);
                }
	}
	else
	{
                if(key_pressed) 
                {
                        SEC_JACKDEV_DBG("Released key = %d", key_pressed_code);
                        
			input_report_key(hi->input, key_pressed_code, 0);
                        switch_set_state(&switch_sendend, 0);
                        input_sync(hi->input);
			key_pressed = 0;

                }
	}
}

static void sendend_timer_work_func(struct work_struct *ignored)
{
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
 	int headset_state;

	headset_state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

	key_enable = 1;

	if(key_pressed && !headset_state)
	{
                SEC_JACKDEV_DBG("Forcely released key = %d", key_pressed_code);
                
		input_report_key(hi->input, key_pressed_code, 0);
                switch_set_state(&switch_sendend, 0);
		input_sync(hi->input);
		key_pressed = 0;
	}
}

static DECLARE_WORK(jack_detect_work, jack_detect_change);
static DECLARE_WORK(sendend_switch_work, sendend_switch_change);
static DECLARE_WORK(sendend_timer_work, sendend_timer_work_func);


//IRQ Handler
static irqreturn_t detect_irq_handler(int irq, void *dev_id)
{
	if (!jack_irq_token)
		return IRQ_HANDLED;
	
	key_enable = 0;
	schedule_work(&jack_detect_work);
	return IRQ_HANDLED;
}
 
static void send_end_key_event_timer_handler(unsigned long arg)
{
	schedule_work(&sendend_timer_work);
}

static irqreturn_t send_end_irq_handler(int irq, void *dev_id)
{
        struct sec_gpio_info   *det_jack = &hi->port.det_jack;
        int headset_state;

        headset_state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

        if (key_enable && headset_state)
        {
                schedule_work(&sendend_switch_work);		
        }

        return IRQ_HANDLED;
}

static ssize_t select_jack_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printk(KERN_INFO "[JACK] %s : operate nothing\n", __FUNCTION__);

	return 0;
}

static ssize_t select_jack_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int value = 0; 
	struct sec_gpio_info   *send_end = &hi->port.send_end;


	sscanf(buf, "%d", &value);
	printk(KERN_INFO "[JACK_DRIVER] User  selection : 0X%x", value);
		
	switch(value)
	{
		case SEC_HEADSET_3_POLE_DEVICE:
		{
			if(send_end_irq_token==1)
			{
				disable_irq(send_end->eint);
                                disable_irq_wake(send_end->eint);
				send_end_irq_token=0;
			}

			if(!get_recording_status())
			{
				McDrv_Ctrl_MICBIAS2(0);
			}

			current_jack_type_status = SEC_HEADSET_3_POLE_DEVICE;			
			printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 3 pole headset or TV-out attatched : \n", __func__,__LINE__);
			break;
		}
		case SEC_HEADSET_4_POLE_DEVICE:
		{
			McDrv_Ctrl_MICBIAS2(1);
			current_jack_type_status = SEC_HEADSET_4_POLE_DEVICE;
			printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 4 pole  headset attached : \n",__func__,__LINE__);
			if(send_end_irq_token==0)
			{
				enable_irq(send_end->eint);
                                enable_irq_wake(send_end->eint);
				send_end_irq_token=1;
			}
			break;
		}
		case SEC_JACK_NO_DEVICE:
		{
			if(send_end_irq_token==1)
			{
				disable_irq(send_end->eint);
                                disable_irq_wake(send_end->eint);
				send_end_irq_token=0;
			}
			current_jack_type_status = SEC_JACK_NO_DEVICE;

			if(!get_recording_status())
			{
				McDrv_Ctrl_MICBIAS2(0);
			}

			printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] JACK dev detached  \n",__func__,__LINE__);			
			break;

		}
		default:
			break;
	}

	switch_set_state(&switch_jack_detection, current_jack_type_status);
	jack_input_selector(current_jack_type_status);

	return size;
}


static void delay_work_timer_func(unsigned long unused)
{
	schedule_work(&jack_detect_work);
	jack_irq_token = 1;
}

static DEVICE_ATTR(select_jack, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, select_jack_show, select_jack_store);

static int sec_jack_probe(struct platform_device *pdev)
{
	int ret;
	struct sec_jack_platform_data *pdata = pdev->dev.platform_data;
	struct sec_gpio_info   *det_jack;
	struct sec_gpio_info   *send_end;
	struct input_dev	   *input;
	current_jack_type_status = SEC_JACK_NO_DEVICE;
	
	printk(KERN_INFO "SEC JACK: Registering jack driver\n");
	
	hi = kzalloc(sizeof(struct sec_jack_info), GFP_KERNEL);
	if (!hi)
		return -ENOMEM;

	memcpy (&hi->port, pdata->port, sizeof(struct sec_jack_port));

	input = hi->input = input_allocate_device();
	if (!input)
	{
		ret = -ENOMEM;
		printk(KERN_ERR "SEC JACK: Failed to allocate input device.\n");
		goto err_request_input_dev;
	}

	input->name = "sec_jack";
	set_bit(EV_SYN, input->evbit);
	set_bit(EV_KEY, input->evbit);
        
	set_bit(KEY_MEDIA, input->keybit);
        set_bit(KEY_VOLUMEUP, input->keybit);
        set_bit(KEY_VOLUMEDOWN, input->keybit);

	ret = input_register_device(input);
	if (ret < 0)
	{
		printk(KERN_ERR "SEC JACK: Failed to register driver\n");
		goto err_register_input_dev;
	}
	

	init_timer(&send_end_key_event_timer);
	send_end_key_event_timer.function = send_end_key_event_timer_handler;

	
	ret = switch_dev_register(&switch_jack_detection);
	if (ret < 0) 
	{
		printk(KERN_ERR "SEC JACK: Failed to register switch device\n");
		goto err_switch_dev_register;
	}

	ret = switch_dev_register(&switch_sendend);
	if (ret < 0) 
	{
		printk(KERN_ERR "SEC JACK: Failed to register switch device\n");
		goto err_switch_dev_register;
	}

	//Create JACK Device file in Sysfs
	jack_class = class_create(THIS_MODULE, "jack");
	if(IS_ERR(jack_class))
	{
		printk(KERN_ERR "Failed to create class(sec_jack)\n");
	}

	jack_selector_fs = device_create(jack_class, NULL, 0, NULL, "jack_selector");
	if (IS_ERR(jack_selector_fs))
		printk(KERN_ERR "Failed to create device(sec_jack)!= %ld\n", IS_ERR(jack_selector_fs));

	if (device_create_file(jack_selector_fs, &dev_attr_select_jack) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_select_jack.attr.name);	

	//GPIO configuration
	send_end = &hi->port.send_end;
	s3c_gpio_cfgpin(send_end->gpio, S3C_GPIO_SFN(send_end->gpio_af));
	s3c_gpio_setpull(send_end->gpio, S3C_GPIO_PULL_NONE);
	irq_set_irq_type(send_end->eint, IRQ_TYPE_EDGE_BOTH);
       
	det_jack = &hi->port.det_jack;
	s3c_gpio_cfgpin(det_jack->gpio, S3C_GPIO_SFN(det_jack->gpio_af));
	s3c_gpio_setpull(det_jack->gpio, S3C_GPIO_PULL_NONE);
	irq_set_irq_type(det_jack->eint, IRQ_TYPE_EDGE_BOTH);

	ret = request_irq(send_end->eint, send_end_irq_handler, IRQF_DISABLED, "sec_headset_send_end", NULL);


	SEC_JACKDEV_DBG("sended isr send=0X%x, ret =%d", send_end->eint, ret);
	if (ret < 0)
	{
		printk(KERN_ERR "SEC HEADSET: Failed to register send/end interrupt.\n");
		goto err_request_send_end_irq;
	}

	enable_irq_wake(send_end->eint); //Enables and disables must match

	disable_irq(send_end->eint);
        disable_irq_wake(send_end->eint);
	send_end_irq_token=0;

	det_jack->low_active = 1; 

	jack_irq_token = 0;
	ret = request_irq(det_jack->eint, detect_irq_handler, IRQF_DISABLED, "sec_headset_detect", NULL);

	SEC_JACKDEV_DBG("det isr det=0X%x, ret =%d", det_jack->eint, ret);
	if (ret < 0) 
	{
		printk(KERN_ERR "SEC HEADSET: Failed to register detect interrupt.\n");
		goto err_request_detect_irq;
	}

	enable_irq_wake(det_jack->eint);
        
	wake_lock_init(&jack_sendend_wake_lock, WAKE_LOCK_SUSPEND, "sec_jack");

	init_timer(&delay_work_timer);
	delay_work_timer.function = delay_work_timer_func;
	delay_work_timer.expires = get_jiffies_64() + (HZ * 5);
	add_timer(&delay_work_timer); 
	
	return 0;

err_request_send_end_irq:
	free_irq(det_jack->eint, 0);
err_request_detect_irq:
	switch_dev_unregister(&switch_jack_detection);
	switch_dev_unregister(&switch_sendend);
err_switch_dev_register:
	input_unregister_device(input);
err_register_input_dev:
	input_free_device(input);
err_request_input_dev:
	kfree (hi);

	return ret;	
}

static int sec_jack_remove(struct platform_device *pdev)
{
	SEC_JACKDEV_DBG("");
	input_unregister_device(hi->input);
	free_irq(hi->port.det_jack.eint, 0);
	free_irq(hi->port.send_end.eint, 0);
	switch_dev_unregister(&switch_jack_detection);
	switch_dev_unregister(&switch_sendend);
	return 0;
}

#ifdef CONFIG_PM
static int sec_jack_suspend(struct platform_device *pdev, pm_message_t state)
{
        SEC_JACKDEV_DBG("");
        
	if(current_jack_type_status == SEC_JACK_NO_DEVICE || current_jack_type_status == SEC_HEADSET_3_POLE_DEVICE)
	{
		if(!get_recording_status())
		{
			McDrv_Ctrl_MICBIAS2(0);
		}
	}

	return 0;
}
static int sec_jack_resume(struct platform_device *pdev)
{
        SEC_JACKDEV_DBG("");
	return 0;
}
#else
#define s3c_headset_resume	NULL
#define s3c_headset_suspend	NULL
#endif

static int __init sec_jack_init(void)
{
	SEC_JACKDEV_DBG("");
	return platform_driver_register(&sec_jack_driver);
}

static void __exit sec_jack_exit(void)
{
	platform_driver_unregister(&sec_jack_driver);
}

static struct platform_driver sec_jack_driver = {
	.probe		= sec_jack_probe,
	.remove		= sec_jack_remove,
	.suspend	= sec_jack_suspend,
	.resume		= sec_jack_resume,
	.driver		= {
		.name		= "sec_jack",
		.owner		= THIS_MODULE,
	},
};

module_init(sec_jack_init);
module_exit(sec_jack_exit);

MODULE_AUTHOR("Uk Kim <w0806.kim@samsung.com>");
MODULE_DESCRIPTION("SEC JACK detection driver");
MODULE_LICENSE("GPL");
