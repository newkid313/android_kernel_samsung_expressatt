/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <media/msm_camera.h>
#include <media/v4l2-subdev.h>
#include <mach/gpio.h>
#include <mach/camera.h>
#include <mach/express-gpio.h>

#include <asm/mach-types.h>
#include <mach/vreg.h>
#include <linux/io.h>

#include "msm.h"
#include "isx012.h"
#ifdef CONFIG_MACH_JAGUAR
#include "isx012_regs.h"
#elif defined(CONFIG_MACH_COMANCHE)
#include "isx012_regs_comanche.h"
#elif defined(CONFIG_MACH_AEGIS2)
#include "isx012_regs_aegis2.h"
#elif defined(CONFIG_MACH_APEXQ)
#include "isx012_regs_apexq.h"
#elif defined(CONFIG_MACH_EXPRESS)
#include "isx012_regs_express.h"
#elif defined(CONFIG_MACH_STRETTO)
#include "isx012_regs_stretto.h"
#else
#include "isx012_regs_v2.h"
#endif
#include "msm.h"
#include "msm_ispif.h"
#include "msm_sensor.h"

#undef CONFIG_LOAD_FILE
#ifdef CONFIG_LOAD_FILE

#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static char *isx012_regs_table;
static int isx012_regs_table_size;
static int isx012_write_regs_from_sd(char *name);
#define TABLE_MAX_NUM 500
int gtable_buf[TABLE_MAX_NUM];
int gAE_OFSETVAL = AE_OFSETVAL, gAE_MAXDIFF = AE_MAXDIFF;
#define ISX012_BURST_WRITE_LIST(A)	\
	isx012_i2c_write_list(A, (sizeof(A) / sizeof(A[0])), #A)
#else
#if defined(CONFIG_MACH_STRETTO)
#define ISX012_BURST_WRITE_LIST(A)	\
	isx012_i2c_write_list(A, (sizeof(A) / sizeof(A[0])), #A)
#else
#define ISX012_BURST_WRITE_LIST(A)	\
	isx012_i2c_burst_write_list(A, (sizeof(A) / sizeof(A[0])), #A)
#endif
#endif

static bool g_bCameraRunning;
static bool g_bPreFlash;

#define ISX012_WRITE_LIST(A)	\
	isx012_i2c_write_list(A, (sizeof(A) / sizeof(A[0])), #A)

#if defined(CONFIG_MACH_JAGUAR) || defined(CONFIG_MACH_STRETTO)
#define FLASH_PULSE_CNT 2
#else
#define FLASH_PULSE_CNT 4
#endif

#define MOVIEMODE_FLASH	17
#define FLASHMODE_FLASH	18

#define ERROR 1

#define IN_AUTO_MODE 1
#define IN_MACRO_MODE 2

struct isx012_work {
	struct work_struct work;
};

struct isx012_exif_data {
	unsigned short iso;
	unsigned short shutterspeed;
};

static struct i2c_client *isx012_client;
static struct isx012_exif_data *isx012_exif;
static struct msm_sensor_ctrl_t isx012_s_ctrl;
static struct device isx012_dev;
struct isx012_ctrl {
	const struct msm_camera_sensor_info *sensordata;
	struct isx012_userset settings;
	struct msm_camera_i2c_client *sensor_i2c_client;
	struct v4l2_subdev *sensor_dev;
	struct v4l2_subdev sensor_v4l2_subdev;
	struct v4l2_subdev_info *sensor_v4l2_subdev_info;
	uint8_t sensor_v4l2_subdev_info_size;
	struct v4l2_subdev_ops *sensor_v4l2_subdev_ops;

	int op_mode;
	int dtp_mode;
	int cam_mode;
	int vtcall_mode;
	int started;
	int flash_mode;
	int lowLight;
	int dtpTest;
	int af_mode;
	int af_status;
	unsigned int lux;
	int awb_mode;
	int samsungapp;
};

static unsigned int config_csi2;
static struct isx012_ctrl *isx012_ctrl;
int initFlag;

uint16_t g_ae_auto = 0, g_ae_now = 0;
int16_t g_ersc_auto = 0, g_ersc_now = 0;

struct isx012_format {
	enum v4l2_mbus_pixelcode code;
	enum v4l2_colorspace colorspace;
	u16 fmt;
	u16 order;
};
static void isx012_set_metering(int mode);
static int32_t isx012_sensor_setting(int update_type, int rt);
static DECLARE_WAIT_QUEUE_HEAD(isx012_wait_queue);
static void isx012_set_flash(int mode);
static int isx012_sensor_config(struct msm_sensor_ctrl_t *s_ctrl,
	void __user *argp);
DEFINE_MUTEX(isx012_mut);

/**
 * isx012_i2c_read_multi: Read (I2C) multiple bytes to the camera sensor
 * @client: pointer to i2c_client
 * @cmd: command register
 * @w_data: data to be written
 * @w_len: length of data to be written
 * @r_data: buffer where data is read
 * @r_len: number of bytes to read
 *
 * Returns 0 on success, <0 on error
 */

static int isx012_i2c_read_multi(unsigned short subaddr, unsigned long *data)
{
	unsigned char buf[4];
	struct i2c_msg msg = { isx012_client->addr, 0, 2, buf };

	int err = 0;

	if (!isx012_client->adapter)
		return -EIO;

	buf[0] = subaddr >> 8;
	buf[1] = subaddr & 0xff;

	err = i2c_transfer(isx012_client->adapter, &msg, 1);
	if (unlikely(err < 0)) {
		cam_err(" returned error, %d", err);
		return -EIO;
	}

	msg.flags = I2C_M_RD;
	msg.len = 4;

	err = i2c_transfer(isx012_client->adapter, &msg, 1);
	if (unlikely(err < 0)) {
		cam_err(" returned error, %d", err);
		return -EIO;
	}

	/*
	 * Data comes in Little Endian in parallel mode; So there
	 * is no need for byte swapping here
	 */
	*data = *(unsigned long *)(&buf);

	return err;
}

/**
 * isx012_i2c_read: Read (I2C) multiple bytes to the camera sensor
 * @client: pointer to i2c_client
 * @cmd: command register
 * @data: data to be read
 *
 * Returns 0 on success, <0 on error
 */
static int isx012_i2c_read(unsigned short subaddr, unsigned short *data)
{
	unsigned char buf[2];
	struct i2c_msg msg = { isx012_client->addr, 0, 2, buf };

	int err = 0;

	if (!isx012_client->adapter) {
		cam_err(" can't search i2c client adapter");
		return -EIO;
	}

	buf[0] = subaddr >> 8;
	buf[1] = subaddr & 0xff;

	err = i2c_transfer(isx012_client->adapter, &msg, 1);
	if (unlikely(err < 0)) {
		cam_err(" returned error, %d", err);
		return -EIO;
	}

	msg.flags = I2C_M_RD;

	err = i2c_transfer(isx012_client->adapter, &msg, 1);
	if (unlikely(err < 0)) {
		cam_err(" returned error, %d", err);
		return -EIO;
	}

	/*
	 * Data comes in Little Endian in parallel mode; So there
	 * is no need for byte swapping here
	 */
	*data = *(unsigned short *)(&buf);

	return err;
}

/**
 * isx012_i2c_write_multi: Write (I2C) multiple bytes to the camera sensor
 * @client: pointer to i2c_client
 * @cmd: command register
 * @w_data: data to be written
 * @w_len: length of data to be written
 *
 * Returns 0 on success, <0 on error
 */
static int isx012_i2c_write_multi(unsigned short addr,
				  unsigned int w_data, unsigned int w_len)
{
	unsigned char buf[w_len + 2];
	struct i2c_msg msg = { isx012_client->addr, 0, w_len + 2, buf };

	int retry_count = 5;
	int err = 0;

	if (!isx012_client->adapter) {
		cam_err(" can't search i2c client adapter");
		return -EIO;
	}

	buf[0] = addr >> 8;
	buf[1] = addr & 0xff;

	/*
	 * Data should be written in Little Endian in parallel mode; So there
	 * is no need for byte swapping here
	 */
	if (w_len == 1)
		buf[2] = (unsigned char)w_data;
	else if (w_len == 2)
		*((unsigned short *)&buf[2]) = (unsigned short)w_data;
	else
		*((unsigned int *)&buf[2]) = w_data;

	while (retry_count--) {
		err = i2c_transfer(isx012_client->adapter, &msg, 1);
		if (likely(err == 1))
			break;
	}

	if (err != 1) {
		cam_err(" returned error, %d", err);
		return -EIO;
	}

	return 0;
}

#define ISX012_BURST_DATA_LENGTH 2700
unsigned char isx012_buf[ISX012_BURST_DATA_LENGTH];
static int isx012_i2c_burst_write_list(struct isx012_short_t regs[], int size,
	char *name)
{
	int i = 0;
	int iTxDataIndex = 0;
	int retry_count = 5;
	int err = 1;

	struct i2c_msg msg = {isx012_client->addr, 0, 0, isx012_buf};

	CAM_DEBUG(" %s", name);

	if (!isx012_client->adapter) {
		cam_err(" can't search i2c client adapter");
		return -EIO;
	}

	while (i < size) {
		if (regs[i].subaddr == 0xFFFF) {
			CAM_DEBUG(" delay %dms", regs[i].value);
			msleep(regs[i].value);
		} else {
			if (0 == iTxDataIndex) {
				isx012_buf[iTxDataIndex++] =
					(regs[i].subaddr & 0xFF00) >> 8;
				isx012_buf[iTxDataIndex++] =
					(regs[i].subaddr & 0xFF);
			}

			retry_count = 5;

			if ((i < size - 1) && ((iTxDataIndex + regs[i].len)
				<= (ISX012_BURST_DATA_LENGTH - regs[i+1].len))
				&& (regs[i].subaddr + regs[i].len ==
				regs[i+1].subaddr)) {
				if (regs[i].len == 1) {
					isx012_buf[iTxDataIndex++] =
						(regs[i].value & 0xFF);
				} else {
					isx012_buf[iTxDataIndex++] =
						(regs[i].value & 0x00FF);
					isx012_buf[iTxDataIndex++] =
						(regs[i].value & 0xFF00) >> 8;
				}
			} else {
				if (regs[i].len == 1) {
					isx012_buf[iTxDataIndex++] =
						(regs[i].value & 0xFF);
					msg.len = iTxDataIndex;
				} else {
					isx012_buf[iTxDataIndex++] =
						(regs[i].value & 0x00FF);
					isx012_buf[iTxDataIndex++] =
						(regs[i].value & 0xFF00) >> 8;
					msg.len = iTxDataIndex;
				}

				while (retry_count--) {
					err  = i2c_transfer
						(isx012_client->adapter,
						&msg, 1);
					if (likely(err == 1))
						break;
				}
				iTxDataIndex = 0;
			}
			i++;
		}
	}

	if (err != 1) {
		cam_err(" returned error, %d", err);
		return -EIO;
	}

	return 0;
}

static int isx012_i2c_write_list(struct isx012_short_t regs[], int size,
				 char *name)
{
#ifdef CONFIG_LOAD_FILE
	isx012_write_regs_from_sd(name);
#else
	int err = 0;
	int i = 0;

	CAM_DEBUG(" %s", name);

	if (!isx012_client->adapter) {
		cam_err(" can't search i2c client adapter");
		return -EIO;
	}

	for (i = 0; i < size; i++) {
		if (regs[i].subaddr == 0xFFFF) {
			CAM_DEBUG("delay %dms", regs[i].value);
			msleep(regs[i].value);
		} else {
			err = isx012_i2c_write_multi(regs[i].subaddr,
						     regs[i].value,
						     regs[i].len);

			if (unlikely(err < 0)) {
				cam_err("%s: register set failed", __func__);
				return -EIO;
			}
		}
	}
#endif

	return 0;
}

#ifdef CONFIG_LOAD_FILE
void isx012_regs_table_init(void)
{
	struct file *filp;
	char *dp;
	long l;
	loff_t pos;
	int ret;
	mm_segment_t fs = get_fs();

	CAM_DEBUG(" E");

	set_fs(get_ds());

	filp = filp_open("/mnt/sdcard/isx012_regs.h", O_RDONLY, 0);

	if (IS_ERR_OR_NULL(filp)) {
		cam_err(" file open error");
		return PTR_ERR(filp);
	}

	l = filp->f_path.dentry->d_inode->i_size;
	CAM_DEBUG(" l = %ld", l);
	dp = vmalloc(l);
	if (dp == NULL) {
		cam_err(" Out of Memory");
		filp_close(filp, current->files);
	}

	pos = 0;
	memset(dp, 0, l);
	ret = vfs_read(filp, (char __user *)dp, l, &pos);

	if (ret != l) {
		cam_err(" Failed to read file ret = %d", ret);
		vfree(dp);
		filp_close(filp, current->files);
		return -EINVAL;
	}

	filp_close(filp, current->files);

	set_fs(fs);

	isx012_regs_table = dp;

	isx012_regs_table_size = l;

	*((isx012_regs_table + isx012_regs_table_size) - 1) = '\0';

	CAM_DEBUG(" X");
	return 0;
}

void isx012_regs_table_exit(void)
{
	CAM_DEBUG(" E");

	if (isx012_regs_table) {
		vfree(isx012_regs_table);
		isx012_regs_table = NULL;
	}

	CAM_DEBUG(" X");
}

static int isx012_define_table()
{
	char *start, *end, *reg;
	char *start_token, *reg_token, *temp;
	char reg_buf[61], temp2[61];
	char token_buf[5] = {0,};
	int token_value = 0;
	int index_1 = 0, index_2 = 0, total_index;
	int len = 0, total_len = 0;

	*(reg_buf + 60) = '\0';
	*(temp2 + 60) = '\0';
	*(token_buf + 4) = '\0';
	memset(gtable_buf, 9999, TABLE_MAX_NUM);

	CAM_DEBUG(" E");

	start = strstr(isx012_regs_table, "aeoffset_table");
	end = strstr(start, "};");

	/* Find table */
	index_2 = 0;
	while (1) {
		reg = strstr(start, "	");
		if ((reg == NULL) || (reg > end)) {
			CAM_DEBUG(" end!");
			break;
		}

		/* length cal */
		index_1 = 0;
		total_len = 0;
		temp = reg;
		if (temp != NULL)
			memcpy(temp2, (temp + 1), 60);

		start_token = strstr(temp, ",");
		while (index_1 < 10) {
			start_token = strstr(temp, ",");
			len = strcspn(temp, ",");
			total_len = total_len + len;
			temp = (temp + (len+2));
			index_1++;
		}
		total_len = total_len + 19;

		/* read table */
		if (reg != NULL) {
			memcpy(reg_buf, (reg + 1), total_len);
			start = (reg + total_len+1);
		}

		reg_token = reg_buf;

		index_1 = 0;
		start_token = strstr(reg_token, ",");
		while (index_1 < 10) {
			start_token = strstr(reg_token, ",");
			len = strcspn(reg_token, ",");
			memcpy(token_buf, reg_token, len);
			kstrtoul(token_buf, 10, &token_value);
			total_index = index_2 * 10 + index_1;
			gtable_buf[total_index] = token_value;
			index_1++;
			reg_token = (reg_token + (len + 2));
		}
		index_2++;
	}

#if FOR_DEBUG	/*Only debug*/
	index_2 = 0;
	while (index_2 < TABLE_MAX_NUM) {
		CAM_DEBUG(" [%d]%d ", index_2, gtable_buf[index_2]);
		index_2++;
	}
#endif

	CAM_DEBUG(" X");

	return 0;
}

static int isx012_define_read(char *name, int len_size)
{
	char *start, *end, *reg;
	char reg_7[7] = {0,}, reg_5[5] = {0,};
	int define_value = 0;

	*(reg_7 + 6) = '\0';
	*(reg_5 + 4) = '\0';

	CAM_DEBUG(" E");

	start = strstr(isx012_regs_table, name);
	end = strstr(start, "tuning");

	reg = strstr(start, " ");

	if ((reg == NULL) || (reg > end)) {
		printk(KERN_DEBUG "isx012_define_read error %s : ", name);
		return -1;
	}

	/* Write Value to Address */
	if (reg != NULL) {
		if (len_size == 6) {
			memcpy(reg_7, (reg + 1), len_size);
			kstrtoul(reg_7, 16, &define_value);
		} else {
			memcpy(reg_5, (reg + 1), len_size);
			kstrtoul(reg_5, 10, &define_value);
		}
	}
	CAM_DEBUG(" X (0x%x)!", define_value);

	return define_value;
}

void isx012_AEgain_offset_tuning(void)
{
	CAM_DEBUG(" E");

	gAE_OFSETVAL = isx012_define_read("AE_OFSETVAL", 4);
	gAE_MAXDIFF = isx012_define_read("AE_MAXDIFF", 4);
	printk(KERN_DEBUG "gAE_OFSETVAL = %d, gAE_MAXDIFF = %d",
			gAE_OFSETVAL, gAE_MAXDIFF);

	isx012_define_table(); /* for aeoffset_table */
}

static int isx012_write_regs_from_sd(char *name)
{
	char *start, *end, *reg, *size;
	unsigned short addr;
	unsigned int len, value;
	char reg_buf[7], data_buf1[5], data_buf2[7], len_buf[5];

	*(reg_buf + 6) = '\0';
	*(data_buf1 + 4) = '\0';
	*(data_buf2 + 6) = '\0';
	*(len_buf + 4) = '\0';

	CAM_DEBUG(" isx012_regs_table_write start!");
	CAM_DEBUG(" E string = %s", name);

	start = strstr(isx012_regs_table, name);
	end = strstr(start, "};");

	while (1) {
		/* Find Address */
		reg = strstr(start, "{0x");

		if ((reg == NULL) || (reg > end))
			break;

		/* Write Value to Address */
		if (reg != NULL) {
			memcpy(reg_buf, (reg + 1), 6);
			memcpy(data_buf2, (reg + 9), 6);
			size = strstr(data_buf2, ",");
			if (size) {	/* 1 byte write */
				memcpy(data_buf1, (reg + 9), 4);
				memcpy(len_buf, (reg + 15), 4);
				kstrtoint(reg_buf, 16, &addr);
				kstrtoint(data_buf1, 16, &value);
				kstrtoint(len_buf, 16, &len);
				if (reg)
					start = (reg + 22);
			} else {	/* 2 byte write */
				memcpy(len_buf, (reg + 17), 4);
				kstrtoint(reg_buf, 16, &addr);
				kstrtoint(data_buf2, 16, &value);
				kstrtoint(len_buf, 16, &len);
				if (reg)
					start = (reg + 24);
			}
			size = NULL;
			CAM_DEBUG(" delay 0x%04x, value 0x%04x, , len 0x%02x",
				  addr, value, len);
			if (addr == 0xFFFF)
				msleep(value);
			else
				isx012_i2c_write_multi(addr, value, len);
		}
	}

	CAM_DEBUG(" isx005_regs_table_write end!");

	return 0;
}
#endif

void isx012_set_init_mode(void)
{
	short unsigned int r_data[1] = { 0 };
	int timeout_cnt = 0;
	int retry_cnt = 200;

	config_csi2 = 0;
	g_bCameraRunning = false;

	isx012_ctrl->cam_mode = PREVIEW_MODE;
	isx012_ctrl->op_mode = CAMERA_MODE_INIT;
	isx012_ctrl->af_mode = SHUTTER_AF_MODE;
	isx012_ctrl->vtcall_mode = 0;
#ifdef CONFIG_MACH_JAGUAR
	isx012_ctrl->samsungapp = 0;
#else
	isx012_ctrl->samsungapp = 1; /* temp : SamsungApp 0, etc 1 */
#endif

	/* Lcd ON after AE, AWB operation */
	isx012_i2c_write_multi(0x0282, 0x21, 0x01);
	do {
		isx012_i2c_read(0x8A24, r_data);
		if ((r_data[0] == 0x2)
			|| (r_data[0] == 0x4)
			|| (r_data[0] == 0x6)) {
			cam_err("Entering delay awb sts :0x%x",
				r_data[0]);
			break;
		}
		mdelay(1);
	} while (timeout_cnt++ < retry_cnt);
	isx012_i2c_write_multi(0x0282, 0x20, 0x01);

	cam_err("wait %dsm delay for awb hunting",
					(timeout_cnt-1));
}

static int isx012_exif_shutter_speed(void)
{
	int err = 0;

	unsigned short shutter_speed_l = 0;
	unsigned short shutter_speed_h = 0;
	unsigned char l_data[2] = {0, 0}, h_data[2] = {0, 0};

	 /*SHT_TIME_OUT_L */
	err = isx012_i2c_read_multi(0x019C, (unsigned long *)l_data);
	if (err < 0)
		cam_err(" i2c read returned error, %d", err);
	/* SHT_TIME_OUT_H */
	err = isx012_i2c_read(0x019E, (unsigned short *)h_data);
	if (err < 0)
		cam_err(" i2c read returned error, %d", err);

	shutter_speed_l = (l_data[1] << 8 | l_data[0]);
	shutter_speed_h = (h_data[1] << 8 | h_data[0]);

	isx012_exif->shutterspeed = ((100000000 /
		(shutter_speed_l + (shutter_speed_h << 16)))+50)/100;

	return err;
}

static int isx012_exif_iso(void)
{
	int exif_iso = 0;
	int err = 0;
	short unsigned int r_data[1] = {0};
	unsigned int iso_table[19] = {25, 32, 40, 50, 64,
					80, 100, 125, 160, 200, 250,
					320, 400, 500, 640, 800, 1000,
					1250, 1600};
	err = isx012_i2c_read(0x019A, r_data);  /* ISOSENE_OUT */
	if (err < 0)
		cam_err(" i2c read returned error, %d", err);
	exif_iso = r_data[0];

	isx012_exif->iso = iso_table[exif_iso-1];
	return err;
}

static int isx012_get_exif(int exif_cmd, unsigned short value2)
{
	unsigned short val = 0;

	switch (exif_cmd) {
	case EXIF_SHUTTERSPEED:
		/* shutter speed low */
		val = isx012_exif->shutterspeed;
		break;

	case EXIF_ISO:
		val = isx012_exif->iso;
		break;

	default:
		break;
	}

	return val;
}

void isx012_get_LuxValue(void)
{
	int err = -1;
	unsigned short read_val = 0;

	err = isx012_i2c_read(0x01A5, &read_val);
	if (err < 0)
		cam_err(" i2c read returned error, %d", err);

	isx012_ctrl->lux = 0x00FF & read_val;
	CAM_DEBUG(" Lux = %d", isx012_ctrl->lux);
}

void isx012_get_LowLightCondition_Normal(void)
{
	switch (isx012_ctrl->settings.brightness) {
	case CAMERA_EV_M4:
		CAM_DEBUG(" EV_M4");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_M4)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	case CAMERA_EV_M3:
		CAM_DEBUG(" EV_M3");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_M3)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	case CAMERA_EV_M2:
		CAM_DEBUG(" EV_M2");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_M2)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	case CAMERA_EV_M1:
		CAM_DEBUG(" EV_M1");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_M1)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	case CAMERA_EV_P1:
		CAM_DEBUG(" EV_P1");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_P1)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	case CAMERA_EV_P2:
		CAM_DEBUG(" EV_P2");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_P2)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	case CAMERA_EV_P3:
		CAM_DEBUG(" EV_P3");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_P3)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	case CAMERA_EV_P4:
		CAM_DEBUG(" EV_P4");
		if (isx012_ctrl->lux >= LOWLIGHT_EV_P4)
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;

	default:
		CAM_DEBUG(" default");
#if defined(CONFIG_MACH_AEGIS2) || defined(CONFIG_MACH_EXPRESS)
		if (isx012_ctrl->lux >= LOWLIGHT_DEFAULT2)
#else
		if (isx012_ctrl->lux >= LOWLIGHT_DEFAULT)
#endif
			isx012_ctrl->lowLight = 1;
		else
			isx012_ctrl->lowLight = 0;
		break;
	}
}

void isx012_get_LowLightCondition_NIGHT(void)
{
	if (isx012_ctrl->lux >= LOWLIGHT_SCENE_NIGHT)
		isx012_ctrl->lowLight = 1;
	else
		isx012_ctrl->lowLight = 0;

	CAM_DEBUG(" %d", isx012_ctrl->lowLight);
}

static int isx012_get_LowLightCondition(void)
{
	int err = -1;
	unsigned char l_data[2] = { 0, 0 }, h_data[2] = {
	0, 0};
	unsigned int ldata_temp = 0, hdata_temp = 0;

	isx012_get_LuxValue();

	if (isx012_ctrl->settings.iso == CAMERA_ISO_MODE_AUTO) {/*Auto ISO*/
		if (isx012_ctrl->settings.scene == CAMERA_SCENE_NIGHT)
			isx012_get_LowLightCondition_NIGHT();
		else
			isx012_get_LowLightCondition_Normal();
	} else {		/*manual iso */
		/*SHT_TIME_OUT_L */
		err = isx012_i2c_read_multi(0x019C, (unsigned long *)l_data);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);
		ldata_temp = (l_data[1] << 8 | l_data[0]);

		/*SHT_TIME_OUT_H */
		err = isx012_i2c_read(0x019E, (unsigned short *)h_data);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);
		hdata_temp = (h_data[1] << 8 | h_data[0]);

		isx012_ctrl->lux = (h_data[1] << 24 | h_data[0] << 16 |
				  l_data[1] << 8 | l_data[0]);

		switch (isx012_ctrl->settings.iso) {
		case CAMERA_ISO_MODE_100:
			if (isx012_ctrl->lux >= LOWLIGHT_ISO100)
				isx012_ctrl->lowLight = 1;
			else
				isx012_ctrl->lowLight = 0;
			break;

		case CAMERA_ISO_MODE_200:
			if (isx012_ctrl->lux >= LOWLIGHT_ISO200)
				isx012_ctrl->lowLight = 1;
			else
				isx012_ctrl->lowLight = 0;
			break;

		case CAMERA_ISO_MODE_400:
			if (isx012_ctrl->lux >= LOWLIGHT_ISO400)
				isx012_ctrl->lowLight = 1;
			else
				isx012_ctrl->lowLight = 0;
			break;

		default:
			break;

		}
	}

	CAM_DEBUG(" lowLight:%d", isx012_ctrl->lowLight);

	return err;
}

void isx012_mode_transtion_OM(void)
{
	int count = 0;
	int status = 0;
	int om_state = 0;

	for (count = 0; count < 100; count++) {
		isx012_i2c_read(0x000E, (unsigned short *)&status);

		if ((status & 0x1) == 0x1) {
			om_state = 1;
			break;
		}
		usleep(1000);
	}

	if (!om_state)
		cam_err("operating mode error");

	for (count = 0; count < 100; count++) {
		isx012_i2c_write_multi(0x0012, 0x01, 0x01);
		isx012_i2c_read(0x000E, (unsigned short *)&status);

		if ((status & 0x1) == 0x0)
			break;

		usleep(1000);
	}
}

void isx012_mode_transtion_CM(void)
{
	int count = 0;
	int status = 0;
	int cm_state = 0;

	CAM_DEBUG(" E");

	for (count = 0; count < 200; count++) {
		isx012_i2c_read(0x000E, (unsigned short *)&status);

		if ((status & 0x2) == 0x2) {
			cm_state = 1;
			break;
		}
		usleep(10*1000);
	}

	if (!cm_state)
		cam_err("change mode error");

	for (count = 0; count < 200; count++) {
		isx012_i2c_write_multi(0x0012, 0x02, 0x01);
		isx012_i2c_read(0x000E, (unsigned short *)&status);

		if ((status & 0x2) == 0x0)
			break;
		usleep(10*1000);
	}
	CAM_DEBUG(" X");
}

void isx012_wait_for_VINT(void)
{
	int count = 0;
	int status = 0;
	int vint_state = 0;

	CAM_DEBUG(" E");

	for (count = 0; count < 100; count++) {
		isx012_i2c_read(0x000E, (unsigned short *)&status);

		if ((status & 0x20) == 0x20) {
			vint_state = 1;
			break;
		}
		usleep(1000);
	}

	if (!vint_state)
		cam_err("vint state error");

	for (count = 0; count < 100; count++) {
		isx012_i2c_write_multi(0x0012, 0x20, 0x01);
		isx012_i2c_read(0x000E, (unsigned short *)&status);

		if ((status & 0x20) == 0x00)
			break;

		usleep(1000);
	}
	CAM_DEBUG(" X");
}


void isx012_Sensor_Calibration(void)
{
	int err = 0;
	int status = 0;
	int temp = 0;

	CAM_DEBUG(" E");

	/* Read OTP1 */
	err = isx012_i2c_read(0x004F, (unsigned short *)&status);
	if (err < 0)
		cam_err(" i2c read returned error, %d", err);

	if ((status & 0x1) == 0x1) {
		/* Read ShadingTable */
		err = isx012_i2c_read(0x005C, (unsigned short *)&status);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);
		temp = (status & 0x03C0) >> 6;

		/* Write Shading Table */
		switch (temp) {
		case 0x0:
			ISX012_BURST_WRITE_LIST(isx012_Shading_0);
			break;
		case 0x1:
			ISX012_BURST_WRITE_LIST(isx012_Shading_1);
			break;
		case 0x2:
			ISX012_BURST_WRITE_LIST(isx012_Shading_2);
			break;
		default:
			break;
		}

		/* Write NorR */
		err = isx012_i2c_read(0x0054, (unsigned short *)&status);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);
		temp = status & 0x3FFF;
		isx012_i2c_write_multi(0x6804, temp, 0x02);

		/* Write NorB */
		err = isx012_i2c_read(0x0056, (unsigned short *)&status);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);
		temp = status & 0x3FFF;
		isx012_i2c_write_multi(0x6806, temp, 0x02);

		/* Write PreR */
		err = isx012_i2c_read(0x005A, (unsigned short *)&status);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);
		temp = (status & 0x0FFC) >> 2;
		isx012_i2c_write_multi(0x6808, temp, 0x02);

		/* Write PreB */
		err = isx012_i2c_read(0x005B, (unsigned short *)&status);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);
		temp = (status & 0x3FF0) >> 4;
		isx012_i2c_write_multi(0x680A, temp, 0x02);
	} else {
		/* Read OTP0 */
		err = isx012_i2c_read(0x0040, (unsigned short *)&status);
		if (err < 0)
			cam_err(" i2c read returned error, %d", err);

		if ((status & 0x1) == 0x1) {
			/* Read ShadingTable */
			err = isx012_i2c_read(0x004D,
					(unsigned short *)&status);
			if (err < 0)
				cam_err(" i2c read returned error, %d", err);
			temp = (status & 0x03C0) >> 6;

			/* Write Shading Table */
			switch (temp) {
			case 0x0:
				ISX012_BURST_WRITE_LIST(isx012_Shading_0);
				break;
			case 0x1:
				ISX012_BURST_WRITE_LIST(isx012_Shading_1);
				break;
			case 0x2:
				ISX012_BURST_WRITE_LIST(isx012_Shading_2);
				break;
			default:
				break;
			}

			/* Write NorR */
			err = isx012_i2c_read(0x0045,
					(unsigned short *)&status);
			if (err < 0)
				cam_err(" i2c read returned error, %d", err);
			temp = status & 0x3FFF;
			err = isx012_i2c_write_multi(0x6804, temp, 0x02);
			if (err < 0)
				cam_err(" i2c read returned error, %d", err);

			/* Write NorB */
			err = isx012_i2c_read(0x0047,
					(unsigned short *)&status);
			if (err < 0)
				cam_err(" i2c read returned error, %d", err);
			temp = status & 0x3FFF;
			isx012_i2c_write_multi(0x6806, temp, 0x02);

			/* Write PreR */
			err = isx012_i2c_read(0x004B,
					(unsigned short *)&status);
			if (err < 0)
				cam_err(" i2c read returned error, %d", err);
			temp = (status & 0x0FFC) >> 2;
			isx012_i2c_write_multi(0x6808, temp, 0x02);

			/* Write PreB */
			err = isx012_i2c_read(0x004C,
					(unsigned short *)&status);
			if (err < 0)
				cam_err(" i2c read returned error, %d", err);
			temp = (status & 0x3FF0) >> 4;
			isx012_i2c_write_multi(0x680A, temp, 0x02);
		} else
		ISX012_BURST_WRITE_LIST(isx012_Shading_Nocal);
	}

	CAM_DEBUG(" X");

}

static int isx012_get_af_result(void)
{
	int err = 0;
	int ret = 0;
	int status = 0;

	err = isx012_i2c_read(0x8B8B,
			(unsigned short *)&status);
	if (err < 0)
		cam_err(" i2c read returned error, %d", err);
	if ((status & 0x1) == 0x1) {
		CAM_DEBUG(" AF success");
		ret = 1;
	} else if ((status & 0x1) == 0x0) {
		CAM_DEBUG(" AF fail");
		ret = 2;
	} else {
		CAM_DEBUG(" AF move");
		ret = 0;
	}

	return ret;
}

static int calculate_AEgain_offset(uint16_t ae_auto,
		uint16_t ae_now, int16_t ersc_auto, int16_t ersc_now)
{
	int err = -EINVAL;
	int16_t aediff, aeoffset;

	CAM_DEBUG(" E");

	/*AE_Gain_Offset = Target - ERRSCL_NOW*/
	aediff = (ae_now + ersc_now) - (ae_auto + ersc_auto);

	if (aediff < 0)
		aediff = 0;

#ifdef CONFIG_LOAD_FILE
	if (ersc_now < 0) {
		if (aediff >= gAE_MAXDIFF)
			aeoffset = -gAE_OFSETVAL - ersc_now;
		else
			aeoffset = -gtable_buf[aediff / 10] - ersc_now;
	} else {
		if (aediff >= gAE_MAXDIFF)
			aeoffset = -gAE_OFSETVAL;
		else
			aeoffset = -gtable_buf[aediff / 10];
	}
#else
	if (ersc_now < 0) {
		if (aediff >= AE_MAXDIFF)
			aeoffset = -AE_OFSETVAL - ersc_now;
		else
			aeoffset = -aeoffset_table[aediff / 10] - ersc_now;
	} else {
		if (aediff >= AE_MAXDIFF)
			aeoffset = -AE_OFSETVAL;
		else
			aeoffset = -aeoffset_table[aediff / 10];
	}
#endif

	/*SetAE Gain offset*/
	err = isx012_i2c_write_multi(CAP_GAINOFFSET, aeoffset, 2);

	return err;
}

static int isx012_get_af_status(void)
{
	uint16_t ae_data[1] = { 0 };
	int16_t ersc_data[1] = { 0 };
	int16_t aescl_data[1] = { 0 };
	int16_t ae_scl = 0;

	CAM_DEBUG(" E");

	if (isx012_ctrl->af_status == 1) {
		isx012_i2c_write_multi(0x0012, 0x10, 0x01);
		isx012_ctrl->af_status = isx012_get_af_result();

		if (((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
		     && (isx012_ctrl->lowLight))
		    || (isx012_ctrl->flash_mode == CAMERA_FLASH_ON)) {
			/*Read AE scale - ae_now, ersc_now*/
			isx012_i2c_read(0x01CC, ersc_data);
			g_ersc_now = ersc_data[0];
			isx012_i2c_read(0x01D0, ae_data);
			g_ae_now = ae_data[0];
			isx012_i2c_read(0x8BC0, aescl_data);
			ae_scl = aescl_data[0];
		}

		CAM_DEBUG(" Single AF off");
		if (isx012_ctrl->af_mode == SHUTTER_AF_MODE)
			ISX012_WRITE_LIST(isx012_AF_SAF_OFF);
		else
			ISX012_WRITE_LIST(isx012_AF_TouchSAF_OFF);

		/*wait 1V time (66ms)*/
		msleep(66);

		/*AE SCL*/
		if (((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
			&& (isx012_ctrl->lowLight))
		    || (isx012_ctrl->flash_mode == CAMERA_FLASH_ON)) {
			ae_scl = ae_scl - 4802;
			isx012_i2c_write_multi(0x5E02, ae_scl, 0x02);
			calculate_AEgain_offset(g_ae_auto, g_ae_now,
					    g_ersc_auto, g_ersc_now);
			isx012_set_flash(FLASH_OFF);
		}
	}

	CAM_DEBUG(" X %d", isx012_ctrl->af_status);

	return isx012_ctrl->af_status;
}

static int isx012_get_camcorder_af_status(void)
{
	uint16_t ae_data[1] = { 0 };
	int16_t ersc_data[1] = { 0 };
	int16_t aescl_data[1] = { 0 };
	int16_t ae_scl = 0;

	CAM_DEBUG(" E");

	if (isx012_ctrl->af_status == 1) {
		isx012_i2c_write_multi(0x0012, 0x10, 0x01);
		isx012_ctrl->af_status = isx012_get_af_result();

		if (((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
			 && (isx012_ctrl->lowLight))
			|| (isx012_ctrl->flash_mode == CAMERA_FLASH_ON)) {
			/*Read AE scale - ae_now, ersc_now*/
			isx012_i2c_read(0x01CC, ersc_data);
			g_ersc_now = ersc_data[0];
			isx012_i2c_read(0x01D0, ae_data);
			g_ae_now = ae_data[0];
			isx012_i2c_read(0x8BC0, aescl_data);
			ae_scl = aescl_data[0];
		}

		CAM_DEBUG(" Single AF off");
		ISX012_WRITE_LIST(isx012_AF_TouchSAF_OFF);

		/*wait 1V time (66ms)*/
		msleep(66);

		/*AE SCL*/
		if (isx012_ctrl->flash_mode == CAMERA_FLASH_ON) {
			ae_scl = ae_scl - 4802;
			isx012_i2c_write_multi(0x5E02, ae_scl, 0x02);
			calculate_AEgain_offset(g_ae_auto, g_ae_now,
						g_ersc_auto, g_ersc_now);
		}
	}

	CAM_DEBUG(" X %d", isx012_ctrl->af_status);

	return isx012_ctrl->af_status;
}

static int isx012_get_sensor_af_status(void)
{
	int ret = 0;
	int status = 0;
	int err = 0;
	isx012_ctrl->af_status = 0;

	err = isx012_i2c_read(0x8B8A, (unsigned short *)&status);

	if ((status & 0x8) == 0x8) {
		isx012_ctrl->af_status = 1;
		if (isx012_ctrl->cam_mode == MOVIE_MODE)
			ret = isx012_get_camcorder_af_status();
		else
			ret = isx012_get_af_status();
	}

	return ret;
}

static int isx012_get_flash_status(void)
{
	int flash_status = 0;

	if (((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
		&& (isx012_ctrl->lowLight))
		|| (isx012_ctrl->flash_mode == CAMERA_FLASH_ON)) {
		flash_status = 1;
	}

	CAM_DEBUG(" %d", flash_status);

	return flash_status;
}

static void isx012_set_flash(int mode)
{
	int i = 0;

	CAM_DEBUG(" %d", mode);

	if (torchonoff > 0) {
		CAM_DEBUG(" [TorchOnOFF = %d] Do not control flash!",
			torchonoff);
		return;
	}
#if defined(CONFIG_MACH_EXPRESS)
	if (mode == MOVIE_FLASH) {
		CAM_DEBUG(" MOVIE FLASH ON");
		if (system_rev >= BOARD_REV05) {
			if ((system_rev == BOARD_REV05)\
				|| (system_rev == BOARD_REV06))
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_set, 0);
		} else {
			gpio_set_value_cansleep(isx012_ctrl->sensordata->
				sensor_platform_info->flash_en, 0);
		}
		if (system_rev >= BOARD_REV05) {
			for (i = 1; i > 0; i--) {
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_en, 0);
				udelay(1);
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_en, 1);
				udelay(1);
			}
		} else {
			for (i = FLASH_PULSE_CNT; i > 0; i--) {
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_set, 1);
				udelay(1);
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_set, 0);
				udelay(1);
			}
			gpio_set_value_cansleep(isx012_ctrl->sensordata->
				sensor_platform_info->flash_set, 1);
			usleep(2 * 1000);
		}
	} else if (mode == CAPTURE_FLASH) {
		if (system_rev >= BOARD_REV05) {
			CAM_DEBUG(" CAPTURE FLASH ON EXPRESS REV 05");
			if ((system_rev == BOARD_REV05)\
				|| (system_rev == BOARD_REV06))
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_set, 0);
			for (i = 11 ; i > 0; i--) {
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_en, 0);
				udelay(1);
				gpio_set_value_cansleep(isx012_ctrl->
					sensordata->sensor_platform_info->
						flash_en, 1);
				udelay(1);
			}
			usleep(2 * 1000);
		} else {
			CAM_DEBUG(" CAPTURE FLASH ON ");
			gpio_set_value_cansleep(isx012_ctrl->sensordata->
				sensor_platform_info->flash_en, 1);
			gpio_set_value_cansleep(isx012_ctrl->sensordata->
				sensor_platform_info->flash_set, 0);
		}
	} else {
		if (system_rev >= BOARD_REV07) {
			CAM_DEBUG(" FLASH OFF");
			gpio_set_value_cansleep(isx012_ctrl->
				sensordata->sensor_platform_info->
					flash_en, 0);
		} else {
			CAM_DEBUG(" FLASH OFF");
			gpio_set_value_cansleep(isx012_ctrl->
				sensordata->sensor_platform_info->
					flash_en, 0);
			gpio_set_value_cansleep(isx012_ctrl->
				sensordata->sensor_platform_info->
					flash_set, 0);
		}
	}
#else
	if (mode == MOVIE_FLASH) {
		CAM_DEBUG(" MOVIE FLASH ON");
		gpio_set_value_cansleep(isx012_ctrl->sensordata->
					sensor_platform_info->flash_en, 0);

		for (i = FLASH_PULSE_CNT; i > 0; i--) {
			gpio_set_value_cansleep(isx012_ctrl->sensordata->
						sensor_platform_info->flash_set,
						1);
			udelay(1);
			gpio_set_value_cansleep(isx012_ctrl->sensordata->
						sensor_platform_info->flash_set,
						0);
			udelay(1);
		}
		gpio_set_value_cansleep(isx012_ctrl->sensordata->
					sensor_platform_info->flash_set, 1);
		usleep(2 * 1000);
	} else if (mode == CAPTURE_FLASH) {
		CAM_DEBUG(" CAPTURE FLASH ON");
		gpio_set_value_cansleep(isx012_ctrl->sensordata->
					sensor_platform_info->flash_en, 1);
		gpio_set_value_cansleep(isx012_ctrl->sensordata->
					sensor_platform_info->flash_set, 0);
	}

	else {
		CAM_DEBUG(" FLASH OFF");
		gpio_set_value_cansleep(isx012_ctrl->sensordata->
					sensor_platform_info->flash_en, 0);
		gpio_set_value_cansleep(isx012_ctrl->sensordata->
					sensor_platform_info->flash_set, 0);
	}
#endif
}

void isx012_set_preview_size(int32_t index)
{
	CAM_DEBUG(" %d -> %d", isx012_ctrl->settings.preview_size_idx, index);

	switch (index) {
	case PREVIEW_SIZE_QCIF:
		ISX012_WRITE_LIST(isx012_176_Preview);
		break;

	case PREVIEW_SIZE_MMS:
		ISX012_WRITE_LIST(isx012_528_Preview);
		break;

	case PREVIEW_SIZE_D1:
		ISX012_WRITE_LIST(isx012_720_Preview);
		break;

	case PREVIEW_SIZE_WVGA:
		ISX012_WRITE_LIST(isx012_800_Preview);
		break;

	case PREVIEW_SIZE_HD:
		ISX012_WRITE_LIST(isx012_1280_Preview);
		break;

	case PREVIEW_SIZE_FHD:
		ISX012_WRITE_LIST(isx012_1920_Preview);
		break;

	default:
		ISX012_WRITE_LIST(isx012_640_Preview);
		break;
	}

	isx012_ctrl->settings.preview_size_idx = index;
}

static void isx012_set_af_mode(int mode)
{
	CAM_DEBUG(" %d", mode);

	switch (mode) {
	case CAMERA_AF_AUTO:
		ISX012_WRITE_LIST(isx012_AF_Macro_OFF);
		if (isx012_ctrl->settings.focus_status != IN_AUTO_MODE)
			ISX012_WRITE_LIST(isx012_AF_ReStart);
		isx012_ctrl->settings.focus_status = IN_AUTO_MODE;
		break;

	case CAMERA_AF_MACRO:
		ISX012_WRITE_LIST(isx012_AF_Macro_ON);
		if (isx012_ctrl->settings.focus_status != IN_MACRO_MODE)
			ISX012_WRITE_LIST(isx012_AF_ReStart);
		isx012_ctrl->settings.focus_status = IN_MACRO_MODE;
		break;

	default:
		cam_err(" set default AF auto.", mode);
		ISX012_WRITE_LIST(isx012_AF_Macro_OFF);
		if (isx012_ctrl->settings.focus_status != IN_AUTO_MODE)
			ISX012_WRITE_LIST(isx012_AF_ReStart);
		isx012_ctrl->settings.focus_status = IN_AUTO_MODE;
		break;
	}

	isx012_ctrl->settings.focus_mode = mode;
}

static int isx012_set_af_stop(int af_check)
{
	CAM_DEBUG(" %d", af_check);

	if (af_check == 1) {
		if (isx012_ctrl->settings.focus_mode == CAMERA_AF_MACRO) {
			CAM_DEBUG(" AF cancel : macro_ON ");
			ISX012_WRITE_LIST(isx012_AF_Cancel_Macro_ON);
		} else {
			CAM_DEBUG(" AF cancel : macro_OFF ");
			ISX012_WRITE_LIST(isx012_AF_Cancel_Macro_OFF);
		}
	}

	if ((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
	    || (isx012_ctrl->flash_mode == CAMERA_FLASH_ON)) {
		isx012_set_flash(FLASH_OFF);
		ISX012_BURST_WRITE_LIST(isx012_Flash_OFF);

		if (isx012_ctrl->awb_mode == CAMERA_WHITE_BALANCE_AUTO) {
			CAM_DEBUG(" AWB mode : AUTO");
			isx012_i2c_write_multi(0x0282, 0x20, 0x01);
		}
		isx012_i2c_write_multi(0x8800, 0x01, 0x01);
	}

	isx012_set_af_mode(isx012_ctrl->settings.focus_mode);

	isx012_ctrl->af_mode = SHUTTER_AF_MODE;

	return 0;
}

static int isx012_set_camcorder_af_stop(int af_check)
{
	CAM_DEBUG(" %d", af_check);

	if (af_check == 1) {
		CAM_DEBUG(" AF cancel : macro_OFF ");
		ISX012_WRITE_LIST(isx012_AF_Cancel_Macro_OFF);
	}

	if (isx012_ctrl->flash_mode == CAMERA_FLASH_ON) {
		ISX012_BURST_WRITE_LIST(isx012_Flash_OFF);

		if (isx012_ctrl->awb_mode == CAMERA_WHITE_BALANCE_AUTO) {
			CAM_DEBUG(" AWB mode : AUTO");
			isx012_i2c_write_multi(0x0282, 0x20, 0x01);
		}
		isx012_i2c_write_multi(0x8800, 0x01, 0x01);
	}

	return 0;
}

void isx012_set_frame_rate(int32_t fps)
{
	CAM_DEBUG(" %d", fps);

	switch (fps) {
	case 15:
		ISX012_WRITE_LIST(isx012_FRAME_15FPS_Setting);
		break;

	case 12:
		ISX012_WRITE_LIST(isx012_FRAME_12FPS_Setting);
		break;

	case 10:
		ISX012_WRITE_LIST(isx012_FRAME_10FPS_Setting);
		break;

	case 7:
		ISX012_WRITE_LIST(isx012_FRAME_07FPS_Setting);
		break;

	default:
		break;
	}

	isx012_ctrl->settings.fps = fps;
}


void isx012_set_preview(void)
{
	if ((isx012_ctrl->settings.scenemode == CAMERA_SCENE_NIGHT)
		&& (isx012_ctrl->lowLight)) {
		CAM_DEBUG(" Lowlux_Nightshot - 500ms delay");
		ISX012_WRITE_LIST(isx012_Lowlux_Night_Reset);
		msleep(500);
	}

	/*set preview size*/
	isx012_set_preview_size(isx012_ctrl->settings.preview_size_idx);

	/*start preview*/
	if (isx012_ctrl->cam_mode == MOVIE_MODE) {
		CAM_DEBUG(" ** Camcorder Mode");
		ISX012_BURST_WRITE_LIST(isx012_Camcorder_Mode_ON);
		ISX012_WRITE_LIST(isx012_Preview_Mode);
		isx012_ctrl->op_mode = CAMERA_MODE_RECORDING;
	} else {
		CAM_DEBUG(" ** Preview Mode");
		ISX012_WRITE_LIST(isx012_Preview_Mode);
		isx012_ctrl->op_mode = CAMERA_MODE_PREVIEW;
	}

	isx012_mode_transtion_CM();

	if (isx012_ctrl->settings.scenemode == CAMERA_SCENE_FIRE) {
		CAM_DEBUG(" firework - 1000ms delay");
		msleep(1000);
	}
}

void isx012_set_capture(void)
{
	bool bCaptureFlash = 0;
	int timeout_cnt = 0;
	short unsigned int r_data[1] = { 0 };

	CAM_DEBUG(" E");

	ISX012_WRITE_LIST(isx012_Capture_SizeSetting);

	if (((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
		&& (isx012_ctrl->lowLight))
		|| (isx012_ctrl->flash_mode == CAMERA_FLASH_ON))
		bCaptureFlash = 1;
	else
		bCaptureFlash = 0;

	if (g_bPreFlash != bCaptureFlash) {
		CAM_DEBUG(" preFlash and Full Flash mode are not same. "
					"Fast AE, AWB");
		if (bCaptureFlash)
			isx012_set_flash(CAPTURE_FLASH);

		isx012_i2c_write_multi(0x0181, 0x01, 0x01);
		isx012_i2c_write_multi(0x00B2, 0x03, 0x01);
		isx012_i2c_write_multi(0x00B3, 0x03, 0x01);
		isx012_i2c_write_multi(0x0081, 0x01, 0x01);

		do {
			isx012_i2c_read(0x0080, r_data);
			if (r_data[0] == 0x1)
				break;
			mdelay(1);
		} while (timeout_cnt++ < ISX012_DELAY_RETRIES);

		timeout_cnt = 0;
		do {
			isx012_i2c_read(0x01B0, r_data);
			if (r_data[0] == 0x0)
				break;
			mdelay(1);
		} while (timeout_cnt++ < ISX012_DELAY_RETRIES);
	} else {
		if (((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
			&& (isx012_ctrl->lowLight))
			|| (isx012_ctrl->flash_mode == CAMERA_FLASH_ON)) {
			if (isx012_ctrl->af_mode == TOUCH_AF_MODE) {
				CAM_DEBUG(" [for tuning] touchaf :%d",
					isx012_ctrl->af_mode);
				isx012_i2c_write_multi(0x0294, 0x02, 0x01);
				isx012_i2c_write_multi(0x0297, 0x02, 0x01);
				isx012_i2c_write_multi(0x029A, 0x02, 0x01);
				isx012_i2c_write_multi(0x029E, 0x02, 0x01);

				/*wait 1V time (66ms)*/
				msleep(66);
			}
			isx012_set_flash(CAPTURE_FLASH);
		}
	}

	if ((isx012_ctrl->settings.scenemode == CAMERA_SCENE_NIGHT)
	    && (isx012_ctrl->lowLight))
		ISX012_WRITE_LIST(isx012_Lowlux_Night_Capture_Mode);
	else
		ISX012_WRITE_LIST(isx012_Capture_Mode);

	/*wait for complete capture mode*/
	isx012_mode_transtion_CM();
	isx012_wait_for_VINT();

	if (((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO)
	     && (isx012_ctrl->lowLight))
	    || (isx012_ctrl->flash_mode == CAMERA_FLASH_ON)) {
		short unsigned int r_data[1] = { 0 };
		int timeout_cnt = 0;

	/*wait 1V time (210ms)*/
#if defined(CONFIG_MACH_AEGIS2)
		msleep(260);
#else
		msleep(210);
#endif

		do {
			isx012_i2c_read(0x8A24, r_data);
			/*Flash Saturation*/
			if ((r_data[0] == 0x2)
				|| (r_data[0] == 0x4)
				|| (r_data[0] == 0x6)) {
				cam_err("Entering delay awb sts :0x%x",
					r_data[0]);
				break;
			}
			mdelay(1);
		} while (timeout_cnt++ < ISX012_DELAY_RETRIES);
	}

	/*Read exif informatio*/
	isx012_exif_iso();
	isx012_exif_shutter_speed();

	/* reset AF mode */
	isx012_ctrl->af_mode = SHUTTER_AF_MODE;
	isx012_ctrl->op_mode = CAMERA_MODE_CAPTURE;

	CAM_DEBUG(" X");
}

#if defined(CONFIG_ISX012) && defined(CONFIG_S5K8AAY) /* JAGUAR */
static int32_t isx012_sensor_setting(int update_type, int rt)
{
	int32_t rc = 0;
	struct msm_camera_csid_params isx012_csid_params;
	struct msm_camera_csiphy_params isx012_csiphy_params;

	switch (update_type) {
	case REG_INIT:
		break;

	case UPDATE_PERIODIC:
		if (rt == RES_PREVIEW || rt == RES_CAPTURE) {
			struct msm_camera_csid_vc_cfg isx012_vccfg[] = {
				{0, 0x1E, CSI_DECODE_8BIT},
				{1, CSI_EMBED_DATA, CSI_DECODE_8BIT},
			};

			CAM_DEBUG(" UPDATE_PERIODIC");

			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
					   NOTIFY_ISPIF_STREAM,
					   (void *)ISPIF_STREAM(PIX_0,
							ISPIF_OFF_IMMEDIATELY));
			msleep(30);

			isx012_csid_params.lane_cnt = 2;
			isx012_csid_params.lane_assign = 0xe4;
			isx012_csid_params.lut_params.num_cid =
			    ARRAY_SIZE(isx012_vccfg);
			isx012_csid_params.lut_params.vc_cfg = &isx012_vccfg[0];
			isx012_csiphy_params.lane_cnt = 2;
			isx012_csiphy_params.settle_cnt = 0x14;
			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
				NOTIFY_CSID_CFG, &isx012_csid_params);
			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
				NOTIFY_CID_CHANGE, NULL);
			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
				NOTIFY_CSIPHY_CFG, &isx012_csiphy_params);
			mb();
			msleep(20);

			config_csi2 = 1;

			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
					   NOTIFY_ISPIF_STREAM,
					   (void *)ISPIF_STREAM(PIX_0,
						ISPIF_ON_FRAME_BOUNDARY));
			msleep(30);

		}
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}
#else
static int32_t isx012_sensor_setting(int update_type, int rt)
{
	int32_t rc = 0;
	struct msm_camera_csid_params isx012_csid_params;
	struct msm_camera_csiphy_params isx012_csiphy_params;

	CAM_DEBUG(" E");

	switch (update_type) {
	case REG_INIT:
		break;

	case UPDATE_PERIODIC:
		if (rt == RES_PREVIEW || rt == RES_CAPTURE) {
			struct msm_camera_csid_vc_cfg isx012_vccfg[] = {
				{0, 0x1E, CSI_DECODE_8BIT},
				{1, CSI_EMBED_DATA, CSI_DECODE_8BIT},
			};

			CAM_DEBUG(" UPDATE_PERIODIC");

			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
					   NOTIFY_ISPIF_STREAM,
					   (void *)ISPIF_STREAM(PIX_0,
							ISPIF_OFF_IMMEDIATELY));
			msleep(30);

			isx012_csid_params.lane_cnt = 2;
			isx012_csid_params.lane_assign = 0xe4;
			isx012_csid_params.lut_params.num_cid =
			    ARRAY_SIZE(isx012_vccfg);
			isx012_csid_params.lut_params.vc_cfg = &isx012_vccfg[0];
			isx012_csiphy_params.lane_cnt = 2;
			isx012_csiphy_params.settle_cnt = 0x14;
			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
				NOTIFY_CSID_CFG, &isx012_csid_params);
			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
				NOTIFY_CID_CHANGE, NULL);
			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
				NOTIFY_CSIPHY_CFG, &isx012_csiphy_params);
			mb();
			msleep(20);

			config_csi2 = 1;

			v4l2_subdev_notify(isx012_ctrl->sensor_dev,
					   NOTIFY_ISPIF_STREAM,
					   (void *)ISPIF_STREAM(PIX_0,
						ISPIF_ON_FRAME_BOUNDARY));
			msleep(30);
		}
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}
#endif

static long isx012_set_sensor_mode(int mode)
{
	CAM_DEBUG(" %d", mode);

	switch (mode) {
	case SENSOR_PREVIEW_MODE:
	case SENSOR_VIDEO_MODE:
		if (isx012_ctrl->op_mode ==
			CAMERA_MODE_CAPTURE)
			isx012_set_af_stop(0);
		isx012_set_preview();
		break;

	case SENSOR_SNAPSHOT_MODE:
	case SENSOR_RAW_SNAPSHOT_MODE:
		isx012_set_capture();
		break;

	default:
		return 0;
	}
	return 0;
}

static int isx012_set_effect(int effect)
{
	CAM_DEBUG(" %d", effect);

	switch (effect) {
	case CAMERA_EFFECT_OFF:
		ISX012_WRITE_LIST(isx012_Effect_Normal);
		break;

	case CAMERA_EFFECT_MONO:
		ISX012_WRITE_LIST(isx012_Effect_Black_White);
		break;

	case CAMERA_EFFECT_NEGATIVE:
		ISX012_WRITE_LIST(isx012_Effect_Negative);
		break;

	case CAMERA_EFFECT_SEPIA:
		ISX012_WRITE_LIST(isx012_Effect_Sepia);
		break;

	default:
		ISX012_WRITE_LIST(isx012_Effect_Normal);
		return 0;
	}

	isx012_ctrl->settings.effect = effect;

	return 0;
}

static int isx012_set_whitebalance(int wb)
{
	CAM_DEBUG(" %d", wb);

	switch (wb) {
	case CAMERA_WHITE_BALANCE_AUTO:
		ISX012_WRITE_LIST(isx012_WB_Auto);
		break;

	case CAMERA_WHITE_BALANCE_INCANDESCENT:
		ISX012_WRITE_LIST(isx012_WB_Incandescent);
		break;

	case CAMERA_WHITE_BALANCE_FLUORESCENT:
		ISX012_WRITE_LIST(isx012_WB_Fluorescent);
		break;

	case CAMERA_WHITE_BALANCE_DAYLIGHT:
		ISX012_WRITE_LIST(isx012_WB_Daylight);
		break;

	case CAMERA_WHITE_BALANCE_CLOUDY_DAYLIGHT:
		ISX012_WRITE_LIST(isx012_WB_Cloudy);
		break;

	default:
		return 0;
	}

	isx012_ctrl->settings.wb = wb;

	return 0;
}

static void isx012_check_dataline(int val)
{
	if (val) {
		CAM_DEBUG(" DTP ON");
		ISX012_WRITE_LIST(isx012_DTP_Init);
		isx012_ctrl->dtpTest = 1;

	} else {
		CAM_DEBUG(" DTP OFF");
		ISX012_WRITE_LIST(isx012_DTP_Stop);
		isx012_ctrl->dtpTest = 0;
	}
}

static void isx012_set_ae_awb(int lock)
{
	if (lock) {
		CAM_DEBUG(" AE_AWB LOCK");
		ISX012_WRITE_LIST(isx012_ae_awb_lock);

	} else {
		CAM_DEBUG(" AE_AWB UNLOCK");
		ISX012_WRITE_LIST(isx012_ae_awb_unlock);
	}
}

static void isx012_set_ev(int ev)
{
	CAM_DEBUG(" %d", ev);

	switch (ev) {
	case CAMERA_EV_M4:
		ISX012_WRITE_LIST(isx012_ExpSetting_M4Step);
		break;

	case CAMERA_EV_M3:
		ISX012_WRITE_LIST(isx012_ExpSetting_M3Step);
		break;

	case CAMERA_EV_M2:
		ISX012_WRITE_LIST(isx012_ExpSetting_M2Step);
		break;

	case CAMERA_EV_M1:
		ISX012_WRITE_LIST(isx012_ExpSetting_M1Step);
		break;

	case CAMERA_EV_DEFAULT:
		ISX012_WRITE_LIST(isx012_ExpSetting_Default);
		break;

	case CAMERA_EV_P1:
		ISX012_WRITE_LIST(isx012_ExpSetting_P1Step);
		break;

	case CAMERA_EV_P2:
		ISX012_WRITE_LIST(isx012_ExpSetting_P2Step);
		break;

	case CAMERA_EV_P3:
		ISX012_WRITE_LIST(isx012_ExpSetting_P3Step);
		break;

	case CAMERA_EV_P4:
		ISX012_WRITE_LIST(isx012_ExpSetting_P4Step);
		break;
	default:
		break;
	}

	isx012_ctrl->settings.brightness = ev;
}

static void isx012_set_scene_mode(int mode)
{
	CAM_DEBUG(" %d", mode);

	switch (mode) {
	case CAMERA_SCENE_AUTO:
		ISX012_WRITE_LIST(isx012_Scene_Default);
		break;

	case CAMERA_SCENE_LANDSCAPE:
		ISX012_WRITE_LIST(isx012_Scene_Landscape);
		break;

	case CAMERA_SCENE_DAWN:
		ISX012_WRITE_LIST(isx012_Scene_Dawn);
		break;

	case CAMERA_SCENE_BEACH:
		ISX012_WRITE_LIST(isx012_Scene_Beach_Snow);
		break;

	case CAMERA_SCENE_SUNSET:
		ISX012_WRITE_LIST(isx012_Scene_Sunset);
		break;

	case CAMERA_SCENE_NIGHT:
		ISX012_WRITE_LIST(isx012_Scene_Nightmode);
		break;

	case CAMERA_SCENE_PORTRAIT:
		ISX012_WRITE_LIST(isx012_Scene_Portrait);
		break;

	case CAMERA_SCENE_AGAINST_LIGHT:
		ISX012_WRITE_LIST(isx012_Scene_Backlight);
		break;

	case CAMERA_SCENE_SPORT:
		ISX012_WRITE_LIST(isx012_Scene_Sports);
		break;

	case CAMERA_SCENE_FALL:
		ISX012_WRITE_LIST(isx012_Scene_Fall_Color);
		break;

	case CAMERA_SCENE_TEXT:
		ISX012_WRITE_LIST(isx012_Scene_Document);
		break;

	case CAMERA_SCENE_CANDLE:
		ISX012_WRITE_LIST(isx012_Scene_Candle_Light);
		break;

	case CAMERA_SCENE_FIRE:
		ISX012_WRITE_LIST(isx012_Scene_Fireworks);
		break;

	case CAMERA_SCENE_PARTY:
		ISX012_WRITE_LIST(isx012_Scene_Party_Indoor);
		break;

	default:
		break;
	}

	if (isx012_ctrl->settings.focus_mode == CAMERA_SCENE_TEXT
		&& mode != CAMERA_SCENE_TEXT) {
		CAM_DEBUG(" isx012_AF_Macro_OFF");
		ISX012_WRITE_LIST(isx012_AF_Macro_OFF);
		ISX012_WRITE_LIST(isx012_AF_ReStart);
	} else if (isx012_ctrl->settings.focus_mode != CAMERA_SCENE_TEXT
		&& mode == CAMERA_SCENE_TEXT) {
		CAM_DEBUG(" isx012_AF_Macro_ON");
		ISX012_WRITE_LIST(isx012_AF_Macro_ON);
		ISX012_WRITE_LIST(isx012_AF_ReStart);
	}

	isx012_ctrl->settings.scenemode = mode;
}

static void isx012_set_iso(int iso)
{
	CAM_DEBUG(" %d", iso);

	switch (iso) {
	case CAMERA_ISO_MODE_AUTO:
		ISX012_WRITE_LIST(isx012_ISO_Auto);
		break;

	case CAMERA_ISO_MODE_50:
		ISX012_WRITE_LIST(isx012_ISO_50);
		break;

	case CAMERA_ISO_MODE_100:
		ISX012_WRITE_LIST(isx012_ISO_100);
		break;

	case CAMERA_ISO_MODE_200:
		ISX012_WRITE_LIST(isx012_ISO_200);
		break;

	case CAMERA_ISO_MODE_400:
		ISX012_WRITE_LIST(isx012_ISO_400);
		break;

	case CAMERA_ISO_MODE_800:
		ISX012_WRITE_LIST(isx012_ISO_800);
		break;

	default:
		break;
	}

	isx012_ctrl->settings.iso = iso;
}

static void isx012_set_metering(int mode)
{
	CAM_DEBUG(" %d", mode);

	switch (mode) {
	case CAMERA_CENTER_WEIGHT:
		ISX012_WRITE_LIST(isx012_Metering_Center);
		break;

	case CAMERA_AVERAGE:
		ISX012_WRITE_LIST(isx012_Metering_Matrix);
		break;

	case CAMERA_SPOT:
		ISX012_WRITE_LIST(isx012_Metering_Spot);
		break;

	default:
		break;
	}

	isx012_ctrl->settings.metering = mode;
}

static void isx012_set_af_status(int status)
{
	int timeout_cnt = 0;
	short unsigned int r_data[1] = { 0 };
	uint16_t ae_data[1] = { 0 };
	int16_t ersc_data[1] = { 0 };

	g_bPreFlash = 0;

	if (status) {		/* start AF */
		CAM_DEBUG(" START AF (mode = %s)",
			  (isx012_ctrl->af_mode == SHUTTER_AF_MODE) ?
			  "shutter" : "touch");

		if (isx012_ctrl->af_mode == SHUTTER_AF_MODE)
			ISX012_BURST_WRITE_LIST(isx012_AF_Window_Reset);

#if !(defined(CONFIG_MACH_JAGUAR) || defined(CONFIG_MACH_STRETTO))
		if (isx012_ctrl->settings.focus_mode == CAMERA_AF_MACRO)
			ISX012_WRITE_LIST(isx012_AF_Init_Macro_ON);
		else
			ISX012_WRITE_LIST(isx012_AF_Init_Macro_OFF);
#endif

		isx012_get_LowLightCondition();

		if ((isx012_ctrl->flash_mode == CAMERA_FLASH_AUTO
			&& isx012_ctrl->lowLight) ||
		    isx012_ctrl->flash_mode == CAMERA_FLASH_ON) {

			/*AE line change - AE line change value write*/
			ISX012_WRITE_LIST(isx012_Flash_AELINE);

			/*wait 1V time (60ms)*/
			msleep(60);

			/*Read AE scale - ae_auto, ersc_auto*/
			isx012_i2c_read(0x01CE, ae_data);
			g_ae_auto = ae_data[0];

			isx012_i2c_read(0x01CA, ersc_data);
			g_ersc_auto = ersc_data[0];

			if (isx012_ctrl->awb_mode ==
					CAMERA_WHITE_BALANCE_AUTO) {
				CAM_DEBUG(" AWB mode : AUTO");
				isx012_i2c_write_multi(0x0282, 0x00, 0x01);
			}

			/*Flash On set */
			ISX012_WRITE_LIST(isx012_Flash_ON);

			/*Fast AE, AWB, AF start*/
			isx012_i2c_write_multi(0x8800, 0x01, 0x01);

			/*wait 1V time (40ms)*/
			msleep(40);

			isx012_set_flash(MOVIE_FLASH);
			g_bPreFlash = 1;

			do {
				isx012_i2c_read(0x0080, r_data);
				if (r_data[0] == 0x1)
					break;
				mdelay(1);
			} while (timeout_cnt++ < ISX012_DELAY_RETRIES);

			timeout_cnt = 0;
			do {
				isx012_i2c_read(0x01B0, r_data);
				if (r_data[0] == 0x0)
					break;
				mdelay(1);
			} while (timeout_cnt++ < ISX012_DELAY_RETRIES);
		} else {
#if defined(CONFIG_MACH_JAGUAR) || defined(CONFIG_MACH_STRETTO)
			if (isx012_ctrl->samsungapp == 0) {
				ISX012_WRITE_LIST(
					isx012_Camcorder_Halfrelease_Mode);
			} else {
				if ((isx012_ctrl->settings.scenemode
					== CAMERA_SCENE_NIGHT)
					&& (isx012_ctrl->lowLight)) {
					ISX012_WRITE_LIST(
					isx012_Lowlux_night_Halfrelease_Mode);
				} else {
					ISX012_WRITE_LIST(
						isx012_Halfrelease_Mode);
				}
			}
#else
			if ((isx012_ctrl->settings.scenemode
					== CAMERA_SCENE_NIGHT)
					&& (isx012_ctrl->lowLight)) {
				ISX012_WRITE_LIST(
				isx012_Lowlux_night_Halfrelease_Mode);
			} else {/* temp : SamsungApp 0, etc 1 */
				if (isx012_ctrl->samsungapp == 0) {
					ISX012_WRITE_LIST(
						isx012_Halfrelease_Mode);
				} else {
					ISX012_WRITE_LIST(
						isx012_Barcode_SAF);
				}
			}
#endif
			/*wait 1V time (40ms)*/
			msleep(40);
		}
	} else {		/* stop AF */
		CAM_DEBUG(" stop AF");
		isx012_set_af_stop(1);
	}
}

static void isx012_set_camcorder_af_status(int status)
{
	int timeout_cnt = 0;
	short unsigned int r_data[1] = { 0 };
	uint16_t ae_data[1] = { 0 };
	int16_t ersc_data[1] = { 0 };

	if (status) {		/* start AF */
		if (isx012_ctrl->flash_mode == CAMERA_FLASH_ON) {

			/*AE line change - AE line change value write*/
			ISX012_WRITE_LIST(isx012_Flash_AELINE);

			/*wait 1V time (60ms)*/
			msleep(60);

			/*Read AE scale - ae_auto, ersc_auto*/
			isx012_i2c_read(0x01CE, ae_data);
			g_ae_auto = ae_data[0];

			isx012_i2c_read(0x01CA, ersc_data);
			g_ersc_auto = ersc_data[0];

			if (isx012_ctrl->awb_mode ==
					CAMERA_WHITE_BALANCE_AUTO) {
				CAM_DEBUG(" AWB mode : AUTO");
				isx012_i2c_write_multi(0x0282, 0x00, 0x01);
			}

			/*Flash On set */
			ISX012_WRITE_LIST(isx012_Flash_ON);

			/*Fast AE, AWB, AF start*/
			isx012_i2c_write_multi(0x8800, 0x01, 0x01);

			/*wait 1V time (40ms)*/
			msleep(40);

			do {
				isx012_i2c_read(0x0080, r_data);
				if (r_data[0] == 0x1)
					break;
				mdelay(1);
			} while (timeout_cnt++ < ISX012_DELAY_RETRIES);

			timeout_cnt = 0;
			do {
				isx012_i2c_read(0x01B0, r_data);
				if (r_data[0] == 0x0)
					break;
				mdelay(1);
			} while (timeout_cnt++ < ISX012_DELAY_RETRIES);
		} else {
#if defined(CONFIG_MACH_JAGUAR) || defined(CONFIG_MACH_STRETTO)
			ISX012_WRITE_LIST(
				isx012_Camcorder_Halfrelease_Mode);
#else
			ISX012_WRITE_LIST(
				isx012_Halfrelease_Mode);
#endif
			/*wait 1V time (40ms)*/
			msleep(40);
		}
	} else {		/* stop AF */
		CAM_DEBUG(" stop AF");
		isx012_set_camcorder_af_stop(1);
	}
}

static int isx012_set_touchaf_pos(int x, int y)
{
	unsigned int H_ratio = 324;	/*H_RATIO : 3.24 = 2592 / 800 */
	unsigned int V_ratio = 405;	/*V_RATIO : 4.05 = 1944 / 480 */

	unsigned int AF_OPD4_HDELAY = 486;
	unsigned int AF_OPD4_VDELAY = 259;
	unsigned int AF_OPD4_HVALID = 259;
	unsigned int AF_OPD4_VVALID = 324;


	switch (isx012_ctrl->settings.preview_size_idx) {
	case PREVIEW_SIZE_WVGA:
		H_ratio = 324;
		break;

	default:
		H_ratio = 405;
		break;
	}

	AF_OPD4_HVALID = 259;
	AF_OPD4_VVALID = 324;

	AF_OPD4_HDELAY = ((x * H_ratio) / 100) - (AF_OPD4_HVALID / 2) + 8 + 41;
	AF_OPD4_VDELAY = ((y * V_ratio) / 100) - (AF_OPD4_VVALID / 2) + 5;

	if (AF_OPD4_HDELAY < 8 + 41)
		AF_OPD4_HDELAY = 8 + 41;
	if (AF_OPD4_VDELAY < 5)
		AF_OPD4_VDELAY = 5;

	if (AF_OPD4_HDELAY > 2608 - AF_OPD4_HVALID - 8 - 41)
		AF_OPD4_HDELAY = 2608 - AF_OPD4_HVALID - 8 - 41;
	if (AF_OPD4_VDELAY > 1944 - AF_OPD4_VVALID - 5)
		AF_OPD4_VDELAY = 1944 - AF_OPD4_VVALID - 5;

	isx012_i2c_write_multi(0x6A50, AF_OPD4_HDELAY, 2);
	isx012_i2c_write_multi(0x6A52, AF_OPD4_VDELAY, 2);
	isx012_i2c_write_multi(0x6A54, AF_OPD4_HVALID, 2);
	isx012_i2c_write_multi(0x6A56, AF_OPD4_VVALID, 2);

	isx012_i2c_write_multi(0x6A80, 0x0000, 1);
	isx012_i2c_write_multi(0x6A81, 0x0000, 1);
	isx012_i2c_write_multi(0x6A82, 0x0000, 1);
	isx012_i2c_write_multi(0x6A83, 0x0000, 1);
	isx012_i2c_write_multi(0x6A84, 0x0008, 1);
	isx012_i2c_write_multi(0x6A85, 0x0000, 1);
	isx012_i2c_write_multi(0x6A86, 0x0000, 1);
	isx012_i2c_write_multi(0x6A87, 0x0000, 1);
	isx012_i2c_write_multi(0x6A88, 0x0000, 1);
	isx012_i2c_write_multi(0x6A89, 0x0000, 1);
	isx012_i2c_write_multi(0x6646, 0x0000, 1);

	return 0;
}

static struct msm_cam_clk_info cam_clk_info[] = {
#if defined(CONFIG_MACH_APEXQ) || defined(CONFIG_MACH_COMANCHE)\
	|| defined(CONFIG_MACH_EXPRESS) || defined(CONFIG_MACH_AEGIS2)
	{"cam_clk", MSM_SENSOR_MCLK_19HZ},
#else
	{"cam_clk", MSM_SENSOR_MCLK_24HZ},
#endif
};

#if defined(CONFIG_ISX012) && defined(CONFIG_S5K8AAY) /* JAGUAR */
static int isx012_sensor_power_up(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_init();
#endif

	rc = msm_camera_request_gpio_table(data, 1);
	if (rc < 0)
		cam_err("%s: request gpio failed", __func__);

	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 0);
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);

	/*Power on the LDOs */
	data->sensor_platform_info->sensor_power_on(0);

	/*Set Main Clock*/
	if (s_ctrl->clk_rate != 0)
		cam_clk_info->clk_rate = s_ctrl->clk_rate;

	rc = msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 1);

	if (rc < 0)
		cam_err(" clk enable failed");

	usleep(50);

	/*reset VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 1);
	usleep(100);

	/*off standy VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);
	msleep(125);

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 1);
	mdelay(8);

	isx012_mode_transtion_OM();

	/*PreSleep*/
	ISX012_BURST_WRITE_LIST(isx012_Pll_Setting_4);
	usleep(2*1000);

	isx012_mode_transtion_OM();

	/*Sleep*/
	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 1);
	mdelay(13);

	isx012_mode_transtion_OM();

	/*Active*/
	isx012_mode_transtion_CM();

	CAM_DEBUG(" MIPI write");
	rc = isx012_i2c_write_multi(0x5008, 0x00, 0x01);
	if (rc < 0) {
		cam_err("I2C ERROR: rc = %d", rc);
		return rc;
	}

	isx012_Sensor_Calibration();

	ISX012_BURST_WRITE_LIST(isx012_Init_Reg);

	isx012_set_init_mode();

	CAM_DEBUG(" X");

	return rc;
}

#elif defined(CONFIG_ISX012) && defined(CONFIG_SR030PC50) /* ApexQ*/
static int isx012_sensor_power_up(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	int temp = 0;
	unsigned short test_read;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_init();
	isx012_AEgain_offset_tuning();
#endif

	rc = msm_camera_request_gpio_table(data, 1);
	if (rc < 0)
		cam_err(" request gpio failed");

	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_stby);
	CAM_DEBUG(" check VT standby : %d", temp);

	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_reset);
	CAM_DEBUG(" check VT reset : %d", temp);

	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_reset);
	CAM_DEBUG(" CAM_5M_RST : %d", temp);

	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_stby);
	CAM_DEBUG(" CAM_5M_ISP_INIT : %d", temp);

	/*Power on the LDOs */
	data->sensor_platform_info->sensor_power_on(0);

	/*standy VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 1);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_stby);
	CAM_DEBUG(" check VT standby : %d", temp);

	/*Set Main clock */
	gpio_tlmm_config(GPIO_CFG(data->sensor_platform_info->mclk, 1,
		GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA),
		GPIO_CFG_ENABLE);

	if (s_ctrl->clk_rate != 0)
		cam_clk_info->clk_rate = s_ctrl->clk_rate;

	rc = msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 1);
	if (rc < 0)
		cam_err(" clk enable failed");

	usleep(15);

	/*reset VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 1);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_reset);
	CAM_DEBUG(" check VT reset : %d", temp);
	usleep(100);

	/*off standy VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_stby);
	CAM_DEBUG(" check VT standby : %d", temp);
	usleep(125 * 1000);

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 1);
	temp = gpio_get_value(data->sensor_platform_info->sensor_reset);
	CAM_DEBUG(" CAM_5M_RST : %d", temp);
	usleep(6 * 1000);

	/* sensor validation test */
	CAM_DEBUG(" Main Camera Sensor Validation Test");
	rc = isx012_i2c_read(0x000E, (unsigned short *)&test_read);
	if (rc < 0) {
		pr_info(" Error in Main Camera Sensor Validation Test");
		return rc;
	}

	/*I2C */
	CAM_DEBUG(" Mode Trandition 1");

	isx012_mode_transtion_OM();

	/*usleep(10*1000);
	   ISX012_WRITE_LIST(isx012_Pll_Setting_3);
	   CAM_DEBUG(" Mode Trandition 2"); */


	ISX012_WRITE_LIST(isx012_Pll_Setting_4);
	CAM_DEBUG(" Mode Trandition 2");

	usleep(200); /* 0.2ms */

	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 1);
	temp = gpio_get_value(data->sensor_platform_info->sensor_stby);
	CAM_DEBUG(" CAM_5M_ISP_INIT : %d", temp);
	usleep(12*1000);

	isx012_mode_transtion_OM();

	/*Active*/
	isx012_mode_transtion_CM();

	CAM_DEBUG(" MIPI write");

	isx012_i2c_write_multi(0x5008, 0x00, 0x01);
	isx012_Sensor_Calibration();
	ISX012_WRITE_LIST(isx012_Init_Reg);

	isx012_set_init_mode();

	CAM_DEBUG(" X");

	return rc;
}
#elif defined(CONFIG_ISX012) && defined(CONFIG_DB8131M) /* Gogh *//* AEGIS2 */
static int isx012_sensor_power_up(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	int temp = 0;
	unsigned short test_read;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_init();
	isx012_AEgain_offset_tuning();
#endif

	rc = msm_camera_request_gpio_table(data, 1);
	if (rc < 0)
		cam_err(" request gpio failed");

	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_stby);
	CAM_DEBUG(" check VT standby : %d", temp);

	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_reset);
	CAM_DEBUG(" check VT reset : %d", temp);

	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_reset);
	CAM_DEBUG(" CAM_5M_RST : %d", temp);

	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_stby);
	CAM_DEBUG(" CAM_5M_ISP_INIT : %d", temp);

	/*Power on the LDOs */
	data->sensor_platform_info->sensor_power_on(0);
	usleep(15);

	/*Set Main clock */
	gpio_tlmm_config(GPIO_CFG(data->sensor_platform_info->mclk, 1,
		GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA),
		GPIO_CFG_ENABLE);

	if (s_ctrl->clk_rate != 0)
		cam_clk_info->clk_rate = s_ctrl->clk_rate;

	rc = msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 1);
	if (rc < 0)
		cam_err(" clk enable failed");

	usleep(1000);/* > 1clk */

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 1);
	temp = gpio_get_value(data->sensor_platform_info->sensor_reset);
	CAM_DEBUG(" CAM_5M_RST : %d", temp);
	usleep(6 * 1000);

	isx012_mode_transtion_OM();

	/*PreSleep*/
	ISX012_BURST_WRITE_LIST(isx012_Pll_Setting_4);
	usleep(200);

	isx012_mode_transtion_OM();
	usleep(300);

	/*Sleep*/
	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 1);
	temp = gpio_get_value(data->sensor_platform_info->sensor_stby);
	CAM_DEBUG(" CAM_5M_STBY : %d", temp);
	mdelay(13);

	isx012_mode_transtion_OM();

	/*Active*/
	isx012_mode_transtion_CM();

	CAM_DEBUG(" MIPI write");
	rc = isx012_i2c_write_multi(0x5008, 0x00, 0x01);
	if (rc < 0) {  /* sensor validation check */
		cam_err(" I2C ERROR: rc = %d", rc);
		return rc;
	}

	CAM_DEBUG(" Calibration");
	isx012_Sensor_Calibration();

	CAM_DEBUG(" Init register");
	ISX012_BURST_WRITE_LIST(isx012_Init_Reg);

	isx012_set_init_mode();

	CAM_DEBUG(" X");

	return rc;
}
#elif defined(CONFIG_ISX012) && defined(CONFIG_S5K6A3YX) /* stretto */
static int isx012_sensor_power_up(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_init();
#endif

	rc = msm_camera_request_gpio_table(data, 1);
	if (rc < 0)
		cam_err("%s: request gpio failed", __func__);

	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);

	/*Power on the LDOs */
	data->sensor_platform_info->sensor_power_on(0);

	/*Set Main Clock*/
	if (s_ctrl->clk_rate != 0)
		cam_clk_info->clk_rate = s_ctrl->clk_rate;

	rc = msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 1);

	if (rc < 0)
		cam_err(" clk enable failed");

	usleep(50);

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 1);
	mdelay(8);

	isx012_mode_transtion_OM();

	/*PreSleep*/
	ISX012_BURST_WRITE_LIST(isx012_Pll_Setting_4);
	usleep(2*1000);

	isx012_mode_transtion_OM();

	/*Sleep*/
	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 1);
	mdelay(13);

	isx012_mode_transtion_OM();

	/*Active*/
	isx012_mode_transtion_CM();

	CAM_DEBUG(" MIPI write");
	rc = isx012_i2c_write_multi(0x5008, 0x00, 0x01);
	if (rc < 0) {
		cam_err("I2C ERROR: rc = %d", rc);
		return rc;
	}

	isx012_Sensor_Calibration();

	ISX012_BURST_WRITE_LIST(isx012_Init_Reg);

	isx012_set_init_mode();

	CAM_DEBUG(" X");

	return rc;
}
#else
static int isx012_sensor_power_up(struct msm_sensor_ctrl_t *s_ctrl)
{
	printk(KERN_DEBUG "isx012_sensor_power_up");
}
#endif

void sensor_native_control(void __user *arg)
{
	struct ioctl_native_cmd ctrl_info;

	if (copy_from_user((void *)&ctrl_info,
			   (const void *)arg, sizeof(ctrl_info)))
		cam_err("fail copy_from_user!");

	switch (ctrl_info.mode) {	/* for fast initialising */
	case EXT_CAM_EV:
		if (isx012_ctrl->settings.scene == CAMERA_SCENE_BEACH)
			break;
		else {
			isx012_set_ev(ctrl_info.value_1);
			break;
		}

	case EXT_CAM_WB:
		isx012_set_whitebalance(ctrl_info.value_1);
		isx012_ctrl->awb_mode = ctrl_info.value_1;
		break;

	case EXT_CAM_METERING:
		isx012_set_metering(ctrl_info.value_1);
		break;

	case EXT_CAM_ISO:
		isx012_set_iso(ctrl_info.value_1);
		break;

	case EXT_CAM_EFFECT:
		isx012_set_effect(ctrl_info.value_1);
		break;

	case EXT_CAM_SCENE_MODE:
		isx012_set_scene_mode(ctrl_info.value_1);
		break;

	case EXT_CAM_MOVIE_MODE:
		CAM_DEBUG(" MOVIE mode : %d", ctrl_info.value_1);
		isx012_ctrl->cam_mode = ctrl_info.value_1;
		break;

	case EXT_CAM_PREVIEW_SIZE:
		isx012_set_preview_size(ctrl_info.value_1);
		break;

	case EXT_CAM_SET_AF_STATUS:
		if (isx012_ctrl->cam_mode == MOVIE_MODE)
			isx012_set_camcorder_af_status(ctrl_info.value_1);
		else
			isx012_set_af_status(ctrl_info.value_1);
		break;

	case EXT_CAM_GET_AF_STATUS:
		ctrl_info.value_1 = isx012_get_sensor_af_status();
		break;

	case EXT_CAM_GET_AF_RESULT:
		ctrl_info.value_1 = isx012_get_af_result();
		break;

	case EXT_CAM_SET_TOUCHAF_POS:
		isx012_set_touchaf_pos(ctrl_info.value_1,
			ctrl_info.value_2);
		isx012_ctrl->af_mode = TOUCH_AF_MODE;
		break;

	case EXT_CAM_SET_AF_MODE:
		if (ctrl_info.value_1)
			isx012_ctrl->af_mode = TOUCH_AF_MODE;
		else
			isx012_ctrl->af_mode = SHUTTER_AF_MODE;

		break;

	case EXT_CAM_FLASH_STATUS:
		isx012_set_flash(ctrl_info.value_1);
		break;

	case EXT_CAM_GET_FLASH_STATUS:
		ctrl_info.value_1 = isx012_get_flash_status();
		break;

	case EXT_CAM_FLASH_MODE:
		isx012_ctrl->flash_mode = ctrl_info.value_1;
		break;

	case EXT_CAM_FOCUS:
		isx012_set_af_mode(ctrl_info.value_1);
		break;

	case EXT_CAM_DTP_TEST:
		isx012_check_dataline(ctrl_info.value_1);
		break;

	case EXT_CAM_SET_AE_AWB:
		isx012_set_ae_awb(ctrl_info.value_1);
		break;

	case EXT_CAM_SET_AF_STOP:
		isx012_set_af_stop(ctrl_info.value_1);
		break;

	case EXT_CAM_EXIF:
		ctrl_info.value_1 = isx012_get_exif(ctrl_info.address,
			ctrl_info.value_2);
		break;

	case EXT_CAM_VT_MODE:
		CAM_DEBUG(" VT mode : %d", ctrl_info.value_1);
		isx012_ctrl->vtcall_mode = ctrl_info.value_1;
		break;

	case EXT_CAM_SET_FPS:
		isx012_set_frame_rate(ctrl_info.value_1);
		break;

	case EXT_CAM_SAMSUNG_CAMERA:
		CAM_DEBUG(" SAMSUNG camera : %d", ctrl_info.value_1);
		isx012_ctrl->samsungapp = ctrl_info.value_1;
		break;

	default:
		break;
	}

	if (copy_to_user((void *)arg,
			 (const void *)&ctrl_info, sizeof(ctrl_info)))
		cam_err("fail copy_from_user!");
}

long isx012_sensor_subdev_ioctl(struct v4l2_subdev *sd,
			unsigned int cmd, void *arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case VIDIOC_MSM_SENSOR_CFG:
		return isx012_sensor_config(&isx012_s_ctrl, argp);
	default:
		return -ENOIOCTLCMD;
	}
}

int isx012_sensor_config(struct msm_sensor_ctrl_t *s_ctrl,
	void __user *argp)
{
	struct sensor_cfg_data cfg_data;
	long rc = 0;

	if (copy_from_user(&cfg_data,
			   (void *)argp, sizeof(struct sensor_cfg_data)))
		return -EFAULT;

	switch (cfg_data.cfgtype) {
	case CFG_SENSOR_INIT:
		if (config_csi2 == 0)
			rc = isx012_sensor_setting(UPDATE_PERIODIC,
					RES_PREVIEW);
		break;
	case CFG_SET_MODE:
		rc = isx012_set_sensor_mode(cfg_data.mode);
		break;
	case CFG_GET_AF_MAX_STEPS:
	default:
		rc = 0;
		break;
	}

	return rc;
}

#if defined(CONFIG_ISX012) && defined(CONFIG_S5K8AAY)/* jaquar */
static int isx012_sensor_power_down(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

	isx012_set_flash(FLASH_OFF);

	/*Soft landing */
	ISX012_WRITE_LIST(isx012_Sensor_Off_VCM);

	/*standy VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);

	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);
	usleep(100 * 1000);

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);

	/*CAM_MCLK0*/
	msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 0);

	gpio_tlmm_config(GPIO_CFG(data->sensor_platform_info->mclk, 0,
		GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA),
		GPIO_CFG_ENABLE);

	/*reset VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 0);
	usleep(50);

	/*power off the LDOs */
	data->sensor_platform_info->sensor_power_off(0);

	rc = msm_camera_request_gpio_table(data, 0);
	if (rc < 0)
		cam_err(" request gpio failed");

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_exit();
#endif

	CAM_DEBUG(" X");

	return rc;
}
#elif defined(CONFIG_ISX012) && defined(CONFIG_SR030PC50) /* ApexQ*/
static int isx012_sensor_power_down(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	int temp = 0;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

	isx012_set_flash(FLASH_OFF);

	/*Soft landing */
	ISX012_WRITE_LIST(isx012_Sensor_Off_VCM);

	/*standy VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_stby);
	CAM_DEBUG(" check VT standby : %d", temp);

	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_stby);
	CAM_DEBUG(" CAM_5M_ISP_INIT : %d", temp);
	usleep(100 * 1000);

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_reset);
	CAM_DEBUG(" CAM_5M_RST : %d", temp);

	/*CAM_MCLK0*/
	msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 0);

	gpio_tlmm_config(GPIO_CFG(data->sensor_platform_info->mclk, 0,
		GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA),
		GPIO_CFG_ENABLE);

	/*reset VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_reset);
	CAM_DEBUG(" check VT reset : %d", temp);
	usleep(50);

	/*power off the LDOs */
	data->sensor_platform_info->sensor_power_off(0);

	msm_camera_request_gpio_table(data, 0);

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_exit();
#endif

	CAM_DEBUG(" X");

	return rc;
}
#elif defined(CONFIG_ISX012) && defined(CONFIG_DB8131M) /* Gogh *//* AEGIS2 */
static int isx012_sensor_power_down(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	int temp = 0;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

	isx012_set_flash(FLASH_OFF);

	/*Soft landing */
	ISX012_WRITE_LIST(isx012_Sensor_Off_VCM);
	usleep(10*1000);

	/*standy VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_stby);
	CAM_DEBUG(" check VT standby : %d", temp);

	/*reset VT */
	gpio_set_value_cansleep(data->sensor_platform_info->vt_sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->vt_sensor_reset);
	CAM_DEBUG(" check VT reset : %d", temp);

	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_stby);
	CAM_DEBUG(" CAM_5M_ISP_INIT : %d", temp);
	mdelay(110); /* > 100ms */

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);
	temp = gpio_get_value(data->sensor_platform_info->sensor_reset);
	CAM_DEBUG(" CAM_5M_RST : %d", temp);
	usleep(1000);

	/*CAM_MCLK0*/
	msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 0);

	gpio_tlmm_config(GPIO_CFG(data->sensor_platform_info->mclk, 0,
		GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA),
		GPIO_CFG_ENABLE);

	usleep(1000);

	/*power off the LDOs */
	data->sensor_platform_info->sensor_power_off(0);

	msm_camera_request_gpio_table(data, 0);

	config_csi2 = 0;
	g_bCameraRunning = false;

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_exit();
#endif

	CAM_DEBUG(" X");

	return rc;
}
#elif defined(CONFIG_ISX012) && defined(CONFIG_S5K6A3YX) /* stretto */
static int isx012_sensor_power_down(struct msm_sensor_ctrl_t *s_ctrl)
{
	int rc = 0;
	struct msm_camera_sensor_info *data = s_ctrl->sensordata;

	CAM_DEBUG(" E");

	isx012_set_flash(FLASH_OFF);

	/*Soft landing */
	ISX012_WRITE_LIST(isx012_Sensor_Off_VCM);

	/*standby Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_stby, 0);
	usleep(100 * 1000);

	/*reset Main cam */
	gpio_set_value_cansleep(data->sensor_platform_info->sensor_reset, 0);

	/*CAM_MCLK0*/
	msm_cam_clk_enable(&s_ctrl->sensor_i2c_client->client->dev,
		cam_clk_info, &s_ctrl->cam_clk, ARRAY_SIZE(cam_clk_info), 0);

	gpio_tlmm_config(GPIO_CFG(data->sensor_platform_info->mclk, 0,
		GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA),
		GPIO_CFG_ENABLE);

	/*power off the LDOs */
	data->sensor_platform_info->sensor_power_off(0);

	rc = msm_camera_request_gpio_table(data, 0);
	if (rc < 0)
		cam_err(" request gpio failed");

#ifdef CONFIG_LOAD_FILE
	isx012_regs_table_exit();
#endif

	CAM_DEBUG(" X");

	return rc;
}
#else
static int isx012_sensor_power_down(struct msm_sensor_ctrl_t *s_ctrl)
{
	CAM_DEBUG("");
}
#endif

struct v4l2_subdev_info isx012_subdev_info[] = {
	{
	 .code = V4L2_MBUS_FMT_YUYV8_2X8,
	 .colorspace = V4L2_COLORSPACE_JPEG,
	 .fmt = 1,
	 .order = 0,
	 },
	/* more can be supported, to be added later */
};

static int isx012_enum_fmt(struct v4l2_subdev *sd, unsigned int index,
			   enum v4l2_mbus_pixelcode *code)
{
	CAM_DEBUG(" Index is %d", index);

	if ((unsigned int)index >= ARRAY_SIZE(isx012_subdev_info))
		return -EINVAL;

	*code = isx012_subdev_info[index].code;
	return 0;
}

static struct v4l2_subdev_core_ops isx012_subdev_core_ops = {
	.s_ctrl = msm_sensor_v4l2_s_ctrl,
	.queryctrl = msm_sensor_v4l2_query_ctrl,
	.ioctl = isx012_sensor_subdev_ioctl,
	.s_power = msm_sensor_power,
};

static struct v4l2_subdev_video_ops isx012_subdev_video_ops = {
	.enum_mbus_fmt = isx012_enum_fmt,
};

static struct v4l2_subdev_ops isx012_subdev_ops = {
	.core = &isx012_subdev_core_ops,
	.video = &isx012_subdev_video_ops,
};

static int isx012_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	int rc = 0;
	struct msm_sensor_ctrl_t *s_ctrl;

	CAM_DEBUG(" E");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		rc = -ENOTSUPP;
		cam_err("i2c_check_functionality failed");
		goto probe_failure;
	}

	s_ctrl = (struct msm_sensor_ctrl_t *)(id->driver_data);
	if (s_ctrl->sensor_i2c_client != NULL) {
		s_ctrl->sensor_i2c_client->client = client;
		if (s_ctrl->sensor_i2c_addr != 0)
			s_ctrl->sensor_i2c_client->client->addr =
				s_ctrl->sensor_i2c_addr;
	} else {
		cam_err("s_ctrl->sensor_i2c_client is NULL");
		rc = -EFAULT;
		goto probe_failure;
	}

	s_ctrl->sensordata = client->dev.platform_data;
	if (s_ctrl->sensordata == NULL) {
		cam_err("%s: NULL sensor data", __func__);
		rc = -EFAULT;
		goto probe_failure;
	}

	isx012_client = client;
	isx012_dev = s_ctrl->sensor_i2c_client->client->dev;

	isx012_ctrl = kzalloc(sizeof(struct isx012_ctrl), GFP_KERNEL);
	if (!isx012_ctrl) {
		cam_err("isx012_ctrl alloc failed!");
		rc = -ENOMEM;
		goto probe_failure;
	}

	isx012_exif = kzalloc(sizeof(struct isx012_exif_data), GFP_KERNEL);
	if (!isx012_exif) {
		cam_err("Cannot allocate memory fo EXIF structure!");
		kfree(isx012_ctrl);
		rc = -ENOMEM;
		goto probe_failure;
	}

	memset(isx012_ctrl, 0, sizeof(isx012_ctrl));

	snprintf(s_ctrl->sensor_v4l2_subdev.name,
		sizeof(s_ctrl->sensor_v4l2_subdev.name), "%s", id->name);

	v4l2_i2c_subdev_init(&s_ctrl->sensor_v4l2_subdev, client,
		&isx012_subdev_ops);

	isx012_ctrl->sensor_dev = &s_ctrl->sensor_v4l2_subdev;
	isx012_ctrl->sensordata = client->dev.platform_data;

	rc = msm_sensor_register(&s_ctrl->sensor_v4l2_subdev);
	if (rc < 0) {
		cam_err(" msm_sensor_register failed!");
		kfree(isx012_exif);
		kfree(isx012_ctrl);
		goto probe_failure;
	}
	CAM_DEBUG(" success!");
	CAM_DEBUG(" X");
	return 0;

probe_failure:
	CAM_DEBUG(" failed!");
	CAM_DEBUG(" X");
	return rc;
}

static const struct i2c_device_id isx012_i2c_id[] = {
	{"isx012", (kernel_ulong_t)&isx012_s_ctrl},
	{},
};

static struct msm_camera_i2c_client isx012_sensor_i2c_client = {
	.addr_type = MSM_CAMERA_I2C_WORD_ADDR,
};

static struct i2c_driver isx012_i2c_driver = {
	.id_table = isx012_i2c_id,
	.probe = isx012_i2c_probe,
	.driver = {
		   .name = "isx012",
	},
};

static int __init isx012_init(void)
{
	return i2c_add_driver(&isx012_i2c_driver);
}

static struct msm_sensor_fn_t isx012_func_tbl = {
	.sensor_config = isx012_sensor_config,
	.sensor_power_up = isx012_sensor_power_up,
	.sensor_power_down = isx012_sensor_power_down,
};

static struct msm_sensor_reg_t isx012_regs = {
	.default_data_type = MSM_CAMERA_I2C_BYTE_DATA,
};

static struct msm_sensor_ctrl_t isx012_s_ctrl = {
	.msm_sensor_reg = &isx012_regs,
	.sensor_i2c_client = &isx012_sensor_i2c_client,
	.sensor_i2c_addr = 0x3D,
	.cam_mode = MSM_SENSOR_MODE_INVALID,
	.msm_sensor_mutex = &isx012_mut,
	.sensor_i2c_driver = &isx012_i2c_driver,
	.sensor_v4l2_subdev_info = isx012_subdev_info,
	.sensor_v4l2_subdev_info_size = ARRAY_SIZE(isx012_subdev_info),
	.sensor_v4l2_subdev_ops = &isx012_subdev_ops,
	.func_tbl = &isx012_func_tbl,
#if defined(CONFIG_MACH_APEXQ) || defined(CONFIG_MACH_COMANCHE)\
	|| defined(CONFIG_MACH_EXPRESS) || defined(CONFIG_MACH_AEGIS2)
	.clk_rate = MSM_SENSOR_MCLK_19HZ,
#else
	.clk_rate = MSM_SENSOR_MCLK_24HZ,
#endif
};

module_init(isx012_init);
