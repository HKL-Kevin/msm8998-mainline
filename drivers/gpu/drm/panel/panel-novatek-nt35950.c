// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT35950 DriverIC panels driver
 *
 * Copyright (c) 2020 AngeloGioacchino Del Regno
 *                    <angelogioacchino.delregno@somainline.org>
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define MCS_CMD_MAUCCTR			0xF0 /* Manufacturer command enable */
#define MCS_PARAM_SCALER_FUNCTION	0x58

#define MCS_PARAM_DATA_COMPRESSION	0x90
 #define MCS_DATA_COMPRESSION_NONE	0x00
 #define MCS_DATA_COMPRESSION_FBC	0x02
 #define MCS_DATA_COMPRESSION_DSC	0x03

#define MCS_PARAM_DISP_OUTPUT_CTRL	0xb4
 #define MCS_DISP_OUT_SRAM_EN		BIT(0)
 #define MCS_DISP_OUT_VIDEO_MODE	BIT(4)

/* SubPixel Rendering (SPR) */
#define MCS_PARAM_SPR_EN		0xe3
#define MCS_PARAM_SPR_MODE		0xef

#define NT35950_VREG_MAX		6

struct nt35950 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct regulator_bulk_data vregs[NT35950_VREG_MAX];
	struct gpio_desc *reset_gpio;
	const struct nt35950_panel_desc *desc;

	bool prepared;
};

struct nt35950_panel_desc {
	const char* model_name;
	const struct mipi_dsi_device_info dsi_info;
	const struct drm_display_mode *modes;
	u8 num_lanes;

	bool enable_sram;
	bool is_video_mode;
	bool is_dual_dsi;
};

static inline struct nt35950 *to_nt35950(struct drm_panel *panel)
{
	return container_of(panel, struct nt35950, panel);
}

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void nt35950_reset(struct nt35950 *nt)
{
	gpiod_set_value_cansleep(nt->reset_gpio, 1);
	usleep_range(12000, 13000);
	gpiod_set_value_cansleep(nt->reset_gpio, 0);
	usleep_range(300, 400);
	gpiod_set_value_cansleep(nt->reset_gpio, 1);
	usleep_range(12000, 13000);
}

/*
 * nt35950_set_cmd2_page - Select manufacturer control (CMD2) page
 * @nt:   Main driver structure
 * @page: Page number (0-7)
 *
 * Returns: Number of transferred bytes or negative number on error
 */
static int nt35950_set_cmd2_page(struct nt35950 *nt, u8 page)
{
	const u8 mauc_cmd2_page[] = { MCS_CMD_MAUCCTR, 0x55, 0xaa, 0x52,
				      0x08, page };

	return mipi_dsi_dcs_write_buffer(nt->dsi[0], mauc_cmd2_page,
					 ARRAY_SIZE(mauc_cmd2_page));
}

/*
 * nt35950_set_data_compression - Set data compression mode
 * @nt:        Main driver structure
 * @comp_mode: Compression mode
 *
 * Returns: Number of transferred bytes or negative number on error
 */
static int nt35950_set_data_compression(struct nt35950 *nt, u8 comp_mode)
{
	u8 cmd_data_compression[] = { MCS_PARAM_DATA_COMPRESSION, comp_mode };

	return mipi_dsi_dcs_write_buffer(nt->dsi[0], cmd_data_compression,
					 ARRAY_SIZE(cmd_data_compression));
}

/*
 * nt35950_set_scaler - Enable/disable resolution upscaling
 * @nt:        Main driver structure
 * @comp_mode: Compression mode
 *
 * Returns: Number of transferred bytes or negative number on error
 */
static int nt35950_set_scaler(struct nt35950 *nt, u8 scale_up)
{
	u8 cmd_scaler[] = { MCS_PARAM_SCALER_FUNCTION, scale_up };

	return mipi_dsi_dcs_write_buffer(nt->dsi[0], cmd_scaler,
					 ARRAY_SIZE(cmd_scaler));
}

static int nt35950_inject_black_image(struct nt35950 *nt)
{
	const u8 cmd0_black_img[] = { 0x6f, 0x01 };
	const u8 cmd1_black_img[] = { 0xf3, 0x10 };
	u8 cmd_test[] = { 0xff, 0xaa, 0x55, 0xa5, 0x80 };
	int ret, test_sz = ARRAY_SIZE(cmd_test);

	/* Enable test command */
	ret = mipi_dsi_dcs_write_buffer(nt->dsi[0], cmd_test, test_sz);
	if (ret < 0)
		return ret;

	/* Send a black image */
	ret = mipi_dsi_dcs_write_buffer(nt->dsi[0], cmd0_black_img,
					ARRAY_SIZE(cmd0_black_img));
	if (ret < 0)
		return ret;
	ret = mipi_dsi_dcs_write_buffer(nt->dsi[0], cmd1_black_img,
					ARRAY_SIZE(cmd1_black_img));
	if (ret < 0)
		return ret;

	/* Disable test command */
	cmd_test[test_sz - 1] = 0x00;
	return mipi_dsi_dcs_write_buffer(nt->dsi[0], cmd_test, test_sz);
}

static const int nt35950_test_cmd_en[] = { 0xff, 0xaa, 0x55, 0xa5, 0x80 };

/*
 * nt35950_set_dispout - Set Display Output register parameters
 * @nt:    Main driver structure
 *
 * Returns: Number of transferred bytes or negative number on error
 */
static int nt35950_set_dispout(struct nt35950 *nt)
{
	u8 cmd_dispout[] = { MCS_PARAM_DISP_OUTPUT_CTRL, 0x00 };

	if (nt->desc->is_video_mode)
		cmd_dispout[1] |= MCS_DISP_OUT_VIDEO_MODE;
	if (nt->desc->enable_sram)
		cmd_dispout[1] |= MCS_DISP_OUT_SRAM_EN;

	return mipi_dsi_dcs_write_buffer(nt->dsi[0], cmd_dispout,
					 ARRAY_SIZE(cmd_dispout));
}

static int nt35950_on(struct nt35950 *nt)
{
	struct mipi_dsi_device *dsi = nt->dsi[0];
	struct device *dev = &dsi->dev;
	int ret;

	nt->dsi[0]->mode_flags |= MIPI_DSI_MODE_LPM;
	nt->dsi[1]->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = nt35950_set_cmd2_page(nt, 7);
	if (ret < 0)
		return ret;

	/* Enable SubPixel Rendering */
	dsi_dcs_write_seq(dsi, MCS_PARAM_SPR_EN, 0x01);

	/* SPR Mode: YYG Rainbow-RGB */
	dsi_dcs_write_seq(dsi, MCS_PARAM_SPR_MODE, 0x01);

	ret = nt35950_set_cmd2_page(nt, 0);
	if (ret < 0)
		return ret;

	/* This is unknown... */
	dsi_dcs_write_seq(dsi, 0xc9, 0x01);

	ret = nt35950_set_data_compression(nt, MCS_DATA_COMPRESSION_NONE);
	if (ret < 0)
		return ret;

	ret = nt35950_set_scaler(nt, 1);
	if (ret < 0)
		return ret;

	ret = nt35950_set_dispout(nt);
	if (ret < 0)
		return ret;

	/* Frame rate setting for 60hz */
	dsi_dcs_write_seq(dsi, 0xbd,
			  0x00, 0xac, 0x0c, 0x0c, 0x00,
			  0x01, 0x56, 0x09, 0x09, 0x01,
			  0x01, 0x0c, 0x0c, 0x00, 0xd9);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_tear_scanline(dsi, 0);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear scanline: %d\n", ret);
		return ret;
	}

	/* CMD2 Page 1 */
	ret = nt35950_set_cmd2_page(nt, 1);
	if (ret < 0)
		return ret;

	dsi_dcs_write_seq(dsi, 0xd4, 0x88, 0x88);

	/* CMD3 */
	ret = nt35950_inject_black_image(nt);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;
	msleep(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;
	msleep(120);

	nt->dsi[0]->mode_flags &= ~MIPI_DSI_MODE_LPM;
	nt->dsi[1]->mode_flags &= ~MIPI_DSI_MODE_LPM;

	return 0;
}

static int nt35950_off(struct nt35950 *nt)
{
	struct device *dev = &nt->dsi[0]->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(nt->dsi[0]);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);

	ret = mipi_dsi_dcs_enter_sleep_mode(nt->dsi[0]);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(150);

	nt->dsi[0]->mode_flags |= MIPI_DSI_MODE_LPM;
	nt->dsi[1]->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

/* This function has to be refactored to grab stuff from panel declaration */
static int nt35950_sharp_init_vregs(struct nt35950 *nt, struct device *dev)
{
	int ret;

	nt->vregs[0].supply = "vddio";
	nt->vregs[1].supply = "tvddio";
	nt->vregs[2].supply = "tavdd";
	nt->vregs[3].supply = "avdd";
	nt->vregs[4].supply = "avee";
	nt->vregs[5].supply = "dvdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(nt->vregs),
				      nt->vregs);
	if (ret < 0) {
		dev_err(dev, "Failed to get regulators: %d\n", ret);
		return ret;
	}

	ret = regulator_is_supported_voltage(nt->vregs[0].consumer,
					     1750000, 1950000);
	if (!ret)
		return ret;
	ret = regulator_is_supported_voltage(nt->vregs[1].consumer,
					     1750000, 1950000);
	if (!ret)
		return ret;
	ret = regulator_is_supported_voltage(nt->vregs[2].consumer,
					     2800000, 3300000);
	if (!ret)
		return ret;
	ret = regulator_is_supported_voltage(nt->vregs[3].consumer,
					     5200000, 5900000);
	if (!ret)
		return ret;
	/* AVEE is negative: -5.90V to -5.20V */
	ret = regulator_is_supported_voltage(nt->vregs[4].consumer,
					     5200000, 5900000);
	if (!ret)
		return ret;

	return regulator_is_supported_voltage(nt->vregs[5].consumer,
					     1300000, 1400000);
}

static int nt35950_prepare(struct drm_panel *panel)
{
	struct nt35950 *nt = to_nt35950(panel);
	struct device *dev = &nt->dsi[0]->dev;
	int ret;

	if (nt->prepared)
		return 0;

	/* vio */
	ret = regulator_enable(nt->vregs[0].consumer);
	if (ret)
		return ret;
	usleep_range(2000, 5000);

	/* DVDD */
	ret = regulator_enable(nt->vregs[5].consumer);
	if (ret)
		return ret;
	usleep_range(15000, 18000);

	/* vsp/vsn*/
	ret = regulator_enable(nt->vregs[3].consumer);
	if (ret)
		return ret;
	ret = regulator_enable(nt->vregs[4].consumer);
	if (ret)
		return ret;
	usleep_range(12000, 13000);

	/* touch - remove me */
	ret = regulator_enable(nt->vregs[1].consumer);
	if (ret)
		return ret;
	ret = regulator_enable(nt->vregs[2].consumer);
	if (ret)
		return ret;
	usleep_range(15000, 16000);

	nt35950_reset(nt);

	ret = nt35950_on(nt);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		regulator_bulk_disable(ARRAY_SIZE(nt->vregs), nt->vregs);
		return ret;
	}

	nt->prepared = true;
	return 0;
}

static int nt35950_unprepare(struct drm_panel *panel)
{
	struct nt35950 *nt = to_nt35950(panel);
	struct device *dev = &nt->dsi[0]->dev;
	int ret;

	if (!nt->prepared)
		return 0;

	ret = nt35950_off(nt);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(nt->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(nt->vregs), nt->vregs);

	nt->prepared = false;
	return 0;
}

static const struct drm_display_mode nt35950_mode = {
	/* TODO: Declare 4k */
	.name = "1080x1920",
	.clock = (1080 + 400 + 40 + 300) * (1920 + 12 + 2 + 10) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 400,
	.hsync_end = 1080 + 400 + 40,
	.htotal = 1080 + 400 + 40 + 300,
	.vdisplay = 1920,
	.vsync_start = 1920 + 12,
	.vsync_end = 1920 + 12 + 2,
	.vtotal = 1920 + 12 + 2 + 10,
	.width_mm = 68,
	.height_mm = 121,
};

static int nt35950_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &nt35950_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs nt35950_panel_funcs = {
	.prepare = nt35950_prepare,
	.unprepare = nt35950_unprepare,
	.get_modes = nt35950_get_modes,
};

static int nt35950_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *dsi_r;
	struct mipi_dsi_host *dsi_r_host;
	struct nt35950 *nt;
	const struct mipi_dsi_device_info *info;
	int i, n = 1, ret;

	nt = devm_kzalloc(dev, sizeof(*nt), GFP_KERNEL);
	if (!nt)
		return -ENOMEM;

	ret = nt35950_sharp_init_vregs(nt, dev);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(dev, "Regulator init failure: %d -- NO FAILURE: DEVELOPMENT MODE\n", ret);

	nt->desc = of_device_get_match_data(dev);
	if (!nt->desc)
		return -ENODEV;

	nt->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(nt->reset_gpio)) {
		ret = PTR_ERR(nt->reset_gpio);
		dev_err(dev, "Failed to get reset-gpios: %d\n", ret);
		return ret;
	}

	/* If the panel is connected on two DSIs then DSI0 left, DSI1 right */
	if (nt->desc->is_dual_dsi) {
		info = &nt->desc->dsi_info;
		dsi_r = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);
		if (!dsi_r) {
			dev_err(dev, "Cannot get secondary DSI node.\n");
			return -ENODEV;
		}
		dsi_r_host = of_find_mipi_dsi_host_by_node(dsi_r);
		of_node_put(dsi_r);
		if (!dsi_r_host) {
			dev_err(dev, "Cannot get secondary DSI host\n");
			return -EPROBE_DEFER;
		}

		nt->dsi[1] = mipi_dsi_device_register_full(dsi_r_host, info);
		if (!nt->dsi[1]) {
			dev_err(dev, "Cannot get secondary DSI node\n");
			return -ENODEV;
		}
		n++;
	}

	nt->dsi[0] = dsi;
	mipi_dsi_set_drvdata(dsi, nt);

	drm_panel_init(&nt->panel, dev, &nt35950_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&nt->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&nt->panel);

	for (i = 0; i < n; i++) {
		nt->dsi[i]->lanes = nt->desc->num_lanes;
		nt->dsi[i]->format = MIPI_DSI_FMT_RGB888;

		nt->dsi[i]->mode_flags = MIPI_DSI_MODE_EOT_PACKET |
					 MIPI_DSI_CLOCK_NON_CONTINUOUS |
					 MIPI_DSI_MODE_LPM;

		if (nt->desc->is_video_mode)
			nt->dsi[i]->mode_flags |= MIPI_DSI_MODE_VIDEO;

		ret = mipi_dsi_attach(nt->dsi[i]);
		if (ret < 0) {
			dev_err(dev, "Failed to attach to DSI%d host: %d\n",
				i, ret);
			return ret;
		}
	}

	/* Make sure that before the power sequence starts RESX is LOW */
	gpiod_set_value_cansleep(nt->reset_gpio, 0);
	return 0;
}

static int nt35950_remove(struct mipi_dsi_device *dsi)
{
	struct nt35950 *nt = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(nt->dsi[0]);
	if (ret < 0)
		dev_err(&dsi->dev,
			"Failed to detach from DSI0 host: %d\n", ret);

	if (nt->dsi[1]) {
		ret = mipi_dsi_detach(nt->dsi[1]);
		if (ret < 0)
			dev_err(&dsi->dev,
				"Failed to detach from DSI1 host: %d\n", ret);
		mipi_dsi_device_unregister(nt->dsi[1]);
	};

	drm_panel_remove(&nt->panel);

	return 0;
}

static const struct drm_display_mode sharp_ls055d1sx04_modes = {
	/* TODO: Declare 2160x3840 mode when FBC/DSC will be working. */
	.name = "1080x1920",
	.clock = (1080 + 400 + 40 + 300) * (1920 + 12 + 2 + 10) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 400,
	.hsync_end = 1080 + 400 + 40,
	.htotal = 1080 + 400 + 40 + 300,
	.vdisplay = 1920,
	.vsync_start = 1920 + 12,
	.vsync_end = 1920 + 12 + 2,
	.vtotal = 1920 + 12 + 2 + 10,
	.width_mm = 68,
	.height_mm = 121,
};

const struct nt35950_panel_desc sharp_ls055d1sx04 = {
	.model_name = "Sharp LS055D1SX04",
	.dsi_info = {
		.type = "LS055D1SX04",
		.channel = 0,
		.node = NULL,
	},
	.modes = &sharp_ls055d1sx04_modes,
	.num_lanes = 4,
	.enable_sram = true,
	.is_video_mode = false,
	.is_dual_dsi = true,
};

static const struct of_device_id nt35950_of_match[] = {
	{
		.compatible = "sharp,ls055d1sx04",
		.data = &sharp_ls055d1sx04,
	},

	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nt35950_of_match);

static struct mipi_dsi_driver nt35950_driver = {
	.probe = nt35950_probe,
	.remove = nt35950_remove,
	.driver = {
		.name = "panel-novatek-nt35950",
		.of_match_table = nt35950_of_match,
	},
};
module_mipi_dsi_driver(nt35950_driver);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@somainline.org>");
MODULE_DESCRIPTION("Novatek NT35950 DriverIC panels driver");
MODULE_LICENSE("GPL v2");
