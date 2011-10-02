/*
 * linux/drivers/video/omap2/dss/hdmi.c
 *
 * Copyright (C) 2009 Texas Instruments
 * Author: Yong Zhi
 *
 * HDMI settings from TI's DSS driver
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 * History:
 * Mythripk <mythripk@ti.com>  Apr 2010 Modified for EDID reading and adding OMAP
 *                                      related timing
 *				May 2010 Added support of Hot Plug Detect
 *				July 2010 Redesigned HDMI EDID for Auto-detect of timing
 *				August 2010 Char device user space control for HDMI
 */

#define DSS_SUBSYS_NAME "HDMI"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/platform_device.h>
#include <video/omapdss.h>
#include <plat/cpu.h>
#include <plat/hdmi_lib.h>
#include <plat/gpio.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/fs.h>

#if defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI) || \
	defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI_MODULE)
#include <sound/soc.h>
#include <sound/pcm_params.h>
#endif

#if defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI) || \
	defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI_MODULE)
#include <sound/soc.h>
#include <sound/pcm_params.h>
#endif

#include "dss.h"
#include <plat/edid.h>

static int hdmi_enable_display(struct omap_dss_device *dssdev);
static void hdmi_disable_display(struct omap_dss_device *dssdev);
static int hdmi_display_suspend(struct omap_dss_device *dssdev);
static int hdmi_display_resume(struct omap_dss_device *dssdev);
static void hdmi_get_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings);
static void hdmi_set_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings);
static void hdmi_set_custom_edid_timing_code(struct omap_dss_device *dssdev, int code , int mode);
static int hdmi_get_edid(struct omap_dss_device *dssdev,  u8 *buf, int len);
static int hdmi_check_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings);
static int hdmi_read_edid(struct omap_video_timings *);
static int get_edid_timing_data(struct HDMI_EDID *edid);
static irqreturn_t hdmi_irq_handler(int irq, void *arg);
static int hdmi_enable_hpd(struct omap_dss_device *dssdev);
static int hdmi_set_power(struct omap_dss_device *dssdev);
static void hdmi_power_off(struct omap_dss_device *dssdev);
static int hdmi_open(struct inode *inode, struct file *filp);
static int hdmi_release(struct inode *inode, struct file *filp);
static long hdmi_ioctl(struct file *file,
			  unsigned int cmd, unsigned long arg);


/*Structures for chardevice move this to panel*/
static int hdmi_major;
static struct cdev hdmi_cdev;
static dev_t hdmi_dev_id;
/*This is a basic structure read and write ioctls will be added to configure parameters*/
static struct file_operations hdmi_fops = {
	.owner = THIS_MODULE,
	.open = hdmi_open,
	.release = hdmi_release,
	.unlocked_ioctl = hdmi_ioctl,
};

/* distinguish power states when ACTIVE */
enum hdmi_power_state {
	HDMI_POWER_OFF,
	HDMI_POWER_MIN,		/* minimum power for HPD detect */
	HDMI_POWER_FULL,	/* full power */
} hdmi_power;

static bool is_hdmi_on;		/* whether full power is needed */
static bool is_hpd_on;		/* whether hpd is enabled */
static bool user_hpd_state;	/* user hpd state */

u8		edid[HDMI_EDID_MAX_LENGTH] = {0};
u8		edid_set = false;
u8		header[8] = {0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0};
u8 hpd_mode = 0, custom_set = 0;

enum hdmi_ioctl_cmds {
	HDMI_ENABLE,
	HDMI_DISABLE,
	HDMI_READ_EDID,
};

/* PLL */
#define PLLCTRL_PLL_CONTROL				0x0ul
#define PLLCTRL_PLL_STATUS				0x4ul
#define PLLCTRL_PLL_GO					0x8ul
#define PLLCTRL_CFG1					0xCul
#define PLLCTRL_CFG2					0x10ul
#define PLLCTRL_CFG3					0x14ul
#define PLLCTRL_CFG4					0x20ul

/* HDMI PHY */
#define HDMI_TXPHY_TX_CTRL				0x0ul
#define HDMI_TXPHY_DIGITAL_CTRL			0x4ul
#define HDMI_TXPHY_POWER_CTRL			0x8ul
#define HDMI_TXPHY_PAD_CFG_CTRL			0xCul

struct hdmi_hvsync_pol {
	int vsync_pol;
	int hsync_pol;
};

/*This is the structure which has all supported timing values that OMAP4 supports*/
const struct omap_video_timings all_timings_direct[OMAP_HDMI_TIMINGS_NB] = {
						{640, 480, 25200, 96, 16, 48, 2, 10, 33},
						{1280, 720, 74250, 40, 440, 220, 5, 5, 20},
						{1280, 720, 74250, 40, 110, 220, 5, 5, 20},
						{720, 480, 27027, 62, 16, 60, 6, 9, 30},
						{2880, 576, 108000, 256, 48, 272, 5, 5, 39},
						{1440, 240, 27027, 124, 38, 114, 3, 4, 15},
						{1440, 288, 27000, 126, 24, 138, 3, 2, 19},
						{1920, 540, 74250, 44, 528, 148, 5, 2, 15},
						{1920, 540, 74250, 44, 88, 148, 5, 2, 15},
						{1920, 1080, 148500, 44, 88, 148, 5, 4, 36},
						{720, 576, 27000, 64, 12, 68, 5, 5, 39},
						{1440, 576, 54000, 128, 24, 136, 5, 5, 39},
						{1920, 1080, 148500, 44, 528, 148, 5, 4, 36},
						{2880, 480, 108108, 248, 64, 240, 6, 9, 30},
						{1920, 1080, 74250, 44, 638, 148, 5, 4, 36},
						/*Vesa frome here*/
						{640, 480, 25175, 96, 16, 48, 2 , 10, 33},
						{800, 600, 40000, 128, 40, 88, 4 , 1, 23},
						{848, 480, 33750, 112, 16, 112, 8 , 6, 23},
						{1280, 768, 79500, 128, 64, 192, 7 , 3, 20},
						{1280, 800, 83500, 128, 72, 200, 6 , 3, 22},
						{1360, 768, 85500, 112, 64, 256, 6 , 3, 18},
						{1280, 960, 108000, 112, 96, 312, 3 , 1, 36},
						{1280, 1024, 108000, 112, 48, 248, 3 , 1, 38},
						{1024, 768, 65000, 136, 24, 160, 6, 3, 29},
						{1400, 1050, 121750, 144, 88, 232, 4, 3, 32},
						{1440, 900, 106500, 152, 80, 232, 6, 3, 25},
						{1680, 1050, 146250, 176 , 104, 280, 6, 3, 30},
						{1366, 768, 85500, 143, 70, 213, 3, 3, 24},
						{1920, 1080, 148500, 44, 88, 80, 5, 4, 36},
						{1280, 768, 68250, 32, 48, 80, 7, 3, 12},
						{1400, 1050, 101000, 32, 48, 80, 4, 3, 23},
						{1680, 1050, 119000, 32, 48, 80, 6, 3, 21},
						{1280, 800, 71000, 32, 48, 80, 6, 3, 14},
						{1280, 720, 74250, 40, 110, 220, 5, 5, 20} };

/*This is a static Mapping array which maps the timing values with corresponding CEA / VESA code*/
int code_index[OMAP_HDMI_TIMINGS_NB] = {1, 19, 4, 2, 37, 6, 21, 20, 5, 16, 17, 29, 31, 35, 32,
					/* <--14 CEA 17--> vesa*/
					4, 9, 0xE, 0x17, 0x1C, 0x27, 0x20, 0x23, 0x10, 0x2A,
					0X2F, 0x3A, 0X51, 0X52, 0x16, 0x29, 0x39, 0x1B, 0x54};

/*Static mapping of the Timing values with the corresponding Vsync and Hsync polarity*/
const struct hdmi_hvsync_pol hvpol_mapping[OMAP_HDMI_TIMINGS_NB] = {
					/* CEA */
					{0, 0}, {1, 1}, {1, 1}, {0, 0},
					{0, 0}, {0, 0}, {0, 0}, {1, 1},
					{1, 1}, {1, 1}, {0, 0}, {0, 0},
					{1, 1}, {0, 0}, {1, 1},

					/* VESA */
					{0, 0}, {1, 1}, {1, 1}, {1, 0},
					{1, 0}, {1, 1},	{1, 1}, {1, 1},
					{0, 0}, {1, 0},	{1, 0}, {1, 0},
					{1, 1}, {1, 1},	{0, 1}, {0, 1},
					{0, 1}, {0, 1}, {1, 1} };

/*This is revere static mapping which maps the CEA / VESA code to the corresponding timing values*/
/* FIXME: these tables should be processed at runtime to make maintenance easier */
/* note: table is 10 entries per line to make it easier to find index.. */
int code_cea[39] = {
		-1,  0,  3,  3,  2,  8,  5,  5, -1, -1,
		-1, -1, -1, -1, -1, -1,  9, 10, 10,  1,
		7,   6,  6, -1, -1, -1, -1, -1, -1, 11,
		11, 12, 14, -1, -1, 13, 13,  4,  4};

/* note: table is 10 entries per line to make it easier to find index.. */
int code_vesa[85] = {
		-1, -1, -1, -1, 15, -1, -1, -1, -1, 16,
		-1, -1, -1, -1, 17, -1, 23, -1, -1, -1,
		-1, -1, 29, 18, -1, -1, -1, 32, 19, -1,
		-1, -1, 21, -1, -1, 22, -1, -1, -1, 20,
		-1, 30, 24, -1, -1, -1, -1, 25, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, 31, 26, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, 27, 28, -1, 33};

struct hdmi {
	struct kobject kobj;
	struct mutex lock;
	int code;
	int mode;
	int deep_color;
	int lr_fr;
	int force_set;
	struct hdmi_config cfg;
	struct omap_display_platform_data *pdata;
	struct platform_device *pdev;
};
struct hdmi hdmi;

struct hdmi_cm {
	int code;
	int mode;
};
struct omap_video_timings edid_timings;

unsigned long hdmi_pclk_rate(void)
{
	return (unsigned long)(hdmi.cfg.pixel_clock * 1000);
}

static void update_cfg (struct hdmi_config *cfg, struct omap_video_timings *timings)
{
	cfg->ppl = timings->x_res;
	cfg->lpp = timings->y_res;
	cfg->hbp = timings->hbp;
	cfg->hfp = timings->hfp;
	cfg->hsw = timings->hsw;
	cfg->vbp = timings->vbp;
	cfg->vfp = timings->vfp;
	cfg->vsw = timings->vsw;
	cfg->pixel_clock = timings->pixel_clock;
}

static void update_cfg_pol(struct hdmi_config *cfg, int  code)
{
	cfg->v_pol = hvpol_mapping[code].vsync_pol;
	cfg->h_pol = hvpol_mapping[code].hsync_pol;
}

#if 0
static inline void hdmi_write_reg(u32 base, u16 idx, u32 val)
{
	__raw_writel(val, hdmi.base + base + idx);
}

static inline u32 hdmi_read_reg(u32 base, u16 idx)
{
	return __raw_readl(hdmi.base + base + idx);
}
#else
extern void hdmi_write_reg(u32 base, u16 idx, u32 val);
extern u32 hdmi_read_reg(u32 base, u16 idx);
#endif

#define FLD_GET(val, start, end) (((val) & FLD_MASK(start, end)) >> (end))
#define FLD_MOD(orig, val, start, end) \
	(((orig) & ~FLD_MASK(start, end)) | FLD_VAL(val, start, end))

#define REG_FLD_MOD(b, i, v, s, e) \
	hdmi_write_reg(b, i, FLD_MOD(hdmi_read_reg(b, i), v, s, e))



static ssize_t hdmi_edid_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	memcpy(buf, edid, HDMI_EDID_MAX_LENGTH);
	return HDMI_EDID_MAX_LENGTH;
}

static ssize_t hdmi_edid_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	return 0;
}

static ssize_t hdmi_yuv_supported(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	bool enabled = hdmi_tv_yuv_supported(edid);
	return snprintf(buf, PAGE_SIZE, "%s\n", enabled ? "true" : "false");
}

static ssize_t hdmi_yuv_set(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int enabled;
	enabled = simple_strtoul(buf, NULL, 10);
	if (enabled)
		hdmi_configure_csc(RGB_TO_YUV);
	else
		hdmi_configure_csc(RGB);

	return size;
}


static ssize_t hdmi_deepcolor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", hdmi.deep_color);
}

static ssize_t hdmi_deepcolor_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int deepcolor;
	deepcolor = simple_strtoul(buf, NULL, 0);
	hdmi.deep_color = deepcolor;
	return size;
}

/* This function is used to configure Limited range/full range
 * with RGB format , with YUV format Full range is not supported
 * Please refer to section 6.6 Video quantization ranges in HDMI 1.3a
 * specification for more details.
 * Now conversion to Full range or limited range can either be done at
 * display controller or HDMI IP ,This function allows to select either
 * Please note : To convert to full range it is better to convert the video
 * in the dispc to full range as there will be no loss of data , if a
 * limited range data is sent ot HDMI and converted to Full range in HDMI
 * the data quality would not be good.
 */
static void hdmi_configure_lr_fr(void)
{
	int ret = 0;
	if (hdmi.mode == 0 || (hdmi.mode == 1 && hdmi.code == 1)) {
		ret = hdmi_configure_lrfr(HDMI_FULL_RANGE, 0);
		if (!ret)
			dispc_setup_color_fr_lr(1);
		return;
	}
	if (hdmi.lr_fr) {
		ret = hdmi_configure_lrfr(HDMI_FULL_RANGE, hdmi.force_set);
		if (!ret && !hdmi.force_set)
			dispc_setup_color_fr_lr(1);
	} else {
		ret = hdmi_configure_lrfr(HDMI_LIMITED_RANGE, hdmi.force_set);
		if (!ret && !hdmi.force_set)
			dispc_setup_color_fr_lr(0);
	}
}

static ssize_t hdmi_lr_fr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", hdmi.lr_fr);
}

static ssize_t hdmi_lr_fr_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int range;
	range = simple_strtoul(buf, NULL, 0);
	hdmi.lr_fr = range/10;
	hdmi.force_set = range%10;
	hdmi_configure_lr_fr();
	return size;
}

static DEVICE_ATTR(edid, S_IRUGO, hdmi_edid_show, hdmi_edid_store);
static DEVICE_ATTR(yuv, S_IRUGO | S_IWUSR, hdmi_yuv_supported, hdmi_yuv_set);
static DEVICE_ATTR(deepcolor, S_IRUGO | S_IWUSR, hdmi_deepcolor_show, hdmi_deepcolor_store);
static DEVICE_ATTR(lr_fr, S_IRUGO | S_IWUSR, hdmi_lr_fr_show, hdmi_lr_fr_store);

static int set_hdmi_hot_plug_status(struct omap_dss_device *dssdev, bool onoff)
{
	int ret = 0;

	if (onoff != user_hpd_state) {
		hdmi_notify_hpd(onoff);
		DSSINFO("hot plug event %d", onoff);
		ret = kobject_uevent(&dssdev->dev.kobj,
					onoff ? KOBJ_ADD : KOBJ_REMOVE);
		if (ret)
			DSSWARN("error sending hot plug event %d (%d)",
								onoff, ret);
		/*
		 * TRICKY: we update status here as kobject_uevent seems to
		 * always return an error for now.
		 */
		user_hpd_state = onoff;
	}

	return ret;
}

static int hdmi_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int hdmi_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long hdmi_ioctl(struct file *file,
			  unsigned int cmd, unsigned long arg)
{
	struct omap_dss_device *dssdev = NULL;
	const char *buf = "hdmi";
	int r = 0;
	int match(struct omap_dss_device *dssdev2 , void *data)
	{
		const char *str = data;
		return sysfs_streq(dssdev2->name , str);
	}
	dssdev = omap_dss_find_device((void *)buf , match);

	switch (cmd) {
	case HDMI_ENABLE:
		r = hdmi_enable_display(dssdev);
		/* set HDMI full power on resume (in case suspended) */
		is_hdmi_on = true;
		break;

	case HDMI_DISABLE:
		hdmi_disable_display(dssdev);
		break;
	case HDMI_READ_EDID:
	default:
		r = -EINVAL;
		DSSDBG("Un-recoganized command (%d)\n", cmd);
		break;
	}

	return r;
}
/*
 * refclk = (sys_clk/(highfreq+1))/(n+1)
 * so refclk = 38.4/2/(n+1) = 19.2/(n+1)
 * choose n = 15, makes refclk = 1.2
 *
 * m = tclk/cpf*refclk = tclk/2*1.2
 *
 *	for clkin = 38.2/2 = 192
 *	    phy = 2520
 *
 *	m = 2520*16/2* 192 = 105;
 *
 *	for clkin = 38.4
 *	    phy = 2520
 *
 */

struct hdmi_pll_info {
	u16 regn;
	u16 regm;
	u32 regmf;
	u16 regm2;
	u16 regsd;
	u16 dcofreq;
};

static inline void print_omap_video_timings(struct omap_video_timings *timings)
{
	extern unsigned int dss_debug;
	if (dss_debug) {
		printk(KERN_INFO "Timing Info:\n");
		printk(KERN_INFO "  pixel_clk = %d\n", timings->pixel_clock);
		printk(KERN_INFO "  x_res     = %d\n", timings->x_res);
		printk(KERN_INFO "  y_res     = %d\n", timings->y_res);
		printk(KERN_INFO "  hfp       = %d\n", timings->hfp);
		printk(KERN_INFO "  hsw       = %d\n", timings->hsw);
		printk(KERN_INFO "  hbp       = %d\n", timings->hbp);
		printk(KERN_INFO "  vfp       = %d\n", timings->vfp);
		printk(KERN_INFO "  vsw       = %d\n", timings->vsw);
		printk(KERN_INFO "  vbp       = %d\n", timings->vbp);
	}
}

static void compute_pll(int clkin, int phy,
	int n, struct hdmi_pll_info *pi)
{
	int refclk;
	u32 temp, mf;


	refclk = clkin / (n + 1);

	temp = phy * 100/(refclk);

	pi->regn = n;
	pi->regm = temp/100;
	pi->regm2 = 1;

	mf = (phy - pi->regm * refclk) * 262144;
	pi->regmf = mf/(refclk);

	if (phy > 1000 * 100) {
		pi->dcofreq = 1;
	} else {
		pi->dcofreq = 0;
	}

	pi->regsd = ((pi->regm * clkin/10)/((n + 1) * 250) + 5)/10;

	DSSDBG("M = %d Mf = %d\n", pi->regm, pi->regmf);
	DSSDBG("range = %d sd = %d\n", pi->dcofreq, pi->regsd);
}

static int hdmi_pll_init(int refsel, int dcofreq, struct hdmi_pll_info *fmt, u16 sd)
{
	u32 r;
	unsigned t = 500000;
	u32 pll = HDMI_PLLCTRL;

	/* PLL start always use manual mode */
	REG_FLD_MOD(pll, PLLCTRL_PLL_CONTROL, 0x0, 0, 0);

	r = hdmi_read_reg(pll, PLLCTRL_CFG1);
	r = FLD_MOD(r, fmt->regm, 20, 9); /* CFG1__PLL_REGM */
	r = FLD_MOD(r, fmt->regn, 8, 1);  /* CFG1__PLL_REGN */

	hdmi_write_reg(pll, PLLCTRL_CFG1, r);

	r = hdmi_read_reg(pll, PLLCTRL_CFG2);

	r = FLD_MOD(r, 0x0, 12, 12); /* PLL_HIGHFREQ divide by 2 */
	r = FLD_MOD(r, 0x1, 13, 13); /* PLL_REFEN */
	r = FLD_MOD(r, 0x0, 14, 14); /* PHY_CLKINEN de-assert during locking */

	if (dcofreq) {
		/* divider programming for 1080p */
		REG_FLD_MOD(pll, PLLCTRL_CFG3, sd, 17, 10);
		r = FLD_MOD(r, 0x4, 3, 1); /* 1000MHz and 2000MHz */
	} else
		r = FLD_MOD(r, 0x2, 3, 1); /* 500MHz and 1000MHz */

	hdmi_write_reg(pll, PLLCTRL_CFG2, r);

	r = hdmi_read_reg(pll, PLLCTRL_CFG4);
	r = FLD_MOD(r, fmt->regm2, 24, 18);
	r = FLD_MOD(r, fmt->regmf, 17, 0);

	hdmi_write_reg(pll, PLLCTRL_CFG4, r);

	/* go now */
	REG_FLD_MOD(pll, PLLCTRL_PLL_GO, 0x1ul, 0, 0);

	/* wait for bit change */
	while (FLD_GET(hdmi_read_reg(pll, PLLCTRL_PLL_GO), 0, 0))

	/* Wait till the lock bit is set */
	/* read PLL status */
	while (0 == FLD_GET(hdmi_read_reg(pll, PLLCTRL_PLL_STATUS), 1, 1)) {
		udelay(1);
		if (!--t) {
			printk(KERN_WARNING "HDMI: cannot lock PLL\n");
			DSSDBG("CFG1 0x%x\n", hdmi_read_reg(pll, PLLCTRL_CFG1));
			DSSDBG("CFG2 0x%x\n", hdmi_read_reg(pll, PLLCTRL_CFG2));
			DSSDBG("CFG4 0x%x\n", hdmi_read_reg(pll, PLLCTRL_CFG4));
			return -EIO;
		}
	}

	DSSDBG("PLL locked!\n");

	return 0;
}

static int hdmi_pll_reset(void)
{
	int t = 0;

	/* SYSREEST  controled by power FSM*/
	REG_FLD_MOD(HDMI_PLLCTRL, PLLCTRL_PLL_CONTROL, 0x0, 3, 3);

	/* READ 0x0 reset is in progress */
	while (!FLD_GET(hdmi_read_reg(HDMI_PLLCTRL,
			PLLCTRL_PLL_STATUS), 0, 0)) {
		udelay(1);
		if (t++ > 1000) {
			ERR("Failed to sysrest PLL\n");
			return -ENODEV;
		}
	}
	return 0;
}

int hdmi_pll_program(struct hdmi_pll_info *fmt)
{
	u32 r;
	int refsel;

	HDMI_PllPwr_t PllPwrWaitParam;

	/* wait for wrapper rest */
	HDMI_W1_SetWaitSoftReset();

	/* power off PLL */
	PllPwrWaitParam = HDMI_PLLPWRCMD_ALLOFF;
	r = HDMI_W1_SetWaitPllPwrState(HDMI_WP,
				PllPwrWaitParam);
	if (r)
		return r;

	/* power on PLL */
	PllPwrWaitParam = HDMI_PLLPWRCMD_BOTHON_ALLCLKS;
	r = HDMI_W1_SetWaitPllPwrState(HDMI_WP,
				PllPwrWaitParam);
	if (r)
		return r;

	hdmi_pll_reset();

	refsel = 0x3; /* select SYSCLK reference */

	r = hdmi_pll_init(refsel, fmt->dcofreq, fmt, fmt->regsd);

	return r;
}

/* double check the order */
static int hdmi_phy_init(u32 w1,
		u32 phy, int tmds)
{
	int r;

	/* wait till PHY_PWR_STATUS=LDOON */
	/* HDMI_PHYPWRCMD_LDOON = 1 */
	r = HDMI_W1_SetWaitPhyPwrState(w1, 1);
	if (r)
		return r;

	/* wait till PHY_PWR_STATUS=TXON */
	r = HDMI_W1_SetWaitPhyPwrState(w1, 2);
	if (r)
		return r;

	/* read address 0 in order to get the SCPreset done completed */
	/* Dummy access performed to solve resetdone issue */
	hdmi_read_reg(phy, HDMI_TXPHY_TX_CTRL);

	/* write to phy address 0 to configure the clock */
	/* use HFBITCLK write HDMI_TXPHY_TX_CONTROL__FREQOUT field */
	REG_FLD_MOD(phy, HDMI_TXPHY_TX_CTRL, tmds, 31, 30);

	/* write to phy address 1 to start HDMI line (TXVALID and TMDSCLKEN) */
	hdmi_write_reg(phy, HDMI_TXPHY_DIGITAL_CTRL,
				0xF0000000);

	/* setup max LDO voltage */
	REG_FLD_MOD(phy, HDMI_TXPHY_POWER_CTRL, 0xB, 3, 0);
	/*  write to phy address 3 to change the polarity control  */
	REG_FLD_MOD(phy, HDMI_TXPHY_PAD_CFG_CTRL, 0x1, 27, 27);

	return 0;
}

static int hdmi_phy_off(u32 name)
{
	int r = 0;

	/* wait till PHY_PWR_STATUS=OFF */
	/* HDMI_PHYPWRCMD_OFF = 0 */
	r = HDMI_W1_SetWaitPhyPwrState(name, 0);
	if (r)
		return r;

	return 0;
}

/* driver */
static int get_timings_index(void)
{
	int code;

	if (hdmi.mode == 0)
		code = code_vesa[hdmi.code];
	else
		code = code_cea[hdmi.code];

	if (code == -1)	{
		code = 9;
		hdmi.code = 16;
		hdmi.mode = 1;
	}
	return code;
}

static int hdmi_panel_probe(struct omap_dss_device *dssdev)
{
	int code;
	printk("ENTER hdmi_panel_probe()\n");

	dssdev->panel.config = OMAP_DSS_LCD_TFT |
			OMAP_DSS_LCD_IVS | OMAP_DSS_LCD_IHS;
	hdmi.deep_color = 0;
	hdmi.lr_fr = HDMI_LIMITED_RANGE;
	code = get_timings_index();

	dssdev->panel.timings = all_timings_direct[code];
	printk("hdmi_panel_probe x_res= %d y_res = %d", \
		dssdev->panel.timings.x_res, dssdev->panel.timings.y_res);

	mdelay(50);

	return 0;
}

static void hdmi_panel_remove(struct omap_dss_device *dssdev)
{

}

static bool hdmi_panel_is_enabled(struct omap_dss_device *dssdev)
{
	return is_hdmi_on;
}

static int hdmi_panel_enable(struct omap_dss_device *dssdev)
{
	hdmi_enable_display(dssdev);
	return 0;
}

static void hdmi_panel_disable(struct omap_dss_device *dssdev)
{
	hdmi_disable_display(dssdev);
}

static int hdmi_panel_suspend(struct omap_dss_device *dssdev)
{
	hdmi_display_suspend(dssdev);
	return 0;
}

static int hdmi_panel_resume(struct omap_dss_device *dssdev)
{
	hdmi_display_resume(dssdev);
	return 0;
}

static void hdmi_enable_clocks(int enable)
{
	if (enable)
		dss_clk_enable(DSS_CLK_ICK | DSS_CLK_FCK1 | DSS_CLK_54M |
				DSS_CLK_96M);
	else
		dss_clk_disable(DSS_CLK_ICK | DSS_CLK_FCK1 | DSS_CLK_54M |
				DSS_CLK_96M);
}

static struct omap_dss_driver hdmi_driver = {
	.probe		= hdmi_panel_probe,
	.remove		= hdmi_panel_remove,

	.disable	= hdmi_panel_disable,
	.smart_enable	= hdmi_panel_enable,
	.smart_is_enabled	= hdmi_panel_is_enabled,
	.suspend	= hdmi_panel_suspend,
	.resume		= hdmi_panel_resume,
	.get_timings	= hdmi_get_timings,
	.set_timings	= hdmi_set_timings,
	.check_timings	= hdmi_check_timings,
	.get_edid	= hdmi_get_edid,
	.set_custom_edid_timing_code	= hdmi_set_custom_edid_timing_code,
	.hpd_enable	=	hdmi_enable_hpd,
	.driver			= {
		.name   = "hdmi_panel",
		.owner  = THIS_MODULE,
	},
};
/* driver end */

#if defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI) || \
	defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI_MODULE)

static void hdmi_wp_audio_config_format(
		struct hdmi_audio_format *aud_fmt)
{
	u32 r;

	DSSDBG("Enter hdmi_wp_audio_config_format\n");

	r = hdmi_read_reg(HDMI_WP, HDMI_WP_AUDIO_CFG);
	r = FLD_MOD(r, aud_fmt->stereo_channels, 26, 24);
	r = FLD_MOD(r, aud_fmt->active_chnnls_msk, 23, 16);
	r = FLD_MOD(r, aud_fmt->sig_blk_strt_end, 5, 5);
	r = FLD_MOD(r, aud_fmt->type, 4, 4);
	r = FLD_MOD(r, aud_fmt->justify, 3, 3);
	r = FLD_MOD(r, aud_fmt->sample_order, 2, 2);
	r = FLD_MOD(r, aud_fmt->samples_p_word, 1, 1);
	r = FLD_MOD(r, aud_fmt->sample_size, 0, 0);
	hdmi_write_reg(HDMI_WP, HDMI_WP_AUDIO_CFG, r);
}

static void hdmi_wp_audio_config_dma(struct hdmi_audio_dma *aud_dma)
{
	u32 r;

	DSSDBG("Enter hdmi_wp_audio_config_dma\n");

	r = hdmi_read_reg(HDMI_WP, HDMI_WP_AUDIO_CFG2);
	r = FLD_MOD(r, aud_dma->transfer_size, 15, 8);
	r = FLD_MOD(r, aud_dma->block_size, 7, 0);
	hdmi_write_reg(HDMI_WP, HDMI_WP_AUDIO_CFG2, r);

	r = hdmi_read_reg(HDMI_WP, HDMI_WP_AUDIO_CTRL);
	r = FLD_MOD(r, aud_dma->mode, 9, 9);
	r = FLD_MOD(r, aud_dma->threshold, 8, 0);
	hdmi_write_reg(HDMI_WP, HDMI_WP_AUDIO_CTRL, r);
}

static void hdmi_core_audio_config(struct hdmi_core_audio_config *cfg)
{
	u32 r;

	/* audio clock recovery parameters */
	r = hdmi_read_reg(HDMI_CORE_AV, HDMI_CORE_AV_ACR_CTRL);
	r = FLD_MOD(r, cfg->use_mclk, 2, 2);
	r = FLD_MOD(r, cfg->en_acr_pkt, 1, 1);
	r = FLD_MOD(r, cfg->cts_mode, 0, 0);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_ACR_CTRL, r);

	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_FREQ_SVAL, cfg->mclk_mode, 2, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_N_SVAL1, cfg->n, 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_N_SVAL2, cfg->n >> 8, 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_N_SVAL3, cfg->n >> 16, 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_CTS_SVAL1, cfg->cts, 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_CTS_SVAL2, cfg->cts >> 8, 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_CTS_SVAL3, cfg->cts >> 16, 7, 0);

	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_AUD_PAR_BUSCLK_1, cfg->aud_par_busclk, 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_AUD_PAR_BUSCLK_2,
		(cfg->aud_par_busclk >> 8), 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_AUD_PAR_BUSCLK_3,
		(cfg->aud_par_busclk >> 16), 7, 0);
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_SPDIF_CTRL, cfg->fs_override, 1, 1);

	/* I2S parameters */
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_I2S_CHST4, cfg->freq_sample, 3, 0);

	r = hdmi_read_reg(HDMI_CORE_AV, HDMI_CORE_AV_I2S_IN_CTRL);
	r = FLD_MOD(r, cfg->i2s_cfg.en_high_br_aud, 7, 7);
	r = FLD_MOD(r, cfg->i2s_cfg.sck_edge_mode, 6, 6);
	r = FLD_MOD(r, cfg->i2s_cfg.cbit_order, 5, 5);
	r = FLD_MOD(r, cfg->i2s_cfg.vbit, 4, 4);
	r = FLD_MOD(r, cfg->i2s_cfg.ws_polarity, 3, 3);
	r = FLD_MOD(r, cfg->i2s_cfg.justify, 2, 2);
	r = FLD_MOD(r, cfg->i2s_cfg.direction, 1, 1);
	r = FLD_MOD(r, cfg->i2s_cfg.shift, 0, 0);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_I2S_IN_CTRL, r);

	r = hdmi_read_reg(HDMI_CORE_AV, HDMI_CORE_AV_I2S_CHST5);
	r = FLD_MOD(r, cfg->freq_sample, 7, 4);
	r = FLD_MOD(r, cfg->i2s_cfg.word_length, 3, 1);
	r = FLD_MOD(r, cfg->i2s_cfg.word_max_length, 0, 0);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_I2S_CHST5, r);

	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_I2S_IN_LEN, cfg->i2s_cfg.in_length_bits, 3, 0);

	/* audio channels and mode parameters */
	REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_HDMI_CTRL, cfg->layout, 2, 1);
	r = hdmi_read_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_MODE);
	r = FLD_MOD(r, 0, 7, 7);
	r = FLD_MOD(r, 0, 6, 6);
	r = FLD_MOD(r, 0, 5, 5);
	r = FLD_MOD(r, 1, 4, 4);
	r = FLD_MOD(r, cfg->en_direct_strm_dig_aud, 3, 3);
	r = FLD_MOD(r, cfg->en_parallel_aud, 2, 2);
	r = FLD_MOD(r, cfg->en_spdif, 1, 1);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_MODE, r);
}

static void hdmi_core_audio_infoframe_config(
		struct hdmi_core_infoframe_audio *info_aud)
{
	u8 val;
	u8 sum = 0, checksum = 0;

	sum += 0x84 + 0x001 + 0x00a;
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUDIO_TYPE, 0x84);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUDIO_VERS, 0x01);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUDIO_LEN, 0x0a);

	val = (info_aud->db1_coding_type << 4)
			| (info_aud->db1_channel_count - 1);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(0), val);
	sum += val;

	val = (info_aud->db2_sample_freq << 2) | info_aud->db2_sample_size;
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(1), val);
	sum += val;

	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(2), 0x00);

	val = info_aud->db4_channel_alloc;
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(3), val);
	sum += val;

	val = (info_aud->db5_downmix_inh << 7) | (info_aud->db5_lsv << 3);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(4), val);
	sum += val;

	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(5), 0x00);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(6), 0x00);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(7), 0x00);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(8), 0x00);
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUD_DBYTE(9), 0x00);

	checksum = 0x100 - sum;
	hdmi_write_reg(HDMI_CORE_AV, HDMI_CORE_AV_AUDIO_CHSUM, checksum);

	/*
	 * TODO: Add MPEG and SPD enable and repeat cfg when EDID parsing
	 * is available.
	 */
}

static int hdmi_config_audio_acr(u32 sample_freq, u32 *n, u32 *cts)
{
	u32 r;
	u32 deep_color = 0;
	u32 pclk = hdmi.cfg.pixel_clock;

	if (n == NULL || cts == NULL)
		return -EINVAL;
	if (omap_rev() == OMAP4430_REV_ES1_0)
		deep_color = 100;
	else {
		r = hdmi_read_reg(HDMI_WP, HDMI_WP_VIDEO_CFG);
		switch (r & 0x03) {
		case 1:
			deep_color = 100;
			break;
		case 2:
			deep_color = 125;
			break;
		case 3:
			deep_color = 150;
			break;
		default:
			return -EINVAL;
		}
	}

	switch (sample_freq) {
	case 32000:
		if ((deep_color == 125) && ((pclk == 54054)
				|| (pclk == 74250)))
			*n = 8192;
		else
			*n = 4096;
		break;
	case 44100:
		*n = 6272;
		break;
	case 48000:
		if ((deep_color == 125) && ((pclk == 54054)
				|| (pclk == 74250)))
			*n = 8192;
		else
			*n = 6144;
		break;
	default:
		*n = 0;
		return -EINVAL;
	}

	/* calculate CTS */
	*cts = pclk*(*n/128)*deep_color / (sample_freq/10);

	return 0;
}

static int hdmi_audio_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	struct hdmi_audio_format audio_format;
	struct hdmi_audio_dma audio_dma;
	struct hdmi_core_audio_config core_cfg;
	struct hdmi_core_infoframe_audio aud_if_cfg;
	int err, n, cts;
	enum hdmi_core_audio_sample_freq sample_freq;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		core_cfg.i2s_cfg.word_max_length =
			HDMI_AUDIO_I2S_MAX_WORD_20BITS;
		core_cfg.i2s_cfg.word_length = HDMI_AUDIO_I2S_CHST_WORD_16_BITS;
		core_cfg.i2s_cfg.in_length_bits =
			HDMI_AUDIO_I2S_INPUT_LENGTH_16;
		core_cfg.i2s_cfg.justify = HDMI_AUDIO_JUSTIFY_LEFT;
		audio_format.samples_p_word = HDMI_AUDIO_ONEWORD_TWOSAMPLES;
		audio_format.sample_size = HDMI_AUDIO_SAMPLE_16BITS;
		audio_format.justify = HDMI_AUDIO_JUSTIFY_LEFT;
		audio_dma.transfer_size = 0x10;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		core_cfg.i2s_cfg.word_max_length =
			HDMI_AUDIO_I2S_MAX_WORD_24BITS;
		core_cfg.i2s_cfg.word_length = HDMI_AUDIO_I2S_CHST_WORD_24_BITS;
		core_cfg.i2s_cfg.in_length_bits =
			HDMI_AUDIO_I2S_INPUT_LENGTH_24;
		audio_format.samples_p_word = HDMI_AUDIO_ONEWORD_ONESAMPLE;
		audio_format.sample_size = HDMI_AUDIO_SAMPLE_24BITS;
		audio_format.justify = HDMI_AUDIO_JUSTIFY_RIGHT;
		core_cfg.i2s_cfg.justify = HDMI_AUDIO_JUSTIFY_RIGHT;
		audio_dma.transfer_size = 0x20;
		break;
	default:
		return -EINVAL;
	}

	switch (params_rate(params)) {
	case 32000:
		sample_freq = HDMI_AUDIO_FS_32000;
		break;
	case 44100:
		sample_freq = HDMI_AUDIO_FS_44100;
		break;
	case 48000:
		sample_freq = HDMI_AUDIO_FS_48000;
		break;
	default:
		return -EINVAL;
	}

	err = hdmi_config_audio_acr(params_rate(params), &n, &cts);
	if (err < 0)
		return err;

	/* audio wrapper config */
	audio_format.stereo_channels = HDMI_AUDIO_STEREO_ONECHANNEL;
	audio_format.active_chnnls_msk = 0x03;
	audio_format.type = HDMI_AUDIO_TYPE_LPCM;
	audio_format.sample_order = HDMI_AUDIO_SAMPLE_LEFT_FIRST;
	audio_format.sig_blk_strt_end = HDMI_AUDIO_BLOCK_SIG_STARTEND_ON;

	audio_dma.block_size = 0xC0;
	audio_dma.mode = HDMI_AUDIO_TRANSF_DMA;
	audio_dma.threshold = 0x20;

	hdmi_wp_audio_config_dma(&audio_dma);
	hdmi_wp_audio_config_format(&audio_format);

	/* I2S config */
	core_cfg.i2s_cfg.en_high_br_aud = false;
	core_cfg.i2s_cfg.sck_edge_mode =
		HDMI_AUDIO_I2S_SCK_SAMPLE_EDGE_RISING;
	core_cfg.i2s_cfg.cbit_order = false;
	core_cfg.i2s_cfg.vbit = HDMI_AUDIO_I2S_VBIT_PCM;
	core_cfg.i2s_cfg.ws_polarity = HDMI_AUDIO_I2S_WS_POLARITY_LOW_IS_LEFT;
	core_cfg.i2s_cfg.direction = HDMI_AUDIO_I2S_MSB_SHIFTED_FIRST;
	core_cfg.i2s_cfg.shift = HDMI_AUDIO_I2S_FIRST_BIT_SHIFT;

	/* core audio config */
	core_cfg.freq_sample = sample_freq;
	core_cfg.n = n;
	core_cfg.cts = cts;
	if (omap_rev() == OMAP4430_REV_ES1_0) {
		core_cfg.aud_par_busclk = (((128 * 31) - 1) << 8);
		core_cfg.cts_mode = HDMI_AUDIO_CTS_MODE_HW;
	} else {
		core_cfg.aud_par_busclk = 0;
		core_cfg.cts_mode = HDMI_AUDIO_CTS_MODE_SW;
	}
	core_cfg.layout = HDMI_AUDIO_LAYOUT_2CH;
	core_cfg.use_mclk = false;
	core_cfg.mclk_mode = HDMI_AUDIO_MCLK_128FS;
	core_cfg.fs_override = true;
	core_cfg.en_acr_pkt = true;
	core_cfg.en_direct_strm_dig_aud = false;
	core_cfg.en_parallel_aud = true;
	core_cfg.en_spdif = false;

	hdmi_core_audio_config(&core_cfg);

	/*
	 * configure packet
	 * info frame audio see doc CEA861-D page 74
	 */
	aud_if_cfg.db1_coding_type = HDMI_INFOFRAME_AUDIO_DB1CT_FROM_STREAM;
	aud_if_cfg.db1_channel_count = 2;
	aud_if_cfg.db2_sample_freq = HDMI_INFOFRAME_AUDIO_DB2SF_FROM_STREAM;
	aud_if_cfg.db2_sample_size = HDMI_INFOFRAME_AUDIO_DB2SS_FROM_STREAM;
	aud_if_cfg.db4_channel_alloc = 0x00;
	aud_if_cfg.db5_downmix_inh = false;
	aud_if_cfg.db5_lsv = 0;

	hdmi_core_audio_infoframe_config(&aud_if_cfg);
	return 0;
}

static int hdmi_audio_trigger(struct snd_pcm_substream *substream, int cmd,
				  struct snd_soc_dai *dai)
{
	int err = 0;
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_AUD_MODE, 1, 0, 0);
		REG_FLD_MOD(HDMI_WP, HDMI_WP_AUDIO_CTRL, 1, 31, 31);
		REG_FLD_MOD(HDMI_WP, HDMI_WP_AUDIO_CTRL, 1, 30, 30);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		REG_FLD_MOD(HDMI_CORE_AV, HDMI_CORE_AV_AUD_MODE, 0, 0, 0);
		REG_FLD_MOD(HDMI_WP, HDMI_WP_AUDIO_CTRL, 0, 30, 30);
		REG_FLD_MOD(HDMI_WP, HDMI_WP_AUDIO_CTRL, 0, 31, 31);
		break;
	default:
		err = -EINVAL;
	}
	return err;
}

static struct snd_soc_codec_driver hdmi_audio_codec_drv = {
};

static struct snd_soc_dai_ops hdmi_audio_codec_ops = {
	.hw_params = hdmi_audio_hw_params,
	.trigger = hdmi_audio_trigger,
};

static struct snd_soc_dai_driver hdmi_codec_dai_drv = {
		.name = "omap4-hdmi-audio-codec",
		.playback = {
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_32000 |
				SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
		},
		.ops = &hdmi_audio_codec_ops,
};
#endif

int hdmi_init(struct platform_device *pdev)
{
	int r = 0, hdmi_irq;
	printk("Enter hdmi_init()\n");

	hdmi.pdata = pdev->dev.platform_data;
	hdmi.pdev = pdev;
	mutex_init(&hdmi.lock);

	hdmi_enable_clocks(1);

	hdmi_lib_init();

	hdmi_enable_clocks(0);
	/* Get the major number for this module */
	r = alloc_chrdev_region(&hdmi_dev_id, 0, 1, "hdmi_panel");
	if (r) {
		printk("HDMI: Cound not register character device\n");
		return -ENOMEM;
	}

	hdmi_major = MAJOR(hdmi_dev_id);

	/* initialize character device */
	cdev_init(&hdmi_cdev, &hdmi_fops);

	hdmi_cdev.owner = THIS_MODULE;
	hdmi_cdev.ops = &hdmi_fops;

	/* add char driver */
	r = cdev_add(&hdmi_cdev, hdmi_dev_id, 1);
	if (r) {
		printk("HDMI: Could not add hdmi char driver\n");
		unregister_chrdev_region(hdmi_dev_id, 1);
		return -ENOMEM;
	}

	hdmi_irq = platform_get_irq(pdev, 0);
	r = request_irq(hdmi_irq,
				hdmi_irq_handler,
			0, "OMAP HDMI", (void *)0);


#if defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI) || \
	defined(CONFIG_SND_OMAP_SOC_OMAP4_HDMI_MODULE)

	/* Register ASoC codec DAI */
	r = snd_soc_register_codec(&pdev->dev, &hdmi_audio_codec_drv,
					&hdmi_codec_dai_drv, 1);
	if (r) {
		DSSERR("can't register ASoC HDMI audio codec\n");
		return r;
	}
#endif

	return omap_dss_register_driver(&hdmi_driver);
}

void hdmi_exit(void)
{
	hdmi_lib_exit();
	free_irq(OMAP44XX_IRQ_DSS_HDMI, NULL);
}

static int hdmi_power_on(struct omap_dss_device *dssdev)
{
	int r = 0;
	int code = 0;
	int dirty = false;
	struct omap_video_timings *p;
	struct hdmi_pll_info pll_data;
	struct deep_color *vsdb_format = NULL;
	int clkin, n, phy, max_tmds, temp = 0, tmds_freq;

	hdmi_power = HDMI_POWER_FULL;
	code = get_timings_index();
	dssdev->panel.timings = all_timings_direct[code];

	hdmi_enable_clocks(1);

	p = &dssdev->panel.timings;

	if (!custom_set) {

		code = get_timings_index();

		DSSDBG("No edid set thus will be calling hdmi_read_edid");
		r = hdmi_read_edid(p);
		if (r) {
			r = -EIO;
			goto err;
		}

		vsdb_format = kzalloc(sizeof(*vsdb_format), GFP_KERNEL);
		hdmi_deep_color_support_info(edid, vsdb_format);
		printk("%d deep color bit 30 %d  deep color 36 bit %d max tmds freq",
		vsdb_format->bit_30, vsdb_format->bit_36, vsdb_format->max_tmds_freq);
		max_tmds = vsdb_format->max_tmds_freq * 500;

		if (get_timings_index() != code) {
			dirty = true;
		}
	} else {
		dirty = true;
	}

	update_cfg(&hdmi.cfg, p);

	code = get_timings_index();
	update_cfg_pol(&hdmi.cfg, code);

	dssdev->panel.timings = all_timings_direct[code];

	DSSDBG("hdmi_power on x_res= %d y_res = %d", \
		dssdev->panel.timings.x_res, dssdev->panel.timings.y_res);
	DSSDBG("hdmi_power on code= %d mode = %d", hdmi.code,
		 hdmi.mode);

	clkin = 3840; /* 38.4 mHz */
	n = 15; /* this is a constant for our math */

	switch (hdmi.deep_color) {
	case 0:
		phy = p->pixel_clock;
		hdmi.cfg.deep_color = 0;
		break;
	case 1:
		if (!custom_set) {
			temp = (p->pixel_clock * 125) / 100 ;
			if (vsdb_format->bit_30) {
				if (max_tmds != 0 && max_tmds >= temp)
					phy = (p->pixel_clock * 125) / 100;
			} else {
				printk(KERN_ERR"TV does not support Deep color");
				goto err;
			}
		} else {
			phy = (p->pixel_clock * 125) / 100;
		}
		hdmi.cfg.deep_color = 1;
		break;
	case 2:
		if (p->pixel_clock != 148500) {
			if (!custom_set) {
				temp = (int)(p->pixel_clock * 150) / 100;
				if (vsdb_format->bit_36) {
					if (max_tmds != 0 && max_tmds >= temp)
						phy = (p->pixel_clock * 150) / 100;
				} else {
					printk(KERN_ERR"TV does not support Deep color");
					goto err;
				}
			} else
				phy = (p->pixel_clock * 150) / 100;
		} else {
			printk(KERN_ERR"36 bit deep color not supported");
			goto err;
		}
		hdmi.cfg.deep_color = 2;
		break;
	}

	compute_pll(clkin, phy, n, &pll_data);

	HDMI_W1_StopVideoFrame(HDMI_WP);

	dispc_enable_digit_out(0);

	if (dirty) {
		omap_dss_notify(dssdev, OMAP_DSS_SIZE_CHANGE);
	}

	/* config the PLL and PHY first */
	r = hdmi_pll_program(&pll_data);
	if (r) {
		DSSERR("Failed to lock PLL\n");
		r = -EIO;
		goto err;
	}

	if (phy <= 50000) /*TMDS freq_out in the PHY should be set based on the TMDS clock*/
		tmds_freq = 0x0;
	else if ((phy > 50000) && (phy <= 100000))
		tmds_freq = 0x1;
	else
		tmds_freq = 0x2;

	r = hdmi_phy_init(HDMI_WP, HDMI_PHY, tmds_freq);
	if (r) {
		DSSERR("Failed to start PHY\n");
		r = -EIO;
		goto err;
	}

	hdmi.cfg.hdmi_dvi = hdmi.mode;
	hdmi.cfg.video_format = hdmi.code;

	if ((hdmi.mode)) {
		switch (hdmi.code) {
		case 20:
		case 5:
		case 6:
		case 21:
			hdmi.cfg.interlace = 1;
			break;
		default:
			hdmi.cfg.interlace = 0;
			break;
		}
	}

	hdmi_lib_enable(&hdmi.cfg);

	hdmi_configure_lr_fr();

	/* these settings are independent of overlays */
	dss_switch_tv_hdmi(1);

	/* bypass TV gamma table*/
	dispc_enable_gamma_table(0);

	/* allow idle mode */
	dispc_set_idle_mode();

#ifndef CONFIG_OMAP4_ES1
	/*The default reset value for DISPC.DIVISOR1 LCD is 4
	* in ES2.0 and the clock will run at 1/4th the speed
	* resulting in the sync_lost_digit */
	dispc_set_tv_divisor();
#endif

	/* tv size */
	dispc_set_digit_size(dssdev->panel.timings.x_res,
			dssdev->panel.timings.y_res);

	HDMI_W1_StartVideoFrame(HDMI_WP);

	dispc_enable_digit_out(1);

	kfree(vsdb_format);

	return 0;
err:
	kfree(vsdb_format);
	return r;
}

int hdmi_min_enable(void)
{
	int r;
	DSSDBG("hdmi_min_enable");
	hdmi_power = HDMI_POWER_MIN;
	r = hdmi_phy_init(HDMI_WP, HDMI_PHY, 0);
	if (r) {
		DSSERR("Failed to start PHY\n");
	}
	hdmi.cfg.hdmi_dvi = hdmi.mode;
	hdmi.cfg.video_format = hdmi.code;
	hdmi_lib_enable(&hdmi.cfg);
	return 0;
}

static spinlock_t irqstatus_lock = __SPIN_LOCK_UNLOCKED(irqstatus_lock);
static volatile int irqstatus;

void hdmi_work_queue(struct work_struct *work)
{
	struct omap_dss_device *dssdev = NULL;
	const char *buf = "hdmi";
	int r;
	unsigned long flags;

	int match(struct omap_dss_device *dssdev2 , void *data)
	{
		const char *str = data;
		return sysfs_streq(dssdev2->name , str);
	}
	dssdev = omap_dss_find_device((void *)buf , match);
	DSSDBG("found hdmi handle %s" , dssdev->name);

	spin_lock_irqsave(&irqstatus_lock, flags);
	r = irqstatus;
	irqstatus = 0;
	spin_unlock_irqrestore(&irqstatus_lock, flags);

	DSSDBG("irqstatus=%08x\n hdp_mode = %d dssdev->state = %d, "
		"hdmi_power = %d", r, hpd_mode, dssdev->state, hdmi_power);

	if ((r & HDMI_DISCONNECT) && (hdmi_power == HDMI_POWER_FULL) && (hpd_mode == 1)) {
		set_hdmi_hot_plug_status(dssdev, false);
		/* ignore return value for now */

		/*
		 * WORKAROUND: wait before turning off HDMI.  This may give
		 * audio/video enough time to stop operations.  However, if
		 * user reconnects HDMI, response will be delayed.
		 */
		mdelay(1000);

		DSSINFO("Display disabled\n");
		HDMI_W1_StopVideoFrame(HDMI_WP);
		if (dssdev->platform_disable)
			dssdev->platform_disable(dssdev);
			dispc_enable_digit_out(0);
		HDMI_W1_SetWaitPllPwrState(HDMI_WP, HDMI_PLLPWRCMD_ALLOFF);
		edid_set = false;
		is_hdmi_on = false;
		is_hpd_on = true; /* keep HPD */
		hdmi_enable_clocks(0);
		hdmi_set_power(dssdev);
		hpd_mode = 1;
	}

	if ((r & HDMI_CONNECT) && (hpd_mode == 1) &&
		(hdmi_power != HDMI_POWER_FULL)) {

		DSSINFO("Physical Connect\n");

		/* turn on clocks on connect */
		hdmi_enable_clocks(1);
		is_hdmi_on = true;
		hdmi_set_power(dssdev);
		mdelay(1000);
	}

	if ((r & HDMI_HPD) && (hpd_mode == 1)) {

		mdelay(1000);

		/*
		 * HDMI should already be full on. We use this to read EDID
		 * the first time we enable HDMI via HPD.
		 */
		if (!user_hpd_state) {
			DSSINFO("Connect 1 - Enabling display\n");
			hdmi_enable_clocks(1);
			is_hdmi_on = true;
			hdmi_set_power(dssdev);
		}

		set_hdmi_hot_plug_status(dssdev, true);
		/* ignore return value for now */
	}

	kfree(work);
}

static irqreturn_t hdmi_irq_handler(int irq, void *arg)
{
	struct work_struct *work;
	unsigned long flags;
	int r = 0;
	int work_pending;

	HDMI_W1_HPD_handler(&r);
	DSSDBG("r=%08x, prev irqstatus=%08x\n", r, irqstatus);

	if (((r & HDMI_CONNECT) || (r & HDMI_HPD)) &&  (hpd_mode == 1))
		hdmi_enable_clocks(1);


	spin_lock_irqsave(&irqstatus_lock, flags);
	work_pending = irqstatus;
	if (r & HDMI_DISCONNECT)
		irqstatus &= ~HDMI_CONNECT;
	if (r & HDMI_CONNECT)
		irqstatus &= ~HDMI_DISCONNECT;
	irqstatus |= r;
	spin_unlock_irqrestore(&irqstatus_lock, flags);

	if (r && !work_pending) {
		work = kmalloc(sizeof(struct work_struct), GFP_KERNEL);

		if (work) {
			INIT_WORK(work, hdmi_work_queue);
			schedule_work(work);
		} else {
			printk(KERN_ERR "Cannot allocate memory to create work");
		}
	}

	return IRQ_HANDLED;
}

static void hdmi_power_off(struct omap_dss_device *dssdev)
{
	HDMI_W1_StopVideoFrame(HDMI_WP);

	hdmi_power = HDMI_POWER_OFF;
	dispc_enable_digit_out(0);

	hdmi_phy_off(HDMI_WP);

	HDMI_W1_SetWaitPllPwrState(HDMI_WP, HDMI_PLLPWRCMD_ALLOFF);

	if (dssdev->platform_disable)
		dssdev->platform_disable(dssdev);

	edid_set = false;
	hdmi_enable_clocks(0);

	set_hdmi_hot_plug_status(dssdev, false);
	/* ignore return value for now */

	/*
	 * WORKAROUND: wait before turning off HDMI.  This may give
	 * audio/video enough time to stop operations.  However, if
	 * user reconnects HDMI, response will be delayed.
	 */
	mdelay(1000);

	/* cut clock(s) */
	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
	dss_mainclk_state_disable(true);

	/* reset to default */

}

static int hdmi_start_display(struct omap_dss_device *dssdev)
{
	/* the tv overlay manager is shared*/
	int r = omap_dss_start_device(dssdev);
	if (r) {
		DSSERR("failed to start device\n");
		goto err;
	}

	free_irq(OMAP44XX_IRQ_DSS_HDMI, NULL);

	/* PAD0_HDMI_HPD_PAD1_HDMI_CEC */
	omap_writel(0x01100110, 0x4A100098);
	/* PAD0_HDMI_DDC_SCL_PAD1_HDMI_DDC_SDA */
	omap_writel(0x01100110 , 0x4A10009C);
	/* CONTROL_HDMI_TX_PHY */
	omap_writel(0x10000000, 0x4A100610);

	if (dssdev->platform_enable)
		dssdev->platform_enable(dssdev);

	/* enable clock(s) */
	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
	dss_mainclk_state_enable();

	r = hdmi_set_power(dssdev);
	if (r) {
		DSSERR("failed to power on device\n");
		goto err;
	}
	r = request_irq(OMAP44XX_IRQ_DSS_HDMI, hdmi_irq_handler,
			0, "OMAP HDMI", (void *)0);

err:
	return r;
}

static int hdmi_enable_hpd(struct omap_dss_device *dssdev)
{
	int r = 0;
	DSSDBG("ENTER hdmi_enable_hpd()\n");

	is_hpd_on = true;
	if (dssdev->state == OMAP_DSS_DISPLAY_SUSPENDED)
		dssdev->activate_after_resume = true;

	if (dssdev->state != OMAP_DSS_DISPLAY_DISABLED)
		return 0;

	mutex_lock(&hdmi.lock);

	hpd_mode = 1;
	r = hdmi_start_display(dssdev);
	if (r)
		hpd_mode = 0;

	mutex_unlock(&hdmi.lock);
	return r;
}

static int hdmi_enable_display(struct omap_dss_device *dssdev)
{
	int r = 0;
	bool was_hdmi_on = is_hdmi_on;

	DSSDBG("ENTER hdmi_enable_display()\n");

	is_hdmi_on = is_hpd_on = true;
	if (dssdev->state == OMAP_DSS_DISPLAY_SUSPENDED)
		return 0;

	mutex_lock(&hdmi.lock);

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE)
		/* turn on full power if HDMI was only on HPD */
		r = was_hdmi_on ? 0 : hdmi_set_power(dssdev);
	else
		r = hdmi_start_display(dssdev);

	if (!r)
		set_hdmi_hot_plug_status(dssdev, true);
		/* ignore return value for now */

	mutex_unlock(&hdmi.lock);

	return r;
}

static void hdmi_stop_display(struct omap_dss_device *dssdev)
{
	omap_dss_stop_device(dssdev);

	hdmi_power_off(dssdev);
}

static void hdmi_disable_display(struct omap_dss_device *dssdev)
{
	DSSDBG("Enter hdmi_disable_display()\n");

	mutex_lock(&hdmi.lock);

	is_hdmi_on = is_hpd_on = false;

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE)
		hdmi_stop_display(dssdev);

	/*setting to default only in case of disable and not suspend*/
	hdmi.code = 16;
	hdmi.mode = 1 ;
	hpd_mode = 0;
	mutex_unlock(&hdmi.lock);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static int hdmi_display_suspend(struct omap_dss_device *dssdev)
{
	DSSDBG("hdmi_display_suspend\n");

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
		return -EINVAL;

	mutex_lock(&hdmi.lock);

	hdmi_stop_display(dssdev);

	mutex_unlock(&hdmi.lock);

	dssdev->state = OMAP_DSS_DISPLAY_SUSPENDED;

	return 0;
}

static int hdmi_display_resume(struct omap_dss_device *dssdev)
{
	int r = 0;

	DSSDBG("hdmi_display_resume\n");
	if (dssdev->state != OMAP_DSS_DISPLAY_SUSPENDED)
		return -EINVAL;

	mutex_lock(&hdmi.lock);

	r = hdmi_start_display(dssdev);
	if (!r && hdmi_power == HDMI_POWER_FULL)
		set_hdmi_hot_plug_status(dssdev, true);

	mutex_unlock(&hdmi.lock);

	return r;
}

/* set power state depending on device state and HDMI state */
static int hdmi_set_power(struct omap_dss_device *dssdev)
{
	int r = 0;

	/* do not change power state if suspended */
	if (dssdev->state == OMAP_DSS_DISPLAY_SUSPENDED)
		return 0;

	if (is_hdmi_on)
		r = hdmi_power_on(dssdev);
	else if (is_hpd_on)
		r = hdmi_min_enable();
	else
		hdmi_power_off(dssdev);

	return r;
}

static void hdmi_get_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings)
{
	*timings = dssdev->panel.timings;
}

static void hdmi_set_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings)
{
	DSSDBG("hdmi_set_timings\n");

	dssdev->panel.timings = *timings;

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE) {
		/* turn the hdmi off and on to get new timings to use */
		hdmi_disable_display(dssdev);
		hdmi_enable_display(dssdev);
	}
}

static void hdmi_set_custom_edid_timing_code(struct omap_dss_device *dssdev, int code , int mode)
{
		if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE) {
		/* turn the hdmi off and on to get new timings to use */
		hdmi_disable_display(dssdev);
		hdmi.code = code;
		hdmi.mode = mode;
		custom_set = 1;
		hdmi_enable_display(dssdev);
		custom_set = 0;
	}
}

static struct hdmi_cm hdmi_get_code(struct omap_video_timings *timing)
{
	int i = 0, code = -1, temp_vsync = 0, temp_hsync = 0;
	int timing_vsync = 0, timing_hsync = 0;
	struct omap_video_timings temp;
	struct hdmi_cm cm = {-1};
	DSSDBG("hdmi_get_code");

	for (i = 0; i < OMAP_HDMI_TIMINGS_NB; i++) {
		temp = all_timings_direct[i];
		if ((temp.pixel_clock == timing->pixel_clock) &&
			(temp.x_res == timing->x_res) &&
			(temp.y_res == timing->y_res)) {

			temp_hsync = temp.hfp + temp.hsw + temp.hbp;
			timing_hsync = timing->hfp + timing->hsw + timing->hbp;
			temp_vsync = temp.vfp + temp.vsw + temp.vbp;
			timing_vsync = timing->vfp + timing->vsw + timing->vbp;

			printk("Temp_hsync = %d , temp_vsync = %d , \
				timing_hsync = %d, timing_vsync = %d", \
				temp_hsync, temp_hsync, timing_hsync, timing_vsync);

			if ((temp_hsync == timing_hsync)  &&  (temp_vsync == timing_vsync)) {
				code = i;
				cm.code = code_index[i];
				if (code < OMAP_HDMI_TIMINGS_VESA_START)
					cm.mode = 1;
				else
					cm.mode = 0;
				DSSDBG("Hdmi_code = %d mode = %d\n", cm.code, cm.mode);
				print_omap_video_timings(&temp);
				break;
			 }
		}

	}
	return cm;
}

static int hdmi_get_edid(struct omap_dss_device *dssdev,  u8 *buf, int len)
{
	u8 i = 0, flag = 0, mark = 0;
	int count, offset, effective_addrs, current_descriptor_addrs = 0;
	struct HDMI_EDID * edid_st = (struct HDMI_EDID *)edid;
	struct image_format *img_format;
	struct audio_format *aud_format;
	struct deep_color *vsdb_format;
	struct latency *lat;
	struct omap_video_timings timings;

	img_format = kzalloc(sizeof(*img_format), GFP_KERNEL);
	aud_format = kzalloc(sizeof(*aud_format), GFP_KERNEL);
	vsdb_format = kzalloc(sizeof(*vsdb_format), GFP_KERNEL);
	lat = kzalloc(sizeof(*lat), GFP_KERNEL);

	if (edid_set != 1) {
		printk(KERN_WARNING "Display doesnt seem to be enabled invalid read\n");
	if (HDMI_CORE_DDC_READEDID(HDMI_CORE_SYS, edid, HDMI_EDID_MAX_LENGTH) != 0) {
		printk(KERN_WARNING "HDMI failed to read E-EDID\n");
	}
			for (i = 0x00; i < 0x08; i++) {
				if (edid[i] == header[i])
					continue;
				else {
					flag = 1;
					break;
				}
			}
		if (flag == 0)
			edid_set = 1;
	}

	mdelay(1000);

	printk("\nHeader:\n");
	for (i = 0x00; i < 0x08; i++)
		printk("%02x	", edid[i]);
	printk("\nVendor & Product:\n");
	for (i = 0x08; i < 0x12; i++)
		printk("%02x	", edid[i]);
	printk("\nEDID Structure:\n");
	for (i = 0x12; i < 0x14; i++)
		printk("%02x	", edid[i]);
	printk("\nBasic Display Parameter:\n");
	for (i = 0x14; i < 0x19; i++)
		printk("%02x	", edid[i]);
	printk("\nColor Characteristics:\n");
	for (i = 0x19; i < 0x23; i++)
		printk("%02x	", edid[i]);
	printk("\nEstablished timings:\n");
	for (i = 0x23; i < 0x26; i++)
		printk("%02x	", edid[i]);
	printk("\nStandard timings:\n");
	for (i = 0x26; i < 0x36; i++)
		printk("%02x	", edid[i]);

	for (count = 0; count < EDID_SIZE_BLOCK0_TIMING_DESCRIPTOR; count++) {
		current_descriptor_addrs =
			EDID_DESCRIPTOR_BLOCK0_ADDRESS +
			count * EDID_TIMING_DESCRIPTOR_SIZE;
			printk("Extension 0 Block %d", count);
			get_edid_timing_info(&edid_st->DTD[count], &timings);
			if (!dss_debug) {
				dss_debug = 1;
				mark = 1;
			}
			print_omap_video_timings(&timings);
			if (mark)
				dss_debug = 0;
		}
	if (edid[0x7e] != 0x00) {
		offset = edid[EDID_DESCRIPTOR_BLOCK1_ADDRESS + 2];
		printk("\n offset %x\n", offset);
		if (offset != 0) {
			effective_addrs = EDID_DESCRIPTOR_BLOCK1_ADDRESS
				+ offset;
			/*to determine the number of descriptor blocks */
			for (count = 0;
			      count < EDID_SIZE_BLOCK1_TIMING_DESCRIPTOR;
			      count++) {
			current_descriptor_addrs = effective_addrs +
			count * EDID_TIMING_DESCRIPTOR_SIZE;
			printk("Extension 1 Block %d", count);
			get_eedid_timing_info(current_descriptor_addrs, edid ,
			&timings);
			if (!dss_debug) {
				dss_debug = 1;
				mark = 1;
			}
			print_omap_video_timings(&timings);
			if (mark)
				dss_debug = 0;
			}
		}
	}
	hdmi_get_image_format(edid, img_format);
	printk("%d audio length\n", img_format->length);
	for (i = 0 ; i < img_format->length ; i++)
		printk("%d %d pref code\n", img_format->fmt[i].pref, img_format->fmt[i].code);

	hdmi_get_audio_format(edid, aud_format);
	printk("%d audio length\n", aud_format->length);
	for (i = 0 ; i < aud_format->length ; i++)
		printk("%d %d format num_of_channels\n", aud_format->fmt[i].format,
		aud_format->fmt[i].num_of_ch);

	hdmi_deep_color_support_info(edid, vsdb_format);
	printk("%d deep color bit 30 %d  deep color 36 bit %d max tmds freq",
		vsdb_format->bit_30, vsdb_format->bit_36, vsdb_format->max_tmds_freq);

	hdmi_get_av_delay(edid, lat);
	printk("%d vid_latency %d aud_latency %d interlaced vid latency"
		"%d interlaced aud latency", lat->vid_latency, lat->aud_latency,
		lat->int_vid_latency, lat->int_aud_latency);

	printk("YUV supported %d", hdmi_tv_yuv_supported(edid));

	kfree(img_format);
	kfree(aud_format);
	kfree(vsdb_format);
	kfree(lat);
	memcpy(buf, edid, len);
	return 0;
}


static int hdmi_check_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings)
{
	DSSDBG("hdmi_check_timings\n");

	if (memcmp(&dssdev->panel.timings, timings, sizeof(*timings)) == 0)
		return 0;

	return -EINVAL;
}

int hdmi_init_display(struct omap_dss_device *dssdev)
{
	printk("init_display\n");

	/* register HDMI specific sysfs files */
	/* note: custom_edid_timing should perhaps be moved here too,
	 * instead of generic code?  Or edid sysfs file should be moved
	 * to generic code.. either way they should be in same place..
	 */
	if (device_create_file(&dssdev->dev, &dev_attr_edid))
		DSSERR("failed to create sysfs file\n");
	if (device_create_file(&dssdev->dev, &dev_attr_yuv))
		DSSERR("failed to create sysfs file\n");
	if (device_create_file(&dssdev->dev, &dev_attr_deepcolor))
		DSSERR("failed to create sysfs file\n");
	if (device_create_file(&dssdev->dev, &dev_attr_lr_fr))
		DSSERR("failed to create sysfs file\n");

	return 0;
}

static int hdmi_read_edid(struct omap_video_timings *dp)
{
	int r = 0, ret, code;

	memset(edid, 0, HDMI_EDID_MAX_LENGTH);

	if (!edid_set) {
		ret = HDMI_CORE_DDC_READEDID(HDMI_CORE_SYS, edid, HDMI_EDID_MAX_LENGTH);
	}
	if (ret != 0)
		printk(KERN_WARNING "HDMI failed to read E-EDID\n");
	else {
		if (!memcmp(edid, header, sizeof(header))) {
			/* search for timings of default resolution */
			if (get_edid_timing_data((struct HDMI_EDID *) edid))
				edid_set = true;
		}

	}

	if (!edid_set) {
		DSSDBG("fallback to VGA\n");
		hdmi.code = 4; /*setting default value of 640 480 VGA*/
		hdmi.mode = 0;
	}
	code = get_timings_index();

	*dp = all_timings_direct[code];

	DSSDBG(KERN_INFO"hdmi read EDID:\n");
	print_omap_video_timings(dp);

	return r;
}


/*------------------------------------------------------------------------------
 | Function    : get_edid_timing_data
 +------------------------------------------------------------------------------
 | Description : This function gets the resolution information from EDID
 |
 | Parameters  : void
 |
 | Returns     : void
 +----------------------------------------------------------------------------*/
static int get_edid_timing_data(struct HDMI_EDID *edid)
{
	u8 count, code, offset = 0, effective_addrs = 0, current_descriptor_addrs = 0;
	struct hdmi_cm cm;
	/* Seach block 0, there are 4 DTDs arranged in priority order */
	for (count = 0; count < EDID_SIZE_BLOCK0_TIMING_DESCRIPTOR; count++) {
		get_edid_timing_info(&edid->DTD[count], &edid_timings);
		DSSDBG("Block0 [%d] timings:", count);
		print_omap_video_timings(&edid_timings);
		cm = hdmi_get_code(&edid_timings);
		DSSDBG("Block0[%d] value matches code = %d , mode = %d",\
			count, cm.code, cm.mode);
		if (cm.code == -1)
			continue;
		else {
			hdmi.code = cm.code;
			hdmi.mode = cm.mode;
			DSSDBG("code = %d , mode = %d", hdmi.code, hdmi.mode);
			return 1;
		}

	}
	if (edid->extension_edid != 0x00) {
		offset = edid->offset_dtd;
		if (offset != 0)
			effective_addrs = EDID_DESCRIPTOR_BLOCK1_ADDRESS
				+ offset;
		for (count = 0; count < EDID_SIZE_BLOCK1_TIMING_DESCRIPTOR; count++) {
			current_descriptor_addrs =
				effective_addrs + count * EDID_TIMING_DESCRIPTOR_SIZE;
			get_eedid_timing_info(current_descriptor_addrs, (u8 *)edid,\
							&edid_timings);
			cm = hdmi_get_code(&edid_timings);
			DSSDBG("Block1[%d] value matches code = %d , mode = %d",\
				count, cm.code, cm.mode);
			if (cm.code == -1)
				continue;
			else {
				hdmi.code = cm.code;
				hdmi.mode = cm.mode;
				DSSDBG("code = %d , mode = %d", hdmi.code, hdmi.mode);
				return 1;
			}

		}
	}
	/*As last resort, check for best standard timing supported:*/
	if (edid->timing_1 & 0x01) {
		DSSDBG("800x600@60Hz\n");
		hdmi.mode = 0;
		hdmi.code = 9;
		return 1;
	}
	if (edid->timing_2 & 0x08) {
		DSSDBG("1024x768@60Hz\n");
		hdmi.mode = 0;
		hdmi.code = 16;
		return 1;
	}

	hdmi.code = 4; /*setting default value of 640 480 VGA*/
	hdmi.mode = 0;
	code = code_vesa[hdmi.code];
	edid_timings = all_timings_direct[code];
	return 1;

}

bool is_hdmi_interlaced(void)
{
	return hdmi.cfg.interlace;
}
