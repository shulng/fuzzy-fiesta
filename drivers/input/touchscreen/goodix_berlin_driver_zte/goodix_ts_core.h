#ifndef _GOODIX_TS_CORE_H_
#define _GOODIX_TS_CORE_H_
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <asm/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/of_irq.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#endif
#ifdef CONFIG_FB
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

#define GOODIX_CORE_DRIVER_NAME		"goodix_ts"
#define GOODIX_PEN_DRIVER_NAME		"goodix_ts,pen"
#define GOODIX_DRIVER_VERSION		"v1.1.6"
#define GOODIX_MAX_TOUCH		10
#define GOODIX_PEN_MAX_PRESSURE		4096
#define GOODIX_MAX_PEN_KEY		2
#define GOODIX_CFG_MAX_SIZE		4096
#define GOODIX_MAX_STR_LABLE_LEN	32

#define GOODIX_NORMAL_RESET_DELAY_MS	100
#define GOODIX_HOLD_CPU_RESET_DELAY_MS	5

#define ZTE_GOODIX_SIN_TAP	0x01
#define ZTE_GOODIX_DOU_TAP	0x02
#define ZTE_GOODIX_FP		0x04
#define ZTE_GOODIX_ONE_KEY	0x08

#define GSX_DOUBLE_TAP	0xcc
#define GSX_SINGLE_TAP 0x4c
#define GSX_FP_TAP_AOD 0x46
#define GSX_FP_UP_AOD  0x55
#define GSX_FP_TAP     0x88

#define GOODIX_GESTURE_DATA_START			0x00
#define GOODIX_GESTURE_DATA_OPEN_DOUBLE		0x10
#define GOODIX_GESTURE_DATA_CLOSE_DOUBLE	0x80
#define GOODIX_GESTURE_CMD					0x12

#define GOODIX_MAX_CMD_NUM	9
#define GOODIX_CMD_LEN			5

#define GOODIX_USB_DETECT_GLOBAL

#define RAWDATA_PRINT_COUNT				60

struct tp_point_log {
	unsigned int x;
	unsigned int y;
};

enum CORD_PROB_STA {
	CORE_MODULE_UNPROBED = 0,
	CORE_MODULE_PROB_SUCCESS = 1,
	CORE_MODULE_PROB_FAILED = -1,
	CORE_MODULE_REMOVED = -2,
};

enum GOODIX_ERR_CODE {
	GOODIX_EBUS	  = (1<<0),
	GOODIX_ECHECKSUM = (1<<1),
	GOODIX_EVERSION  = (1<<2),
	GOODIX_ETIMEOUT  = (1<<3),
	GOODIX_EMEMCMP   = (1<<4),

	GOODIX_EOTHER	= (1<<7)
};

enum IC_TYPE_ID {
	IC_TYPE_NONE,
	IC_TYPE_NORMANDY,
	IC_TYPE_NANJING,
	IC_TYPE_YELLOWSTONE,
	IC_TYPE_BERLIN,
};

enum GOODIX_IC_CONFIG_TYPE {
	CONFIG_TYPE_TEST = 0,
	CONFIG_TYPE_NORMAL = 1,
	CONFIG_TYPE_HIGHSENSE = 2,
	CONFIG_TYPE_CHARGER = 3,
	CONFIG_TYPE_CHARGER_HS = 4,
	CONFIG_TYPE_HOLSTER = 5,
	CONFIG_TYPE_HOSTER_CH = 6,
	CONFIG_TYPE_OTHER = 7,
	/* keep this at the last */
	GOODIX_MAX_CONFIG_GROUP = 8,
};

enum CHECKSUM_MODE {
	CHECKSUM_MODE_U8_LE,
	CHECKSUM_MODE_U16_LE,
};

#define MAX_SCAN_FREQ_NUM			5
#define MAX_SCAN_RATE_NUM			5
#define MAX_FREQ_NUM_STYLUS		  8
#define MAX_STYLUS_SCAN_FREQ_NUM	 6
#pragma pack(1)
struct goodix_fw_version {
	u8 rom_pid[6];			   /* rom PID */
	u8 rom_vid[3];			   /* Mask VID */
	u8 rom_vid_reserved;
	u8 patch_pid[8];			  /* Patch PID */
	u8 patch_vid[4];			  /* Patch VID */
	u8 patch_vid_reserved;
	u8 sensor_id;
	u8 reserved[2];
	u16 checksum;
};

struct goodix_ic_info_version {
	u8 info_customer_id;
	u8 info_version_id;
	u8 ic_die_id;
	u8 ic_version_id;
	u32 config_id;
	u8 config_version;
	u8 frame_data_customer_id;
	u8 frame_data_version_id;
	u8 touch_data_customer_id;
	u8 touch_data_version_id;
	u8 reserved[3];
};

struct goodix_ic_info_feature { /* feature info*/
	u16 freqhop_feature;
	u16 calibration_feature;
	u16 gesture_feature;
	u16 side_touch_feature;
	u16 stylus_feature;
};

struct goodix_ic_info_param { /* param */
	u8 drv_num;
	u8 sen_num;
	u8 button_num;
	u8 force_num;
	u8 active_scan_rate_num;
	u16 active_scan_rate[MAX_SCAN_RATE_NUM];
	u8 mutual_freq_num;
	u16 mutual_freq[MAX_SCAN_FREQ_NUM];
	u8 self_tx_freq_num;
	u16 self_tx_freq[MAX_SCAN_FREQ_NUM];
	u8 self_rx_freq_num;
	u16 self_rx_freq[MAX_SCAN_FREQ_NUM];
	u8 stylus_freq_num;
	u16 stylus_freq[MAX_FREQ_NUM_STYLUS];
};

struct goodix_ic_info_misc { /* other data */
	u32 cmd_addr;
	u16 cmd_max_len;
	u32 cmd_reply_addr;
	u16 cmd_reply_len;
	u32 fw_state_addr;
	u16 fw_state_len;
	u32 fw_buffer_addr;
	u16 fw_buffer_max_len;
	u32 frame_data_addr;
	u16 frame_data_head_len;
	u16 fw_attr_len;
	u16 fw_log_len;
	u8 pack_max_num;
	u8 pack_compress_version;
	u16 stylus_struct_len;
	u16 mutual_struct_len;
	u16 self_struct_len;
	u16 noise_struct_len;
	u32 touch_data_addr;
	u16 touch_data_head_len;
	u16 point_struct_len;
	u16 reserved1;
	u16 reserved2;
	u32 mutual_rawdata_addr;
	u32 mutual_diffdata_addr;
	u32 mutual_refdata_addr;
	u32 self_rawdata_addr;
	u32 self_diffdata_addr;
	u32 self_refdata_addr;
	u32 iq_rawdata_addr;
	u32 iq_refdata_addr;
	u32 im_rawdata_addr;
	u16 im_readata_len;
	u32 noise_rawdata_addr;
	u16 noise_rawdata_len;
	u32 stylus_rawdata_addr;
	u16 stylus_rawdata_len;
	u32 noise_data_addr;
	u32 esd_addr;
};

struct goodix_ic_info {
	u16 length;
	struct goodix_ic_info_version version;
	struct goodix_ic_info_feature feature;
	struct goodix_ic_info_param parm;
	struct goodix_ic_info_misc misc;
};
#pragma pack()

/*
 * struct ts_rawdata_info
 *
 */
#define TS_RAWDATA_BUFF_MAX             56000
#define TS_RAWDATA_RESULT_MAX           100
struct ts_rawdata_info {
	int used_size; /*fill in rawdata size*/
	s16 buff[TS_RAWDATA_BUFF_MAX];
	char result[TS_RAWDATA_RESULT_MAX];
};

/*
 * struct goodix_module - external modules container
 * @head: external modules list
 * @initilized: whether this struct is initilized
 * @mutex: mutex lock
 * @wq: workqueue to do register work
 * @core_data: core_data pointer
 */
struct goodix_module {
	struct list_head head;
	bool initilized;
	struct mutex mutex;
	struct workqueue_struct *wq;
	struct completion core_comp;
	struct goodix_ts_core *core_data;
};

/*
 * struct goodix_ts_board_data -  board data
 * @avdd_enable_gpio: num of avdd gpio
 * @iovdd_name: name of analoy regulator
 * @reset_gpio: reset gpio number
 * @irq_gpio: interrupt gpio number
 * @irq_flag: irq trigger type
 * @swap_axis: whether swaw x y axis
 * @panel_max_x/y/w/p: resolution and size
 * @pannel_key_map: key map
 * @fw_name: name of the firmware image
 */
struct goodix_ts_board_data {
	char iovdd_name[GOODIX_MAX_STR_LABLE_LEN];
	unsigned int avdd_enable_gpio;
	int reset_gpio;
	int irq_gpio;
	int avdd_gpio;
	int iovdd_gpio;
	unsigned int  irq_flags;

	unsigned int swap_axis;
	unsigned int panel_max_x;
	unsigned int panel_max_y;
	unsigned int panel_max_w; /*major and minor*/
	unsigned int panel_max_p; /*pressure*/

	bool pen_enable;
	const char *fw_name;
	const char *cfg_bin_name;
	unsigned int edge_suppress_cmd[GOODIX_MAX_CMD_NUM * GOODIX_CMD_LEN];
};

enum goodix_fw_update_mode {
	UPDATE_MODE_DEFAULT = 0,
	UPDATE_MODE_FORCE = (1<<0), /* force update mode */
	UPDATE_MODE_BLOCK = (1<<1), /* update in block mode */
	UPDATE_MODE_FLASH_CFG = (1<<2), /* reflash config */
	UPDATE_MODE_SRC_SYSFS = (1<<4), /* firmware file from sysfs */
	UPDATE_MODE_SRC_HEAD = (1<<5), /* firmware file from head file */
	UPDATE_MODE_SRC_REQUEST = (1<<6), /* request firmware */
	UPDATE_MODE_SRC_ARGS = (1<<7), /* firmware data from function args */
};

#define MAX_CMD_DATA_LEN 10
#define MAX_CMD_BUF_LEN  16
#pragma pack(1)
struct goodix_ts_cmd {
	union {
		struct {
			u8 state;
			u8 ack;
			u8 len;
			u8 cmd;
			u8 data[MAX_CMD_DATA_LEN];
		};
		u8 buf[MAX_CMD_BUF_LEN];
	};
};
#pragma pack()

/* interrupt event type */
enum ts_event_type {
	EVENT_INVALID = 0,
	EVENT_TOUCH = (1 << 0), /* finger touch event */
	EVENT_PEN = (1 << 1),   /* pen event */
	EVENT_REQUEST = (1 << 2),
	EVENT_GESTURE = (1 << 3),
};

enum ts_request_type {
	REQUEST_TYPE_CONFIG = 1,
	REQUEST_TYPE_RESET = 3,
};

/* notifier event */
enum ts_notify_event {
	NOTIFY_FWUPDATE_START,
	NOTIFY_FWUPDATE_FAILED,
	NOTIFY_FWUPDATE_SUCCESS,
	NOTIFY_SUSPEND,
	NOTIFY_RESUME,
	NOTIFY_ESD_OFF,
	NOTIFY_ESD_ON,
	NOTIFY_CFG_BIN_FAILED,
	NOTIFY_CFG_BIN_SUCCESS,
};

enum touch_point_status {
	TS_NONE,
	TS_RELEASE,
	TS_TOUCH,
};
/* coordinate package */
struct goodix_ts_coords {
	int status; /* NONE, RELEASE, TOUCH */
	unsigned int x, y, w, p;
};

struct goodix_pen_coords {
	int status; /* NONE, RELEASE, TOUCH */
	int tool_type;  /* BTN_TOOL_RUBBER BTN_TOOL_PEN */
	unsigned int x, y, p;
	signed char tilt_x;
	signed char tilt_y;
};

/* touch event data */
struct goodix_touch_data {
	int touch_num;
	struct goodix_ts_coords coords[GOODIX_MAX_TOUCH];
};

struct goodix_ts_key {
	int status;
	int code;
};

struct goodix_pen_data {
	struct goodix_pen_coords coords;
	struct goodix_ts_key keys[GOODIX_MAX_PEN_KEY];
};

/*
 * struct goodix_ts_event - touch event struct
 * @event_type: touch event type, touch data or
 *	request event
 * @event_data: event data
 */
struct goodix_ts_event {
	int retry;
	enum ts_event_type event_type;
	u8 request_code; /* represent the request type */
	u8 gesture_type;
	struct goodix_touch_data touch_data;
	struct goodix_pen_data pen_data;
};

enum goodix_ic_bus_type {
	GOODIX_BUS_TYPE_I2C,
	GOODIX_BUS_TYPE_SPI,
	GOODIX_BUS_TYPE_I3C,
};

struct goodix_bus_interface {
	int bus_type;
	struct device *dev;
	int (*read)(struct device *dev, unsigned int addr,
			 unsigned char *data, unsigned int len);
	int (*write)(struct device *dev, unsigned int addr,
			unsigned char *data, unsigned int len);
};

struct goodix_ts_hw_ops {
	int (*power_on)(struct goodix_ts_core *cd, bool on);
	int (*dev_confirm)(struct goodix_ts_core *cd);
	int (*resume)(struct goodix_ts_core *cd);
	int (*suspend)(struct goodix_ts_core *cd);
	int (*gesture)(struct goodix_ts_core *cd, int gesture_type_first, int gesture_type_second);
	int (*reset)(struct goodix_ts_core *cd, int delay_ms);
	int (*irq_enable)(struct goodix_ts_core *cd, bool enable);
	int (*read)(struct goodix_ts_core *cd, unsigned int addr,
			unsigned char *data, unsigned int len);
	int (*write)(struct goodix_ts_core *cd, unsigned int addr,
			 unsigned char *data, unsigned int len);
	int (*send_cmd)(struct goodix_ts_core *cd, struct goodix_ts_cmd *cmd);
	int (*send_config)(struct goodix_ts_core *cd, u8 *config, int len);
	int (*read_config)(struct goodix_ts_core *cd, u8 *config_data, int size);
	int (*read_version)(struct goodix_ts_core *cd, struct goodix_fw_version *version);
	int (*get_ic_info)(struct goodix_ts_core *cd, struct goodix_ic_info *ic_info, bool isPrint);
	int (*esd_check)(struct goodix_ts_core *cd);
	int (*event_handler)(struct goodix_ts_core *cd, struct goodix_ts_event *ts_event);
	int (*after_event_handler)(struct goodix_ts_core *cd); /* clean sync flag */
	int (*get_capacitance_data)(struct goodix_ts_core *cd, struct ts_rawdata_info *info);
	int (*set_horizontal_panel)(struct goodix_ts_core *cd, int cmd);
	int (*set_vertical_panel)(struct goodix_ts_core *cd);
	int (*set_enter_charger)(struct goodix_ts_core *cd);
	int (*set_leave_charger)(struct goodix_ts_core *cd);
	int (*set_finger_onekey_switch)(struct goodix_ts_core *cd, int finger_type, int onekey_type);
#if defined(ZTE_FINGERAREA_A30PRO)
	int (*set_enter_play_game)(struct goodix_ts_core *cd);
	int (*set_leave_play_game)(struct goodix_ts_core *cd);
#endif
};

/*
 * struct goodix_ts_esd - esd protector structure
 * @esd_work: esd delayed work
 * @esd_on: 1 - turn on esd protection, 0 - turn
 *  off esd protection
 */
struct goodix_ts_esd {
	bool irq_status;
	atomic_t esd_on;
	struct delayed_work esd_work;
	struct notifier_block esd_notifier;
	struct goodix_ts_core *ts_core;
};

enum goodix_core_init_stage {
	CORE_UNINIT,
	CORE_INIT_FAIL,
	CORE_INIT_STAGE1,
	CORE_INIT_STAGE2
};

struct goodix_ic_config {
	int len;
	u8 data[GOODIX_CFG_MAX_SIZE];
};

struct zte_ctl {
	int is_single_tap;
	int is_single_aod;
	int is_single_fp;
	int is_set_single_in_suspend;
	int is_wakeup_gesture;
	int is_set_wakeup_in_suspend;
	int is_sensibility;
	int is_smart_cover;
	int is_wake_irq;
	int is_one_key;
	int is_set_onekey_in_suspend;
	int is_play_game;
	int level;
	struct mutex rawdata_read_lock;
};

struct goodix_ts_core {
	int init_stage;
	struct platform_device *pdev;
	struct goodix_fw_version fw_version;
	struct goodix_ic_info ic_info;
	struct goodix_bus_interface *bus;
	struct goodix_ts_board_data board_data;
	struct goodix_ts_hw_ops *hw_ops;
	struct input_dev *input_dev;
	struct input_dev *pen_dev;
	/* TODO counld we remove this from core data? */
	struct goodix_ts_event ts_event;

	/* every pointer of this array represent a kind of config */
	struct goodix_ic_config *ic_configs[GOODIX_MAX_CONFIG_GROUP];
	int avdd_enable_gpio;
	struct regulator *avdd;
	struct regulator *iovdd;

	int power_on;
	int irq;
	size_t irq_trig_cnt;

	struct zte_ctl ztec;

	atomic_t irq_enabled;
	atomic_t suspended;
	/* when this flag is true, driver should not clean the sync flag */
	bool tools_ctrl_sync;

	struct notifier_block ts_notifier;
	struct goodix_ts_esd ts_esd;

#ifdef GOODIX_USB_DETECT_GLOBAL
	struct delayed_work charger_work;
	struct workqueue_struct *charger_wq;
#endif

#ifdef GOODIX_USB_DETECT_GLOBAL
	struct notifier_block charger_notifier;
#endif

#if defined(CONFIG_FB) || defined(CONFIG_DRM)
	struct notifier_block fb_notifier;
#endif
	struct mutex mutex_cmd;
};

/* external module structures */
enum goodix_ext_priority {
	EXTMOD_PRIO_RESERVED = 0,
	EXTMOD_PRIO_FWUPDATE,
	EXTMOD_PRIO_GESTURE,
	EXTMOD_PRIO_HOTKNOT,
	EXTMOD_PRIO_DBGTOOL,
	EXTMOD_PRIO_DEFAULT,
};

#define EVT_HANDLED			0
#define EVT_CONTINUE			0
#define EVT_CANCEL			1
#define EVT_CANCEL_IRQEVT		1
#define EVT_CANCEL_SUSPEND		1
#define EVT_CANCEL_RESUME		1
#define EVT_CANCEL_RESET		1

struct goodix_ext_module;
/* external module's operations callback */
struct goodix_ext_module_funcs {
	int (*init)(struct goodix_ts_core *core_data,
			struct goodix_ext_module *module);
	int (*exit)(struct goodix_ts_core *core_data,
			struct goodix_ext_module *module);
	int (*before_reset)(struct goodix_ts_core *core_data,
				struct goodix_ext_module *module);
	int (*after_reset)(struct goodix_ts_core *core_data,
			   struct goodix_ext_module *module);
	int (*before_suspend)(struct goodix_ts_core *core_data,
				  struct goodix_ext_module *module);
	int (*after_suspend)(struct goodix_ts_core *core_data,
				 struct goodix_ext_module *module);
	int (*before_resume)(struct goodix_ts_core *core_data,
				 struct goodix_ext_module *module);
	int (*after_resume)(struct goodix_ts_core *core_data,
				struct goodix_ext_module *module);
	int (*irq_event)(struct goodix_ts_core *core_data,
			 struct goodix_ext_module *module);
	int (*set_edge_suppress)(int *cmd_start,
			struct goodix_ts_core *core_data,
			 struct goodix_ext_module *module);
};

/*
 * struct goodix_ext_module - external module struct
 * @list: list used to link into modules manager
 * @name: name of external module
 * @priority: module priority vlaue, zero is invalid
 * @funcs: operations callback
 * @priv_data: private data region
 * @kobj: kobject
 * @work: used to queue one work to do registration
 */
struct goodix_ext_module {
	struct list_head list;
	char *name;
	enum goodix_ext_priority priority;
	const struct goodix_ext_module_funcs *funcs;
	void *priv_data;
	struct kobject kobj;
	struct work_struct work;
};

/*
 * struct goodix_ext_attribute - exteranl attribute struct
 * @attr: attribute
 * @show: show interface of external attribute
 * @store: store interface of external attribute
 */
struct goodix_ext_attribute {
	struct attribute attr;
	ssize_t (*show)(struct goodix_ext_module *, char *);
	ssize_t (*store)(struct goodix_ext_module *, const char *, size_t);
};

/* external attrs helper macro */
#define __EXTMOD_ATTR(_name, _mode, _show, _store)	{	\
	.attr = {.name = __stringify(_name), .mode = _mode },	\
	.show   = _show,	\
	.store  = _store,	\
}

/* external attrs helper macro, used to define external attrs */
#define DEFINE_EXTMOD_ATTR(_name, _mode, _show, _store)	\
static struct goodix_ext_attribute ext_attr_##_name = \
	__EXTMOD_ATTR(_name, _mode, _show, _store);


/*#define CONFIG_GOODIX_DEBUG*/
/* log macro */
#define ts_info(fmt, arg...)	pr_info("[GTP-INF][%s:%d] "fmt"\n", __func__, __LINE__, ##arg)
#define	ts_err(fmt, arg...)	pr_err("[GTP-ERR][%s:%d] "fmt"\n", __func__, __LINE__, ##arg)
#ifdef CONFIG_GOODIX_DEBUG
#define ts_debug(fmt, arg...)	pr_info("[GTP-DBG][%s:%d] "fmt"\n", __func__, __LINE__, ##arg)
#else
#define ts_debug(fmt, arg...)	do {} while (0)
#endif

static inline void tp_irq_wake(struct goodix_ts_core *core_data, bool enable)
{
	if (enable && !core_data->ztec.is_wake_irq) {
		enable_irq_wake(core_data->irq);
		core_data->ztec.is_wake_irq = true;
		ts_info("enable_irq_wake");
	} else if (!enable && core_data->ztec.is_wake_irq) {
		disable_irq_wake(core_data->irq);
		core_data->ztec.is_wake_irq = false;
		ts_info("disable_irq_wake");
	}
}

/*
 * get board data pointer
 */
static inline struct goodix_ts_board_data *board_data(
		struct goodix_ts_core *core)
{
	if (!core)
		return NULL;
	return &(core->board_data);
}

/**
 * goodix_register_ext_module - interface for external module
 * to register into touch core modules structure
 *
 * @module: pointer to external module to be register
 * return: 0 ok, <0 failed
 */
int goodix_register_ext_module(struct goodix_ext_module *module);
/* register module no wait */
int goodix_register_ext_module_no_wait(struct goodix_ext_module *module);
/**
 * goodix_unregister_ext_module - interface for external module
 * to unregister external modules
 *
 * @module: pointer to external module
 * return: 0 ok, <0 failed
 */
int goodix_unregister_ext_module(struct goodix_ext_module *module);
/* remove all registered ext module
 * return 0 on success, otherwise return < 0
 */
int goodix_unregister_all_module(void);
int goodix_ts_blocking_notify(enum ts_notify_event evt, void *v);
struct kobj_type *goodix_get_default_ktype(void);
struct kobject *goodix_get_default_kobj(void);
int goodix_get_core_module_state(void);

struct goodix_ts_hw_ops *goodix_get_hw_ops(void);
int goodix_get_config_proc(struct goodix_ts_core *cd);

int goodix_bus_init(void);
void goodix_bus_exit(void);

u32 goodix_append_checksum(u8 *data, int len, int mode);
int checksum_cmp(const u8 *data, int size, int mode);
int is_risk_data(const u8 *data, int size);
u32 goodix_get_file_config_id(u8 *ic_config);
void goodix_rotate_abcd2cbad(int tx, int rx, s16 *data);

int goodix_fw_update_init(struct goodix_ts_core *core_data);
void goodix_fw_update_uninit(void);
int goodix_do_fw_update(struct goodix_ic_config *ic_config, int mode);
int goodix_get_rawdata(struct device *dev, struct ts_rawdata_info *info);

extern struct goodix_ts_core *ts_core_data;

#endif
