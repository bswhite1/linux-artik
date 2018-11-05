/*
 * MIPI-DSI based st7796s LCD panel driver.
 *
 * Copyright (c) 2018 Swine Technologies Inc.
 *
 * Adam Magstadt <amagstadt@swinetechnologies.com>
 * Ben White <bwhite@swinetechnologies.com>
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 * Derived from drivers/gpu/drm/panel/panel-s6e8fa0.c
 *
 * Chanho Park <chanho61.park@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>
#include <linux/backlight.h>

#define MIN_BRIGHTNESS				0
#define MAX_BRIGHTNESS				255
#define DEFAULT_BRIGHTNESS			160

struct st7796s {
	struct device *dev;
	struct drm_panel panel;

	struct regulator_bulk_data supplies[2];
	int reset_gpio;
	u32 power_on_delay;
	u32 reset_delay;
	u32 init_delay;
	bool flip_horizontal;
	bool flip_vertical;
	struct videomode vm;
	u32 width_mm;
	u32 height_mm;
	bool is_power_on;

	struct backlight_device *bl_dev;
	u8 id[3];
	/* This field is tested by functions directly accessing DSI bus before
	 * transfer, transfer is skipped if it is set. In case of transfer
	 * failure or unexpected response the field is set to error value.
	 * Such construct allows to eliminate many checks in higher level
	 * functions.
	 */
	int error;
};

static inline struct st7796s *panel_to_st7796s(struct drm_panel *panel)
{
	return container_of(panel, struct st7796s, panel);
}

static int st7796s_clear_error(struct st7796s *ctx)
{
	int ret = ctx->error;

	ctx->error = 0;
	return ret;
}

static void st7796s_dcs_write(struct st7796s *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return;

	ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing dcs seq: %*ph\n", ret,
			(int)len, data);
		ctx->error = ret;
	}
}

static int st7796s_dcs_read(struct st7796s *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->error < 0)
		return ctx->error;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

#define st7796s_dcs_write_seq(ctx, seq...) \
({\
	const u8 d[] = { seq };\
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64, "DCS sequence too big for stack");\
	st7796s_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

#define st7796s_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	st7796s_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static void st7796s_set_maximum_return_packet_size(struct st7796s *ctx,
						   u16 size)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->error < 0)
		return;

	ret = mipi_dsi_set_maximum_return_packet_size(dsi, size);
	if (ret < 0) {
		dev_err(ctx->dev,
			"error %d setting maximum return packet size to %d\n",
			ret, size);
		ctx->error = ret;
	}
}

static void st7796s_read_mtp_id(struct st7796s *ctx)
{
	int ret;
	int id_len = ARRAY_SIZE(ctx->id);

	ret = st7796s_dcs_read(ctx, MIPI_DCS_GET_DISPLAY_ID, ctx->id, id_len);
	if (ret < id_len || ctx->error < 0) {
		dev_err(ctx->dev, "read id failed\n");
		ctx->error = -EIO;
		return;
	}
}

static void st7796s_set_sequence(struct st7796s *ctx)
{
	st7796s_clear_error(ctx);

	st7796s_set_maximum_return_packet_size(ctx, 3);
	st7796s_read_mtp_id(ctx);

	if (ctx->error != 0) {
		dev_err(ctx->dev, "set sequence error\n");
		return;
	}
	u8 mode[4];
	int mode_len = ARRAY_SIZE(mode);

	st7796s_dcs_write_seq_static(ctx, 0x11); // Sleep Out

	msleep(150);

	st7796s_dcs_write_seq_static(ctx, 0xF0, 0xC3);  //Command Set Control Part #1

	st7796s_dcs_write_seq_static(ctx, 0xF0, 0x96);  //Command Set Control Part #2

	st7796s_dcs_write_seq_static(ctx, 0xB6, 0x8A, 0x07, 0x3b);  // Display Function Control

	st7796s_dcs_write_seq_static(ctx, 0xB5, 0x10, 0x04, 0x00, 0x04); //Blanking Porch Control

	st7796s_dcs_write_seq_static(ctx, 0xB1, 0xA0, 0x10); //Frame Rate Control

	st7796s_dcs_write_seq_static(ctx, 0x36, 0x48); // Memory Data Access Control

	st7796s_dcs_write_seq_static(ctx, 0x35, 0x00); // Tearing Effect Line On - v blanking info only

	st7796s_dcs_write_seq_static(ctx, 0xB4, 0x01); // Display Inversion Control - 1-dot inversion

	st7796s_dcs_write_seq_static(ctx, 0xE8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33); // Display Output Ctrl Adjust

	st7796s_dcs_write_seq_static(ctx, 0xC0, 0x80, 0x77); //Power Control 1 - AVDDS: 6.60, AVCLS - -4.4
	                                                     //                - VGHS: 15.467, VGLS: -12.5

	st7796s_dcs_write_seq_static(ctx, 0xC1, 0x06); // Power Control 2, VAP(GVDD)(V) 4.95+( vcom+vcom offset)

	st7796s_dcs_write_seq_static(ctx, 0xC2, 0xA7); // Power Control #3

	st7796s_dcs_write_seq_static(ctx, 0xC5, 0x18); //VCOM Control - 1.100

	st7796s_dcs_write_seq_static(ctx, 0xE0, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B); // Positive Gamma Control

	st7796s_dcs_write_seq_static(ctx, 0xE1, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2D, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B); // Negative Gamma Control

	st7796s_dcs_write_seq_static(ctx, 0x51, 0x64); // Maximum Brightness

	st7796s_dcs_write_seq_static(ctx, 0x53, 0x2C); // Enable Backlight, manual dimming, and the brightness register

	st7796s_dcs_write_seq_static(ctx, 0xE8, 0x40, 0x82, 0x07, 0x18, 0x27, 0x0A, 0xB6, 0x33); // Display Output Ctrl Adjust

	st7796s_dcs_write_seq_static(ctx, 0xB7, 0x46); // Entry Mode

	st7796s_dcs_write_seq_static(ctx, 0x3A, 0x66); //Interface Pixel Format 18 bit rgb, 18 bit control. //0x77); //Interface Pixel Format  // adu0x75

	st7796s_dcs_write_seq_static(ctx, 0x20); //Display Inversion Off

	st7796s_dcs_write_seq_static(ctx, 0x2A, 0x00, 0x00, 0x01, 0x3F); //Column Address Set

	st7796s_dcs_write_seq_static(ctx, 0x2B, 0x00, 0x00, 0x01, 0xE0); //Row Address Set

	st7796s_dcs_write_seq_static(ctx, 0x38); // Idle Mode Off

	st7796s_dcs_write_seq_static(ctx, 0x29); // DISPLAY ON

	st7796s_dcs_write_seq_static(ctx, 0xF0, 0x3C); //Command Set Control - disable command 2 part I

	st7796s_dcs_write_seq_static(ctx, 0xF0, 0x69); //Command Set Control - disable command 2 part II

	msleep(120);

	st7796s_dcs_write_seq_static(ctx, 0x11); // Sleep Out

	msleep(120);

	if (ctx->error != 0) {
		dev_err(ctx->dev, "set sequence error\n");
		return;
	}
}

static int st7796s_get_brightness(struct backlight_device *bl_dev)
{
	return bl_dev->props.brightness;
}

static int st7796s_set_brightness(struct backlight_device *bl_dev)
{
	struct st7796s *ctx = (struct st7796s *)bl_get_data(bl_dev);
	int brightness = bl_dev->props.brightness;
	/* FIXME: MIPI_DSI_DCS_SHORT_WRITE_PARAM is not working properly, the
	 * panel is turned on from Power key source.
	 */

	if (brightness < MIN_BRIGHTNESS ||
		brightness > bl_dev->props.max_brightness) {
		dev_err(ctx->dev, "Invalid brightness: %u\n", brightness);
		return -EINVAL;
	}

	if (!ctx->is_power_on) {
		return -ENODEV;
	}

	u8 brightness_update[2] = {0x51, brightness};
	st7796s_dcs_write(ctx, brightness_update, ARRAY_SIZE(brightness_update));

	return 0;
}

static const struct backlight_ops st7796s_bl_ops = {
	.get_brightness = st7796s_get_brightness,
	.update_status = st7796s_set_brightness,
};

static int st7796s_power_on(struct st7796s *ctx)
{
	int ret;
	if (ctx->is_power_on)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	msleep(ctx->power_on_delay);

	gpio_direction_output(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpio_set_value(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpio_set_value(ctx->reset_gpio, 1);

	msleep(ctx->reset_delay);

	ctx->is_power_on = true;

	return 0;
}

static int st7796s_power_off(struct st7796s *ctx)
{
	if (!ctx->is_power_on)
		return 0;

	gpio_set_value(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	ctx->is_power_on = false;

	return 0;
}

static int st7796s_disable(struct drm_panel *panel)
{
	struct st7796s *ctx = panel_to_st7796s(panel);

	st7796s_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_OFF);
	if (ctx->error != 0)
		return ctx->error;

	msleep(35);

	st7796s_dcs_write_seq_static(ctx, MIPI_DCS_ENTER_SLEEP_MODE);
	if (ctx->error != 0)
		return ctx->error;

	msleep(125);

	return 0;
}

static int st7796s_unprepare(struct drm_panel *panel)
{
	struct st7796s *ctx = panel_to_st7796s(panel);
	int ret;

	ret = st7796s_power_off(ctx);
	if (ret)
		return ret;

	st7796s_clear_error(ctx);

	return 0;
}

static int st7796s_prepare(struct drm_panel *panel)
{
	struct st7796s *ctx = panel_to_st7796s(panel);
	int ret;
	ret = st7796s_power_on(ctx);
	if (ret < 0)
		return ret;

	st7796s_set_sequence(ctx);
	ret = ctx->error;

	if (ret < 0) {
		st7796s_unprepare(panel);
	}

	return ret;
}

static int st7796s_enable(struct drm_panel *panel)
{
	struct st7796s *ctx = panel_to_st7796s(panel);

	st7796s_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_ON);

	if (ctx->error != 0)
		return ctx->error;

	return 0;
}

static int st7796s_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct st7796s *ctx = panel_to_st7796s(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		DRM_ERROR("failed to create a new display mode\n");
		return 0;
	}

	drm_display_mode_from_videomode(&ctx->vm, mode);
	mode->width_mm = ctx->width_mm;
	mode->height_mm = ctx->height_mm;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs st7796s_drm_funcs = {
	.disable = st7796s_disable,
	.unprepare = st7796s_unprepare,
	.prepare = st7796s_prepare,
	.enable = st7796s_enable,
	.get_modes = st7796s_get_modes,
};

static int st7796s_parse_dt(struct st7796s *ctx)
{
	struct device *dev = ctx->dev;
	struct device_node *np = dev->of_node;
	int ret;

	ret = of_get_videomode(np, &ctx->vm, 0);
	if (ret < 0)
		return ret;

	of_property_read_u32(np, "power-on-delay", &ctx->power_on_delay);
	of_property_read_u32(np, "reset-delay", &ctx->reset_delay);
	of_property_read_u32(np, "init-delay", &ctx->init_delay);
	of_property_read_u32(np, "panel-width-mm", &ctx->width_mm);
	of_property_read_u32(np, "panel-height-mm", &ctx->height_mm);

	ctx->flip_horizontal = of_property_read_bool(np, "flip-horizontal");
	ctx->flip_vertical = of_property_read_bool(np, "flip-vertical");

	return 0;
}

static int st7796s_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct st7796s *ctx;
	int ret;

	if (!drm_panel_connected("st7796s"))
		return -ENODEV;

	ctx = devm_kzalloc(dev, sizeof(struct st7796s), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	ctx->is_power_on = false;
	dsi->lanes = 1;
	dsi->format = MIPI_DSI_FMT_RGB666_PACKED;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO;

	ret = st7796s_parse_dt(ctx);
	if (ret < 0)
		return ret;

	ctx->supplies[0].supply = "vdd3";
	ctx->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		dev_warn(dev, "failed to get regulators: %d\n", ret);

	ctx->reset_gpio = of_get_named_gpio(dev->of_node, "reset-gpio", 0);
	if (ctx->reset_gpio < 0) {
		dev_err(dev, "cannot get reset-gpios %d\n",
			ctx->reset_gpio);
		return ctx->reset_gpio;
	}

	ret = devm_gpio_request(dev, ctx->reset_gpio, "reset-gpio");
	if (ret) {
		dev_err(dev, "failed to request reset-gpio\n");
		return ret;
	}

	ctx->bl_dev = backlight_device_register("st7796s", dev, ctx,
						&st7796s_bl_ops, NULL);
	if (IS_ERR(ctx->bl_dev)) {
		dev_err(dev, "failed to register backlight device\n");
		return PTR_ERR(ctx->bl_dev);
	}

	ctx->bl_dev->props.max_brightness = MAX_BRIGHTNESS;
	ctx->bl_dev->props.brightness = DEFAULT_BRIGHTNESS;
	ctx->bl_dev->props.power = FB_BLANK_POWERDOWN;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &st7796s_drm_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0) {
		backlight_device_unregister(ctx->bl_dev);
		return ret;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		backlight_device_unregister(ctx->bl_dev);
		drm_panel_remove(&ctx->panel);
	}

	return ret;
}

static int st7796s_remove(struct mipi_dsi_device *dsi)
{
	struct st7796s *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	backlight_device_unregister(ctx->bl_dev);
	st7796s_power_off(ctx);

	return 0;
}

static void st7796s_shutdown(struct mipi_dsi_device *dsi)
{
	struct st7796s *ctx = mipi_dsi_get_drvdata(dsi);

	st7796s_power_off(ctx);
}

static const struct of_device_id st7796s_of_match[] = {
	{ .compatible = "sitronix,st7796s" },
	{ }
};
MODULE_DEVICE_TABLE(of, st7796s_of_match);

static struct mipi_dsi_driver st7796s_driver = {
	.probe = st7796s_probe,
	.remove = st7796s_remove,
	.shutdown = st7796s_shutdown,
	.driver = {
		.name = "panel-sitronix-st7796s",
		.of_match_table = st7796s_of_match,
	},
};
module_mipi_dsi_driver(st7796s_driver);

MODULE_AUTHOR("Chanho Park <chanho61.park@samsung.com>");
MODULE_AUTHOR("Adam Magstadt <amagstadt@swinetechnologies.com>");
MODULE_AUTHOR("Ben White <bwhite@swinetechnologies.com>");
MODULE_DESCRIPTION("MIPI-DSI based st7796s LCD Panel Driver");
MODULE_LICENSE("GPL v2");
