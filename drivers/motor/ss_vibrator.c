
/* Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include "../staging/android/timed_output.h"
#include <linux/pm_qos.h>
#include <linux/wakelock.h>

#include "ss_vibrator.h"

#if defined(CONFIG_SLPI_MOTOR)
#include <linux/adsp/slpi_motor.h>
#endif

/* default timeout */
#define VIB_DEFAULT_TIMEOUT 10000
#define PACKET_MAX_SIZE		1000

struct pm_qos_request pm_qos_req;
static struct wake_lock vib_wake_lock;

struct vib_tuning {
	int m;
	int n;
};

struct vib_packet {
	int time;
	int intensity;
	int freq;
	int overdrive;
};

struct ss_vib {
	struct device *dev;
	struct hrtimer vib_timer;
	struct timed_output_dev timed_dev;
	struct work_struct work;
	struct workqueue_struct *queue;
	struct mutex lock;

	int state;
	int timeout;
	int intensity;
	int force_touch_intensity;
	int freq;
	int timevalue;
	int f_packet_en;
	int packet_size;
	int packet_cnt;
	int f_overdrive_en;

	unsigned int vib_pwm_gpio;	/* gpio number for vibrator pwm */
	unsigned int vib_en_gpio;	/* gpio number of vibrator enable */
	unsigned int vib_power_gpio;	/* gpio number of vibrator boost */
	unsigned int flag_en_gpio;
	enum driver_chip chip_model;
	unsigned int gp_clk;
	unsigned int m_default;
	unsigned int n_default;
	unsigned int motor_strength;
	unsigned int strength_od;
	unsigned int strength_default;
	struct vib_tuning tuning[MAX_FREQUENCY];
	struct vib_packet haptic_eng[PACKET_MAX_SIZE];
	void (*power_onoff)(int onoff);
};

#if defined(CONFIG_BOOST_POWER_SHARE)
#define BOOST_REQUESTER_MOTOR	0
#define BOOST_REQUESTER_HRM	1

char boost_power_on(struct ss_vib *vib,char requester, char onoff)
{
	static char motor_on = 0, hrm_on = 0;

	if (requester == BOOST_REQUESTER_MOTOR) {
		if (onoff)
			motor_on = 1;
		else
			motor_on = 0;
	}

	if (requester == BOOST_REQUESTER_HRM) {
		if (onoff)
			hrm_on = 1;
		else
			hrm_on = 0;
	}

	if (vib != NULL) {
		if (vib->vib_power_gpio > 0) {
			if (motor_on || hrm_on) 
				gpio_direction_output(vib->vib_power_gpio, 1);
			else 
				gpio_direction_output(vib->vib_power_gpio, 0);
		} else {
			pr_info("%s, didn't get gpio number\n", __func__);
			return -EIO;
		}
	} else {
		if (motor_on || hrm_on) 
			gpio_direction_output(1020, 1);
		else 
			gpio_direction_output(1020, 0);
	}

	pr_info("%s, request[%s][%s] motor[%d], hrm[%d]\n",__func__, requester ? "HRM":"MOTOR", onoff ? "ON":"OFF", motor_on, hrm_on);
	return (motor_on || hrm_on);
}
EXPORT_SYMBOL(boost_power_on);
#endif

void vibe_set_intensity(int intensity)
{
#if !defined(CONFIG_MOTOR_S2MU004)
	if (0 == intensity)
		vibe_pwm_onoff(0);
	else {
		if ((intensity < 0) || (intensity > MAX_INTENSITY)) {
			intensity = MAX_INTENSITY;
			pr_err("[VIB] used wrong intensity, force set [%d]\n", MAX_INTENSITY);
		}
		intensity = (intensity / 100);	// 100 = 10000 / 100

		vibe_set_pwm_freq(intensity);
		vibe_pwm_onoff(1);
	}
#else
	if (0 == intensity)
		vibe_pwm_onoff(0);
	else {
		if ((intensity < 0) || (intensity > MAX_INTENSITY)) {
			intensity = MAX_INTENSITY;
			pr_err("[VIB] used wrong intensity, force set [%d]\n", MAX_INTENSITY);
		}
		s2mu004_set_intensity(intensity);
		vibe_pwm_onoff(1);
	}
#endif
}

void vibe_set_freq(struct ss_vib *vib, int set_freq)
{
	unsigned int ip_clock = 93750;
	unsigned int freq = set_freq / 10;
	unsigned int base_n = 0, n_m2 = 0 , n_m3 = 0;
	unsigned int m2_freq = 0, m3_freq = 0;

	switch (set_freq) {
		case freq_alert :
			g_nlra_gp_clk_m = vib->tuning[freq_alert].m;
			g_nlra_gp_clk_n = vib->tuning[freq_alert].n;
			vib->freq = 158;
			break;
		case freq_low :
			g_nlra_gp_clk_m = vib->tuning[freq_low].m;
			g_nlra_gp_clk_n = vib->tuning[freq_low].n;
			vib->freq = 120;
			break;
		case freq_mid :
			g_nlra_gp_clk_m = vib->tuning[freq_mid].m;
			g_nlra_gp_clk_n = vib->tuning[freq_mid].n;
			vib->freq = 150;
			break;
		case freq_high :
			g_nlra_gp_clk_m = vib->tuning[freq_high].m;
			g_nlra_gp_clk_n = vib->tuning[freq_high].n;
			vib->freq = 200;
			break;
		case freq_0 :
			g_nlra_gp_clk_m = vib->tuning[freq_0].m;
			g_nlra_gp_clk_n = vib->tuning[freq_0].n;
			vib->freq = 180;
			break;
		default :
			vib->freq = freq;
			//	19200000 / 16 / 128 = 9375
			base_n = ip_clock / freq;
			n_m2 = base_n * 2; 
			n_m2 = (n_m2 + 5) / 10; // round
			n_m3 = base_n * 3;
			n_m3 = (n_m3 + 5) / 10; // round
			m2_freq = (ip_clock / n_m2) * 2;
			m3_freq = (ip_clock / n_m3) * 3;
			if ( abs(set_freq - m2_freq) <= abs(set_freq - m3_freq)) {
				g_nlra_gp_clk_m = 2;
				g_nlra_gp_clk_n = n_m2;
			} else {
				g_nlra_gp_clk_m = 3;
				g_nlra_gp_clk_n = n_m3;
			}
			break;
	}

	if (vib->f_overdrive_en) {
		motor_strength = vib->strength_od;
	}
	else {
		motor_strength = vib->strength_default;
	}

	g_nlra_gp_clk_d = g_nlra_gp_clk_n / 2;
	g_nlra_gp_clk_pwm_mul = motor_strength;
	motor_min_strength = g_nlra_gp_clk_n * MOTOR_MIN_STRENGTH / 100;
}

int32_t vibe_set_pwm_freq(int intensity)
{
	int32_t calc_d;
	int32_t calc_n, half_n;

	/* Put the MND counter in reset mode for programming */
	HWIO_OUTM(GPx_CFG_RCGR, HWIO_GP_SRC_SEL_VAL_BMSK,
				0 << HWIO_GP_SRC_SEL_VAL_SHFT); //SRC_SEL = 000(cxo)
	HWIO_OUTM(GPx_CFG_RCGR, HWIO_GP_SRC_DIV_VAL_BMSK,
				31 << HWIO_GP_SRC_DIV_VAL_SHFT); //SRC_DIV = 11111 (Div 16)
	HWIO_OUTM(GPx_CFG_RCGR, HWIO_GP_MODE_VAL_BMSK,
				2 << HWIO_GP_MODE_VAL_SHFT); //Mode Select 10
	//M value
	HWIO_OUTM(GPx_M_REG, HWIO_GP_MD_REG_M_VAL_BMSK,
		g_nlra_gp_clk_m << HWIO_GP_MD_REG_M_VAL_SHFT);

	calc_n = (~(g_nlra_gp_clk_n - g_nlra_gp_clk_m) & 0xFF);

	if (motor_strength > MAX_STRENGTH) {
		motor_strength = MAX_STRENGTH;
		pr_err("[VIB] used wrong motor_strength, force set [%d]\n", MAX_STRENGTH);
	}

	half_n = g_nlra_gp_clk_n >> 1;		// div 2, 50% duty D value is N / 2

	calc_d = (half_n * motor_strength) / 100;
	calc_d = (calc_d * intensity) / 100;
	calc_d = half_n - calc_d;

	calc_d = (~(calc_d << 1) & 0xFF);

	if (calc_d == 0xFF)
		calc_d = 0xFE;

	// D value
	HWIO_OUTM(GPx_D_REG, HWIO_GP_MD_REG_D_VAL_BMSK, calc_d);

	//N value
	HWIO_OUTM(GPx_N_REG, HWIO_GP_N_REG_N_VAL_BMSK, calc_n);

	return VIBRATION_SUCCESS;
}

int32_t vibe_pwm_onoff(u8 onoff)
{
	if (onoff) {
		HWIO_OUTM(GPx_CMD_RCGR,HWIO_UPDATE_VAL_BMSK,
					1 << HWIO_UPDATE_VAL_SHFT);//UPDATE ACTIVE
		HWIO_OUTM(GPx_CMD_RCGR,HWIO_ROOT_EN_VAL_BMSK,
					1 << HWIO_ROOT_EN_VAL_SHFT);//ROOT_EN
		HWIO_OUTM(CAMSS_GPx_CBCR, HWIO_CLK_ENABLE_VAL_BMSK,
					1 << HWIO_CLK_ENABLE_VAL_SHFT); //CLK_ENABLE
	} else {

		HWIO_OUTM(GPx_CMD_RCGR,HWIO_UPDATE_VAL_BMSK,
					0 << HWIO_UPDATE_VAL_SHFT);
		HWIO_OUTM(GPx_CMD_RCGR,HWIO_ROOT_EN_VAL_BMSK,
					0 << HWIO_ROOT_EN_VAL_SHFT);
		HWIO_OUTM(CAMSS_GPx_CBCR, HWIO_CLK_ENABLE_VAL_BMSK,
					0 << HWIO_CLK_ENABLE_VAL_SHFT);
	}
	return VIBRATION_SUCCESS;
}

static void max778xx_haptic_en(struct ss_vib *vib, bool onoff)
{
	switch (vib->chip_model) {
#if defined(CONFIG_MOTOR_DRV_MAX77854)
	case CHIP_MAX77854:
		max77854_vibtonz_en(onoff);
		break;
#endif
#if defined(CONFIG_MOTOR_DRV_SM5720)
	case CHIP_SM5720:
		sm5720_vibtonz_en(onoff);
		break;
#endif
#if defined(CONFIG_MOTOR_S2MU004)
	case CHIP_S2MU004:
		s2mu004_vibtonz_en(onoff);
		break;
#endif
	default:
		break;
	}
}

static void set_vibrator(struct ss_vib *vib)
{
#if !defined(CONFIG_MOTOR_S2MU004)
	struct pinctrl *motor_pinctrl;
#endif
	pr_info("[VIB]: %s, value[%d]\n", __func__, vib->state);
	if (vib->state) {
		wake_lock(&vib_wake_lock);
		pm_qos_update_request(&pm_qos_req, PM_QOS_NONIDLE_VALUE);
#if defined(CONFIG_SLPI_MOTOR)
		setSensorCallback(true, vib->timevalue);
#endif
#if !defined(CONFIG_MOTOR_S2MU004)
		motor_pinctrl = devm_pinctrl_get_select(vib->dev, "tlmm_motor_active");
		if (IS_ERR(motor_pinctrl)) {
			if (PTR_ERR(motor_pinctrl) == -EPROBE_DEFER)
				pr_err("[VIB]: Error %d\n", -EPROBE_DEFER);

			pr_debug("[VIB]: Target does not use pinctrl\n");
			motor_pinctrl = NULL;
		}
#else
		max778xx_haptic_en(vib, true);
#endif
#if defined(CONFIG_BOOST_POWER_SHARE)
		boost_power_on(vib, BOOST_REQUESTER_MOTOR, 1);
#else 
		if (vib->power_onoff)
			vib->power_onoff(1);
#endif
		if (vib->flag_en_gpio)
			gpio_set_value(vib->vib_en_gpio, VIBRATION_ON);
		gpio_set_value(vib->vib_pwm_gpio, VIBRATION_ON);
		hrtimer_start(&vib->vib_timer, ktime_set(vib->timevalue / 1000, (vib->timevalue % 1000) * 1000000),HRTIMER_MODE_REL);
	} else {
#if !defined(CONFIG_MOTOR_S2MU004)
		motor_pinctrl = devm_pinctrl_get_select(vib->dev, "tlmm_motor_suspend");
		if (IS_ERR(motor_pinctrl)) {
			if (PTR_ERR(motor_pinctrl) == -EPROBE_DEFER)
				pr_err("[VIB]: Error %d\n", -EPROBE_DEFER);

			pr_debug("[VIB]: Target does not use pinctrl\n");
			motor_pinctrl = NULL;
		}
		gpio_set_value(vib->vib_pwm_gpio, VIBRATION_OFF);
#else
		max778xx_haptic_en(vib, false);
#endif
		if (vib->flag_en_gpio)
			gpio_set_value(vib->vib_en_gpio, VIBRATION_OFF);
#if defined(CONFIG_BOOST_POWER_SHARE)
		boost_power_on(vib, BOOST_REQUESTER_MOTOR, 0);
#else
		if (vib->power_onoff)
			vib->power_onoff(0);
#endif
#if defined(CONFIG_SLPI_MOTOR)
		setSensorCallback(false, vib->timevalue);
#endif
		//PM_QOS_DEFAULT_VALUE
		wake_unlock(&vib_wake_lock);
		pm_qos_update_request(&pm_qos_req, PM_QOS_DEFAULT_VALUE);
	}
	pr_info("[VIB]: %s, vibrator control finish value[%d]\n", __func__, vib->state);
}

static void vibrator_enable(struct timed_output_dev *dev, int value)
{
	struct ss_vib *vib = container_of(dev, struct ss_vib, timed_dev);

	mutex_lock(&vib->lock);
	hrtimer_cancel(&vib->vib_timer);

	if (value == 0) {
		pr_info("[VIB]: OFF\n");
		vib->state = 0;
		vib->timevalue = 0;

		/*for packet disable*/
		vib->f_packet_en = 0;
		vib->packet_cnt = 0;
		vib->packet_size = 0;
		vib->f_overdrive_en = false;
	} else {
		vib->state = 1;
		vib->timevalue = value;
		if (f_multi_freq) {
			if (vib->f_packet_en) {
				vib->f_overdrive_en = vib->haptic_eng[0].overdrive;
				vibe_set_freq(vib, vib->haptic_eng[0].freq);
				vibe_set_intensity(vib->haptic_eng[0].intensity);
				vib->timevalue = vib->haptic_eng[0].time;
				vib->intensity = vib->haptic_eng[0].intensity;
				pr_info("[VIB] packet enabled");
			}
			pr_info("[VIB]: ON, Duration : %d msec, intensity : %d, freq : %d strength : %d od : %d\n", vib->timevalue, vib->intensity, vib->freq, motor_strength, vib->f_overdrive_en);
		} else {
			pr_info("[VIB]: ON, Duration : %d msec, intensity : %d\n", vib->timevalue, vib->intensity);
		}
	}

	mutex_unlock(&vib->lock);
	queue_work(vib->queue, &vib->work);
}

static void ss_vibrator_update(struct work_struct *work)
{
	struct ss_vib *vib = container_of(work, struct ss_vib, work);

	set_vibrator(vib);
}

static int vibrator_get_time(struct timed_output_dev *dev)
{
	struct ss_vib *vib = container_of(dev, struct ss_vib, timed_dev);

	if (hrtimer_active(&vib->vib_timer)) {
		ktime_t r = hrtimer_get_remaining(&vib->vib_timer);
		return (int)ktime_to_us(r);
	} else
		return 0;
}

static enum hrtimer_restart vibrator_timer_func(struct hrtimer *timer)
{
	struct ss_vib *vib = container_of(timer, struct ss_vib, vib_timer);
	int power_on;

	if (vib->f_packet_en) {
		if (++vib->packet_cnt >= vib->packet_size) {
			vib->state = 0;
			vib->f_packet_en = 0;
			vib->packet_cnt = 0;
			vib->packet_size = 0;
			vib->f_overdrive_en = false;
			queue_work(vib->queue, &vib->work);
		} else {
			power_on = vib->haptic_eng[vib->packet_cnt].intensity? 1 : 0; 
#if defined(CONFIG_BOOST_POWER_SHARE) 
			boost_power_on(vib, BOOST_REQUESTER_MOTOR, power_on); 
#else  
			if (vib->power_onoff) 
				vib->power_onoff(power_on); 
#endif 
			vib->f_overdrive_en = vib->haptic_eng[vib->packet_cnt].overdrive;
			vibe_set_freq(vib, vib->haptic_eng[vib->packet_cnt].freq);
			vibe_set_intensity(vib->haptic_eng[vib->packet_cnt].intensity);
			vib->timevalue = vib->haptic_eng[vib->packet_cnt].time;
			vib->intensity = vib->haptic_eng[vib->packet_cnt].intensity;
			pr_info("[VIB] %s time[%d] intensity[%d] freq[%d](m=%d,n=%d) od[%d]\n", __func__, vib->timevalue, vib->intensity, vib->freq, g_nlra_gp_clk_m, g_nlra_gp_clk_n, vib->f_overdrive_en);
			hrtimer_forward_now(timer, ktime_set(vib->timevalue / 1000, (vib->timevalue % 1000) * 1000000));
			return HRTIMER_RESTART;
		}
	} else {
		vib->state = 0;
		queue_work(vib->queue, &vib->work);
	}

	return HRTIMER_NORESTART;
}

#if defined(CONFIG_PM)
static int ss_vibrator_suspend(struct device *dev)
{
	struct ss_vib *vib = dev_get_drvdata(dev);

	pr_info("[VIB]: %s\n", __func__);

	hrtimer_cancel(&vib->vib_timer);
	cancel_work_sync(&vib->work);
	/* turn-off vibrator */
	set_vibrator(vib);
	max778xx_haptic_en(vib, false);

	return 0;
}

static int ss_vibrator_resume(struct device *dev)
{
#if !defined(CONFIG_MOTOR_S2MU004)
	struct ss_vib *vib = dev_get_drvdata(dev);

	pr_info("[VIB]: %s\n", __func__);
	max778xx_haptic_en(vib, true);
#else
	pr_info("[VIB]: %s do nothing with s2mu004 haptic\n", __func__);
#endif

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(vibrator_pm_ops, ss_vibrator_suspend, ss_vibrator_resume);

static int vibrator_parse_dt(struct ss_vib *vib)
{
	struct device_node *np = vib->dev->of_node;
	int rc;

	vib->vib_pwm_gpio = of_get_named_gpio(np, "samsung,vib_pwm", 0);
	if (!gpio_is_valid(vib->vib_pwm_gpio)) {
		pr_err("%s:%d, reset gpio not specified\n", __func__, __LINE__);
	}

	vib->vib_en_gpio = of_get_named_gpio(np, "samsung,vib_en", 0);
	if (!gpio_is_valid(vib->vib_en_gpio)) {
		vib->flag_en_gpio = 0;
		pr_info("%s:%d, en gpio not specified\n", __func__, __LINE__);
	} else {
		vib->flag_en_gpio = 1;
	}
	
	vib->vib_power_gpio = of_get_named_gpio(np, "samsung,vib_power", 0);
	if (!gpio_is_valid(vib->vib_power_gpio)) {
		pr_err("%s:%d, power gpio not specified\n", __func__, __LINE__);
	}

	rc = of_property_read_u32(np, "samsung,chip_model", &vib->chip_model);
	if (rc) {
		pr_info("chip_model not specified so using SM5720\n");
		vib->chip_model = CHIP_SM5720;
	}

	rc = of_property_read_u32(np, "samsung,gp_clk", &vib->gp_clk);
	if (rc) {
		pr_info("gp_clk not specified so using default address\n");
		vib->gp_clk = MSM_GCC_GPx_BASE;
		rc = 0;
	}

	rc = of_property_read_u32(np, "samsung,support_multi_freq", &f_multi_freq);
	if (rc) {
		pr_info("support_multi_freq not specified so don't support multi freq\n");
		f_multi_freq = 0;
		rc = 0;
	}

	rc = of_property_read_u32(np, "samsung,strength_od", &vib->strength_od);
	if (rc) {
		pr_info("strength  not specified so use default strength\n");
		vib->strength_od = 94;
		rc = 0;
	}

	rc = of_property_read_u32(np, "samsung,strength_default", &vib->strength_default);
	if (rc) {
		pr_info("support_multi_freq not specified so don't support multi freq\n");
		vib->strength_default = 60;
		rc = 0;
	}

	if (f_multi_freq) {
		int ret;
		int array_val[2];

		ret = of_property_read_u32_array(np, "samsung,freq_0", array_val, 2);
		if (ret) {
			pr_info("%s: Unable to read freq_0\n", __func__);
			array_val[0] = GP_CLK_M_DEFAULT;
			array_val[1] = GP_CLK_N_DEFAULT;
		}
		vib->tuning[freq_0].m = array_val[0];
		vib->tuning[freq_0].n = array_val[1];

		ret = of_property_read_u32_array(np, "samsung,freq_low", array_val, 2);
		if (ret) {
			pr_info("%s: Unable to read freq_low\n", __func__);
			array_val[0] = GP_CLK_M_DEFAULT;
			array_val[1] = GP_CLK_N_DEFAULT;
		}
		vib->tuning[freq_low].m = array_val[0];
		vib->tuning[freq_low].n = array_val[1];

		ret = of_property_read_u32_array(np, "samsung,freq_mid", array_val, 2);
		if (ret) {
			pr_info("%s: Unable to read freq_mid\n", __func__);
			array_val[0] = GP_CLK_M_DEFAULT;
			array_val[1] = GP_CLK_N_DEFAULT;
		}
		vib->tuning[freq_mid].m = array_val[0];
		vib->tuning[freq_mid].n = array_val[1];

		ret = of_property_read_u32_array(np, "samsung,freq_high", array_val, 2);
		if (ret) {
			pr_info("%s: Unable to read freq_high\n", __func__);
			array_val[0] = GP_CLK_M_DEFAULT;
			array_val[1] = GP_CLK_N_DEFAULT;
		}
		vib->tuning[freq_high].m = array_val[0];
		vib->tuning[freq_high].n = array_val[1];

		ret = of_property_read_u32_array(np, "samsung,freq_alert", array_val, 2);
		if (ret) {
			pr_info("%s: Unable to read freq_alert\n", __func__);
			array_val[0] = GP_CLK_M_DEFAULT;
			array_val[1] = GP_CLK_N_DEFAULT;
		}
		vib->tuning[freq_alert].m = array_val[0];
		vib->tuning[freq_alert].n = array_val[1];
	} else {
		rc = of_property_read_u32(np, "samsung,m_default", &vib->m_default);
		if (rc) {
			pr_info("m_default not specified so using default address\n");
			vib->m_default = GP_CLK_M_DEFAULT;
			rc = 0;
		}

		rc = of_property_read_u32(np, "samsung,n_default", &vib->n_default);
		if (rc) {
			pr_info("n_default not specified so using default address\n");
			vib->n_default = GP_CLK_N_DEFAULT;
			rc = 0;
		}

		rc = of_property_read_u32(np, "samsung,motor_strength", &vib->motor_strength);
		if (rc) {
			pr_info("motor_strength not specified so using default address\n");
			vib->motor_strength = MOTOR_STRENGTH;
			rc = 0;
		}

	}

	return rc;
}

static struct device *vib_dev;
extern struct class *sec_class;

static ssize_t show_vib_tuning(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	sprintf(buf, "gp_m %d, gp_n %d, gp_d %d, pwm_mul %d, strength %d, min_str %d\n",
			g_nlra_gp_clk_m, g_nlra_gp_clk_n, g_nlra_gp_clk_d,
			g_nlra_gp_clk_pwm_mul, motor_strength, motor_min_strength);
	return strlen(buf);
}

static ssize_t store_vib_tuning(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	int temp_m, temp_n, temp_str;

	retval = sscanf(buf, "%1d %3d %2d", &temp_m, &temp_n, &temp_str);
	if (retval == 0) {
		pr_info("[VIB]: %s, fail to get vib_tuning value\n", __func__);
		return count;
	}

	g_nlra_gp_clk_m = temp_m;
	g_nlra_gp_clk_n = temp_n;
	g_nlra_gp_clk_d = temp_n / 2;
	g_nlra_gp_clk_pwm_mul = temp_n;
	motor_strength = temp_str;
	motor_min_strength = g_nlra_gp_clk_n*MOTOR_MIN_STRENGTH/100;

	pr_info("[VIB]: %s gp_m %d, gp_n %d, gp_d %d, pwm_mul %d, strength %d, min_str %d\n", __func__,
			g_nlra_gp_clk_m, g_nlra_gp_clk_n, g_nlra_gp_clk_d,
			g_nlra_gp_clk_pwm_mul, motor_strength, motor_min_strength);

	return count;                                                              
}                                                                                  

static DEVICE_ATTR(vib_tuning, 0660, show_vib_tuning, store_vib_tuning);           

static ssize_t intensity_store(struct device *dev,
		struct device_attribute *devattr, const char *buf, size_t count)
{
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 
	int ret = 0, set_intensity = 0; 

	ret = kstrtoint(buf, 0, &set_intensity);
	if (ret) {
		pr_err("[VIB]: %s failed to get intensity",__func__);
		return ret;
	}

	if ((set_intensity < 0) || (set_intensity > MAX_INTENSITY)) {
		pr_err("[VIB]: %sout of rage\n", __func__);
		return -EINVAL;
	}

	vibe_set_intensity(set_intensity);
	vib->intensity = set_intensity;

	return count;
}

static ssize_t intensity_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 

	return sprintf(buf, "intensity: %u\n", vib->intensity);
}

static DEVICE_ATTR(intensity, 0660, intensity_show, intensity_store);

#if !defined(CONFIG_MOTOR_S2MU004)
static ssize_t force_touch_intensity_store(struct device *dev,
		struct device_attribute *devattr, const char *buf, size_t count)
{
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 
	int ret = 0, set_intensity = 0; 

	ret = kstrtoint(buf, 0, &set_intensity);
	if (ret) {
		pr_err("[VIB]: %s failed to get force touch intensity",__func__);
		return ret;
	}

	if ((set_intensity < 0) || (set_intensity > MAX_INTENSITY)) {
		pr_err("[VIB]: %sout of rage\n", __func__);
		return -EINVAL;
	}

	vibe_set_intensity(set_intensity);
	vib->force_touch_intensity = set_intensity;

	return count;
}

static ssize_t force_touch_intensity_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 

	return sprintf(buf, "force touch intensity: %u\n", vib->force_touch_intensity);
}

static DEVICE_ATTR(force_touch_intensity, 0660, force_touch_intensity_show, force_touch_intensity_store);

static ssize_t multi_freq_store(struct device *dev,
		struct device_attribute *devattr, const char *buf, size_t count)
{
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 
	int ret = 0, set_freq = 0; 

	ret = kstrtoint(buf, 0, &set_freq);
	if (ret) {
		pr_err("[VIB]: %s failed to get multi_freq value",__func__);
		return ret;
	}

	if ((set_freq < 0) || (set_freq >= MAX_FREQUENCY)) {
		pr_err("[VIB]: %s out of freq range\n", __func__);
		return -EINVAL;
	}
	vibe_set_freq(vib, set_freq);

	pr_info("[VIB]: %s gp_m %d, gp_n %d, gp_d %d, pwm_mul %d, strength %d, min_str %d\n", __func__,
			g_nlra_gp_clk_m, g_nlra_gp_clk_n, g_nlra_gp_clk_d,
			g_nlra_gp_clk_pwm_mul, motor_strength, motor_min_strength);
	return count;
}

static ssize_t multi_freq_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 

	return sprintf(buf, "%s %d\n", f_multi_freq ? "MULTI" : "FIXED", vib->freq);
}

static DEVICE_ATTR(multi_freq, 0660, multi_freq_show, multi_freq_store);

static ssize_t haptic_engine_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{                                                                                         
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 
	int i = 0, _data = 0, tmp = 0;

	if (sscanf(buf++, "%4u", &_data) == 1) {
		if (_data > PACKET_MAX_SIZE * 4)
			pr_info("%s, [%d] packet size over, Please check again\n", __func__, _data);
		else {
			vib->packet_size = _data / 4;
			vib->packet_cnt = 0;
			vib->f_packet_en = true;
			vib->f_overdrive_en = true;

			buf = strstr(buf, " ");

			for (i = 0; i < vib->packet_size; i++) {
				for (tmp = 0; tmp < 4; tmp++) {
					if(buf==NULL){
						pr_err("%s, packet data error, Please check again\n", __func__);
						vib->f_packet_en = false;
						vib->f_overdrive_en = false;
						return size;
					}

					if (sscanf(buf++, "%5u", &_data) == 1) {
						switch (tmp) {
							case 0:
								vib->haptic_eng[i].time = _data;
								break;
							case 1:
								vib->haptic_eng[i].intensity = _data;
								break;
							case 2:
								vib->haptic_eng[i].freq = _data;
								break;
							case 3:
								vib->haptic_eng[i].overdrive = _data;
								break;
						}
						buf = strstr(buf, " ");
					} else {
						pr_info("%s, packet data error, Please check again\n", __func__);
						vib->f_packet_en = false;
						vib->f_overdrive_en = false;
						break;
					}
				}
			}
		}
	}

	return size;                                                          
}                                                                             

static ssize_t haptic_engine_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct timed_output_dev *t_dev = dev_get_drvdata(dev);
	struct ss_vib *vib = container_of(t_dev, struct ss_vib, timed_dev); 
	int i = 0, tmp = 0;
	char *bufp = buf;

	bufp += snprintf(bufp, PACKET_MAX_SIZE * 4, "\n");    
	for (i = 0; i < vib->packet_size && vib->f_packet_en; i++) {
		for (tmp = 0; tmp < 4; tmp++) {
			switch (tmp) {
				case 0:
					bufp += snprintf(bufp, PACKET_MAX_SIZE * 4, "%u ", vib->haptic_eng[i].time);    
					break;
				case 1:
					bufp += snprintf(bufp, PACKET_MAX_SIZE * 4, "%u ", vib->haptic_eng[i].intensity);
					break;
				case 2:
					bufp += snprintf(bufp, PACKET_MAX_SIZE * 4, "%u ", vib->haptic_eng[i].freq); 
					break;
				case 3:
					bufp += snprintf(bufp, PACKET_MAX_SIZE * 4, "%u ", vib->haptic_eng[i].overdrive); 
					break;
			}
		}
	}
	bufp += snprintf(bufp, PACKET_MAX_SIZE * 4, "\n");    

	return strlen(buf);                                                   
}                                                                             

static DEVICE_ATTR(haptic_engine, 0660, haptic_engine_show, haptic_engine_store);
#endif

#if defined(CONFIG_MOTOR_DRV_MAX77854) || defined(CONFIG_MOTOR_DRV_SM5720)
#if !defined(CONFIG_BOOST_POWER_SHARE)
static void regulator_power_onoff(int onoff)
{
	int ret;
	static struct regulator *reg_ldo;

	if (!reg_ldo) {
		reg_ldo = regulator_get(NULL, "pmcobalt_l25");
		if (IS_ERR(reg_ldo)) {
			printk(KERN_ERR"could not get 8998_ldo, rc = %ld\n", PTR_ERR(reg_ldo));
			return;
		}

		if (f_multi_freq)
			ret = regulator_set_voltage(reg_ldo, 3350000, 3350000);
		else
			ret = regulator_set_voltage(reg_ldo, 2800000, 2800000);
	}

	if (onoff) {
		if (regulator_is_enabled(reg_ldo)) {
			printk(KERN_DEBUG"[VIB]: power_on already\n");
		} else {
			ret = regulator_set_load(reg_ldo, 10000);
			if (ret < 0) {
				printk(KERN_ERR"regulator_set_load pmcobalt_l25 failed, rc=%d\n", ret);
				return;
			}
			ret = regulator_enable(reg_ldo);
			if (ret) {
				printk(KERN_ERR"enable ldo failed, rc=%d\n", ret);
				return;
			}
			printk(KERN_DEBUG"[VIB]: power_on now\n");
		}
	} else {
		if (regulator_is_enabled(reg_ldo)) {
			ret = regulator_set_load(reg_ldo, 0);
			if (ret < 0) {
				printk(KERN_ERR"regulator_set_load pmcobalt_l25 failed, rc=%d\n", ret);
			}
			ret = regulator_disable(reg_ldo);
			if (ret) {
				printk(KERN_ERR"disable ldo failed, rc=%d\n", ret);
				return;
			}
			printk(KERN_DEBUG"[VIB]: power_off now\n");
		} else {
			printk(KERN_DEBUG"[VIB]: power_off already\n");
		}
	}
}
#endif
#else
static void regulator_power_onoff(int onoff)
{
}
#endif

extern int haptic_homekey_press(void)
{
	/*for drv2624 panic prevention*/
	if(g_vib == NULL) {
		pr_info("[VIB] %s : NULL reference, return\n", __func__);
	        return -1;
	}

	mutex_lock(&g_vib->lock);

	max778xx_haptic_en(g_vib, true);

	g_vib->f_overdrive_en = true;
	g_vib->timevalue = 7;
	vibe_set_freq(g_vib, 2000);
	vibe_set_intensity(g_vib->force_touch_intensity);

	g_vib->state = 1;

	pr_info("[VIB] %s : time: %dmsec, intensity: %d, freq: %d, strength : %d\n", __func__, g_vib->timevalue, g_vib->force_touch_intensity, g_vib->freq, motor_strength);
	g_vib->f_overdrive_en = false;

	mutex_unlock(&g_vib->lock);
	
	queue_work(g_vib->queue, &g_vib->work);

	return 0;
}

extern int haptic_homekey_release(void)
{

	/*for drv2624 panic prevention*/
	if(g_vib == NULL) {
		pr_info("[VIB] %s : NULL reference, return\n", __func__);
	        return -1;
	}

	mutex_lock(&g_vib->lock);

	g_vib->f_overdrive_en = true;
	g_vib->timevalue = 7;
	vibe_set_freq(g_vib, 2000);
	vibe_set_intensity(g_vib->force_touch_intensity);

	g_vib->state = 1;

	pr_info("[VIB] %s : time: %dmsec, intensity: %d, freq: %d, strength : %d\n", __func__, g_vib->timevalue, g_vib->force_touch_intensity, g_vib->freq, motor_strength);
	g_vib->f_overdrive_en = false;

	mutex_unlock(&g_vib->lock);

	queue_work(g_vib->queue, &g_vib->work);

	return 0;
}
static int ss_vibrator_probe(struct platform_device *pdev)
{
	struct ss_vib *vib;
	struct pinctrl *motor_pinctrl;
	int rc = 0;

	pr_info("[VIB]: %s\n",__func__);

	vib = devm_kzalloc(&pdev->dev, sizeof(*vib), GFP_KERNEL);
	if (!vib) {
		pr_err("[VIB]: %s : Failed to allocate memory\n", __func__);
		return -ENOMEM;
	}

	if (!pdev->dev.of_node) {
		pr_err("[VIB]: %s failed, DT is NULL", __func__);
		return -ENODEV;
	}

	vib->dev = &pdev->dev;
	rc = vibrator_parse_dt(vib);
	if(rc)
		return rc;

	virt_mmss_gp1_base = ioremap(vib->gp_clk, 0x28);
	if (!virt_mmss_gp1_base)
		panic("[VIB]: Unable to ioremap MSM_MMSS_GP1 memory!");

#if defined(CONFIG_BOOST_POWER_SHARE)
		boost_power_on(vib, BOOST_REQUESTER_MOTOR, 0);
#else
	vib->power_onoff = regulator_power_onoff;
#endif
	vib->intensity = MAX_INTENSITY;
	vib->force_touch_intensity = MAX_INTENSITY;

	if (f_multi_freq) {
		g_nlra_gp_clk_m = vib->tuning[freq_alert].m;
		g_nlra_gp_clk_n = vib->tuning[freq_alert].n;
		vib->freq = freq_alert;
	} else {
		g_nlra_gp_clk_m = vib->m_default;
		g_nlra_gp_clk_n = vib->n_default;
		vib->freq = MAX_FREQUENCY;
	}
	g_nlra_gp_clk_d = (g_nlra_gp_clk_n / 2);
	g_nlra_gp_clk_pwm_mul = g_nlra_gp_clk_n;
	motor_min_strength = g_nlra_gp_clk_n * MOTOR_MIN_STRENGTH/100;

	vib->timeout = VIB_DEFAULT_TIMEOUT;

	vibe_set_intensity(vib->intensity);
	INIT_WORK(&vib->work, ss_vibrator_update);
	mutex_init(&vib->lock);

	vib->queue = create_singlethread_workqueue("ss_vibrator");
	if (!vib->queue) {
		pr_err("[VIB]: %s: can't create workqueue\n", __func__);
		return -ENOMEM;
	}

	hrtimer_init(&vib->vib_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->vib_timer.function = vibrator_timer_func;

	vib->timed_dev.name = "vibrator";
	vib->timed_dev.get_time = vibrator_get_time;
	vib->timed_dev.enable = vibrator_enable;

	motor_pinctrl = devm_pinctrl_get_select(vib->dev, "tlmm_motor_suspend");
	if (IS_ERR(motor_pinctrl)) {
		if (PTR_ERR(motor_pinctrl) == -EPROBE_DEFER)
			pr_err("[VIB]: Error %d\n", -EPROBE_DEFER);
			pr_debug("[VIB]: Target does not use pinctrl\n");
			motor_pinctrl = NULL;
		}
	gpio_set_value(vib->vib_pwm_gpio, VIBRATION_OFF);

	dev_set_drvdata(&pdev->dev, vib);

	g_vib = vib;

	max778xx_haptic_en(vib, true);

	rc = timed_output_dev_register(&vib->timed_dev);
	if (rc < 0) {
		pr_err("[VIB]: timed_output_dev_register fail (rc=%d)\n", rc);
		goto err_read_vib;
	}

	rc = sysfs_create_file(&vib->timed_dev.dev->kobj, &dev_attr_intensity.attr);
	if (rc < 0) {
		pr_err("[VIB]: Failed to register sysfs intensity: %d\n", rc);
	}

#if !defined(CONFIG_MOTOR_S2MU004)
	rc = sysfs_create_file(&vib->timed_dev.dev->kobj, &dev_attr_force_touch_intensity.attr);
	if (rc < 0) {
		pr_err("[VIB]: Failed to register sysfs force_touch_intensity: %d\n", rc);
	}

	if (f_multi_freq) {
		rc = sysfs_create_file(&vib->timed_dev.dev->kobj, &dev_attr_multi_freq.attr);
		if (rc < 0) {
			pr_err("[VIB]: Failed to register sysfs multi_freq: %d\n", rc);
		}

		rc = sysfs_create_file(&vib->timed_dev.dev->kobj, &dev_attr_haptic_engine.attr);
		if (rc < 0) {
			pr_err("[VIB]: Failed to register sysfs haptic_engine: %d\n", rc);
		}
	}
#endif

	vib_dev = device_create(sec_class, NULL, 0, NULL, "vib");
	if (IS_ERR(vib_dev)) {
		pr_info("[VIB]: Failed to create device for samsung vib\n");
	}

	rc = sysfs_create_file(&vib_dev->kobj, &dev_attr_vib_tuning.attr);
	if (rc) {
		pr_info("Failed to create sysfs group for samsung specific led\n");
	}
	wake_lock_init(&vib_wake_lock, WAKE_LOCK_SUSPEND, "vib_present");
	pm_qos_add_request(&pm_qos_req, PM_QOS_CPU_DMA_LATENCY,
                                                PM_QOS_DEFAULT_VALUE);

	return 0;
err_read_vib:
	iounmap(virt_mmss_gp1_base);
	destroy_workqueue(vib->queue);
	mutex_destroy(&vib->lock);
	return rc;
}

static int ss_vibrator_remove(struct platform_device *pdev)
{
	struct ss_vib *vib = dev_get_drvdata(&pdev->dev);
	iounmap(virt_mmss_gp1_base);
	pm_qos_remove_request(&pm_qos_req);

	destroy_workqueue(vib->queue);
	mutex_destroy(&vib->lock);
	wake_lock_destroy(&vib_wake_lock);
	return 0;
}

static const struct of_device_id vib_motor_match[] = {
	{	.compatible = "samsung_vib",
	},
	{}
};

static struct platform_driver ss_vibrator_platdrv =
{
	.driver =
	{
		.name = "samsung_vib",
		.owner = THIS_MODULE,
		.of_match_table = vib_motor_match,
		.pm	= &vibrator_pm_ops,
	},
	.probe = ss_vibrator_probe,
	.remove = ss_vibrator_remove,
};

static int __init ss_timed_vibrator_init(void)
{
	return platform_driver_register(&ss_vibrator_platdrv);
}

void __exit ss_timed_vibrator_exit(void)
{
	platform_driver_unregister(&ss_vibrator_platdrv);
}
module_init(ss_timed_vibrator_init);
module_exit(ss_timed_vibrator_exit);

MODULE_AUTHOR("Samsung Corporation");
MODULE_DESCRIPTION("timed output vibrator device");
MODULE_LICENSE("GPL v2");
