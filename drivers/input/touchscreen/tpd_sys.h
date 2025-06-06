/*
 * FILE:__TSP_FW_CLASS_H_INCLUDED
 *
 */
#ifndef __TPD_FW_H_INCLUDED
#define __TPD_FW_H_INCLUDED

#include <linux/device.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/input.h>
#include <linux/proc_fs.h>
#include <linux/printk.h>

#ifdef TPD_DMESG
#undef TPD_DMESG
#endif
#define TPD_DMESG(a, arg...) pr_notice("tpd: " a, ##arg)

#define CONFIG_CREATE_TPD_SYS_INTERFACE

#define PROC_TOUCH_DIR					"touchscreen"
#define PROC_TOUCH_INFO					"ts_information"
#define PROC_TOUCH_FW_UPGRADE			"FW_upgrade"
#define PROC_TOUCH_GET_TPRAWDATA		"get_tprawdata"
#define PROC_TOUCH_RW_REG				"rw_reg"
#define PROC_TOUCH_SMART_COVER		"smart_cover"
#define PROC_TOUCH_WAKE_GESTURE		"wake_gesture"
#define PROC_TOUCH_EDGE_REPORT_LIMIT		"edge_report_limit"
#define PROC_TOUCH_TP_SINGLETAP		"single_tap"
#define PROC_TOUCH_GET_NOISE		"get_noise"
#define PROC_TOUCH_SCREEN_STATE		"screen_state_interface"
#define PROC_TOUCH_HEADSET_STATE		"headset_state"
#define PROC_TOUCH_MROTATION		"mRotation"
#define PROC_TOUCH_ONEKEY			"one_key"
#define PROC_TOUCH_PLAY_GAME		"play_game"
#define PROC_TOUCH_SENSIBILITY           "sensibility"

/*gesture for tp, begin*/
#define  KEY_GESTURE_DOUBLE_CLICK 214
#define  MAX_VENDOR_NAME_LEN 20

#ifdef CONFIG_TOUCHSCREEN_GOODIX_GTX8
#define RT_DATA_NUM	(5 << 3)
#else
#define RT_DATA_NUM	(5 << 2)
#endif

/* RT_DATA_LEN < 4096 */
#define RT_DATA_LEN	(4096 - 8)

struct tp_runtime_data {
	char *rt_data;
	bool is_empty;
	struct list_head list;
};

enum ts_chip {
	TS_CHIP_INDETER		= 0x00,
	TS_CHIP_SYNAPTICS	= 0x01,
	TS_CHIP_ATMEL		= 0x02,
	TS_CHIP_CYTTSP		= 0x03,
	TS_CHIP_FOCAL		= 0x04,
	TS_CHIP_GOODIX		= 0x05,
	TS_CHIP_MELFAS		= 0x06,
	TS_CHIP_MSTAR		= 0x07,
	TS_CHIP_HIMAX		= 0x08,
	TS_CHIP_NOVATEK		= 0x09,
	TS_CHIP_ILITEK		= 0x0A,

	TS_CHIP_MAX		= 0xFF,
};

struct tp_rwreg_t {
	int type;		/*  0: read, 1: write */
	int reg;		/*  register */
	int len;		/*  read/write length */
	int val;		/*  length = 1; read: return value, write: op return */
	int res;		/*  0: success, otherwise: fail */
	char *opbuf;	/*  length >= 1, read return value, write: op return */
};
enum {
	REG_OP_READ = 0,
	REG_OP_WRITE = 1,
};

enum {
	REG_CHAR_NUM_2 = 2,
	REG_CHAR_NUM_4 = 4,
	REG_CHAR_NUM_8 = 8,
};

struct tpvendor_t {
	int vendor_id;
	char *vendor_name;
};
/*chip_model_id synaptics 1,atmel 2,cypress 3,focal 4,goodix 5,mefals 6,mstar
7,himax 8;
 *
 */
struct tpd_tpinfo_t {
	unsigned int chip_model_id;
	unsigned int chip_part_id;
	unsigned int chip_ver;
	unsigned int module_id;
	unsigned int firmware_ver;
	unsigned int config_ver;
	unsigned int display_ver;
	unsigned int i2c_addr;
	unsigned int i2c_type;
	char tp_name[MAX_VENDOR_NAME_LEN];
	char vendor_name[MAX_VENDOR_NAME_LEN];
};

struct tpd_classdev_t {
	const char *name;
	int b_force_upgrade;
	int fw_compare_result;
	int b_gesture_enable;
	int b_smart_cover_enable;
	bool TP_have_registered;
	int reg_char_num;
	int tp_is_enable;
	int b_single_tap_enable;
	char *noise_buffer;
	int screen_is_on;
	int headset_state;
	int display_rotation;
	int one_key_enable;
	int edge_limit_level;
	int play_game_enable;
	int sensibility_enable;

	int (*tp_i2c_reg_read)(struct tpd_classdev_t *cdev, char addr, u8 *data, int len);
	int (*tp_i2c_reg_write)(struct tpd_classdev_t *cdev, char addr, u8 *data, int len);
	int (*tp_i2c_16bor32b_reg_read)(struct tpd_classdev_t *cdev, u32 addr, u8 *data, int len);
	int (*tp_i2c_16bor32b_reg_write)(struct tpd_classdev_t *cdev, u32 addr, u8 *data, int len);
	int (*tp_fw_upgrade)(struct tpd_classdev_t *cdev, char *fwname, int fwname_len);
	int (*read_block)(struct tpd_classdev_t *cdev, u16 addr, u8 *buf, int len);
	int (*write_block)(struct tpd_classdev_t *cdev, u16 addr, u8 *buf, int len);
	int (*get_tpinfo)(struct tpd_classdev_t *cdev);
	int (*get_gesture)(struct tpd_classdev_t *cdev);
	int (*wake_gesture)(struct tpd_classdev_t *cdev, int enable);
	int (*get_smart_cover)(struct tpd_classdev_t *cdev);
	int (*set_smart_cover)(struct tpd_classdev_t *cdev, int enable);
	int (*get_edge_limit_level)(struct tpd_classdev_t *cdev);
	int (*set_edge_limit_level)(struct tpd_classdev_t *cdev, u8 level);
	int (*get_singletap)(struct tpd_classdev_t *cdev);
	int (*set_singletap)(struct tpd_classdev_t *cdev, int enable);
	int (*get_noise)(struct tpd_classdev_t *cdev, struct list_head *head);
	int (*get_screen_state)(struct tpd_classdev_t *cdev);
	int (*set_screen_state)(struct tpd_classdev_t *cdev, int enable);
	int (*set_headset_state)(struct tpd_classdev_t *cdev, int enable);
	int (*headset_state_show)(struct tpd_classdev_t *cdev);
	int (*set_display_rotation)(struct tpd_classdev_t *cdev, int mrotation);
	int (*get_one_key)(struct tpd_classdev_t *cdev);
	int (*set_one_key)(struct tpd_classdev_t *cdev, int enable);
	int (*get_play_game)(struct tpd_classdev_t *cdev);
	int (*set_play_game)(struct tpd_classdev_t *cdev, int enable);
	int (*get_sensibility)(struct tpd_classdev_t *cdev);
	int (*set_sensibility)(struct tpd_classdev_t *cdev, int enable);
	void *private;
	void *test_node;    /*added for tp test.*/

	/**for tpd test*/
	int (*tpd_test_set_save_filepath)(struct tpd_classdev_t *cdev, const char *buf);
	int (*tpd_test_get_save_filepath)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_set_save_filename)(struct tpd_classdev_t *cdev, const char *buf);
	int (*tpd_test_get_save_filename)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_set_ini_filepath)(struct tpd_classdev_t *cdev, const char *buf);
	int (*tpd_test_get_ini_filepath)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_set_filename)(struct tpd_classdev_t *cdev, const char *buf);
	int (*tpd_test_get_filename)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_set_cmd)(struct tpd_classdev_t *cdev, const char *buf);
	int (*tpd_test_get_cmd)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_set_node_data_type)(struct tpd_classdev_t *cdev, const char *buf);
	int (*tpd_test_get_node_data)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_get_channel_info)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_get_result)(struct tpd_classdev_t *cdev, char *buf);
	int (*tpd_test_get_tp_rawdata)(char *buffer, int length);
	int (*tpd_gpio_shutdown)(void);
	int (*tpd_test_set_bsc_calibration)(struct tpd_classdev_t *cdev, const char *buf);
	int (*tpd_test_get_bsc_calibration)(struct tpd_classdev_t *cdev, char *buf);

	struct mutex cmd_mutex;
	struct tpd_tpinfo_t ic_tpinfo;
	struct device		*dev;
	struct list_head	 node;
};

extern struct tpd_classdev_t tpd_fw_cdev;
extern int tpd_classdev_register(struct device *parent, struct tpd_classdev_t
*tsp_fw_cdev);
extern const char *zte_get_lcd_panel_name(void);
#endif	/* __TSP_FW_CLASS_H_INCLUDED */

