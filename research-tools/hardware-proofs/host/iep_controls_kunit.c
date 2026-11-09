// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the actual IEP menu through Linux's V4L2 ioctl helpers, without DMA. */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/rk-iep.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>

/* Generated from the patched driver by tests/iep-controls.sh. */
#include "iep-control-under-test.h"

struct iep_control_test {
	struct v4l2_ctrl_handler handler;
	struct video_device video;
};

static int iep_control_init(struct kunit *test)
{
	struct iep_control_test *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	v4l2_ctrl_handler_init(&ctx->handler, 11);
	for (unsigned int i = 0; i < ARRAY_SIZE(iep_rgb_controls); i++)
		v4l2_ctrl_new_custom(&ctx->handler, &iep_rgb_controls[i], NULL);
	v4l2_ctrl_new_custom(&ctx->handler, &iep_filter_control, NULL);
	v4l2_ctrl_new_custom(&ctx->handler, &iep_radius_control, NULL);
	v4l2_ctrl_new_custom(&ctx->handler, &iep_threshold_control, NULL);
	v4l2_ctrl_new_std(&ctx->handler, NULL, V4L2_CID_BRIGHTNESS, -32, 31, 1, 0);
	ret = ctx->handler.error;
	if (ret) {
		v4l2_ctrl_handler_free(&ctx->handler);
		return ret;
	}
	test->priv = ctx;
	return 0;
}

static void iep_control_exit(struct kunit *test)
{
	struct iep_control_test *ctx = test->priv;

	v4l2_ctrl_handler_free(&ctx->handler);
}

static void expect_value(struct kunit *test, u32 id, int value)
{
	struct iep_control_test *ctx = test->priv;
	struct v4l2_control ctrl = { .id = id };
	struct v4l2_ctrl *internal = v4l2_ctrl_find(&ctx->handler, id);

	KUNIT_ASSERT_EQ(test, v4l2_g_ctrl(&ctx->handler, &ctrl), 0);
	KUNIT_EXPECT_EQ(test, ctrl.value, value);
	KUNIT_ASSERT_NOT_NULL(test, internal);
	/* This is the actual worker's snapshot helper, not just G_CTRL's view. */
	v4l2_ctrl_lock(internal);
	KUNIT_EXPECT_EQ(test, iep_ctrl_value(internal), value);
	v4l2_ctrl_unlock(internal);
}

static void iep_control_enumeration(struct kunit *test)
{
	struct iep_control_test *ctx = test->priv;
	struct v4l2_query_ext_ctrl query = { .id = V4L2_CID_RK_IEP_FILTER };
	struct v4l2_querymenu menu = { .id = V4L2_CID_RK_IEP_FILTER };
	unsigned int value;

	KUNIT_ASSERT_EQ(test, v4l2_query_ext_ctrl(&ctx->handler, &query), 0);
	KUNIT_EXPECT_EQ(test, query.type, (u32)V4L2_CTRL_TYPE_MENU);
	KUNIT_EXPECT_EQ(test, query.minimum, (s64)RK_IEP_FILTER_BYPASS);
	KUNIT_EXPECT_EQ(test, query.maximum, (s64)RK_IEP_FILTER_EDGE);
	KUNIT_EXPECT_EQ(test, query.default_value, (s64)RK_IEP_FILTER_BYPASS);
	KUNIT_EXPECT_FALSE(test, query.flags & V4L2_CTRL_FLAG_DISABLED);
	KUNIT_EXPECT_FALSE(test, query.flags & V4L2_CTRL_FLAG_READ_ONLY);
	KUNIT_ASSERT_EQ(test, v4l2_querymenu(&ctx->handler, &menu), 0);
	KUNIT_EXPECT_STREQ(test, (char *)menu.name, "Bypass");
	for (value = RK_IEP_FILTER_DENOISE; value <= RK_IEP_FILTER_EDGE; value++) {
		menu.index = value;
		KUNIT_EXPECT_EQ(test, v4l2_querymenu(&ctx->handler, &menu), 0);
	}
	menu.index = RK_IEP_FILTER_EDGE + 1;
	KUNIT_EXPECT_EQ(test, v4l2_querymenu(&ctx->handler, &menu), -EINVAL);
}

static void iep_control_legacy(struct kunit *test)
{
	struct iep_control_test *ctx = test->priv;
	struct v4l2_control ctrl = { .id = V4L2_CID_RK_IEP_FILTER };
	static const int invalid[] = { -1, RK_IEP_FILTER_EDGE + 1, INT_MAX };
	unsigned int i;

	KUNIT_ASSERT_EQ(test, v4l2_s_ctrl(NULL, &ctx->handler, &ctrl), 0);
	for (i = RK_IEP_FILTER_DENOISE; i <= RK_IEP_FILTER_EDGE; i++) {
		ctrl.value = i;
		KUNIT_ASSERT_EQ(test, v4l2_s_ctrl(NULL, &ctx->handler, &ctrl), 0);
		expect_value(test, ctrl.id, i);
	}
	ctrl.value = RK_IEP_FILTER_BYPASS;
	KUNIT_ASSERT_EQ(test, v4l2_s_ctrl(NULL, &ctx->handler, &ctrl), 0);
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		ctrl.value = invalid[i];
		KUNIT_EXPECT_EQ(test, v4l2_s_ctrl(NULL, &ctx->handler, &ctrl), -ERANGE);
		expect_value(test, ctrl.id, RK_IEP_FILTER_BYPASS);
	}
}

static void iep_control_extended(struct kunit *test)
{
	struct iep_control_test *ctx = test->priv;
	struct v4l2_ext_control ctrl = { .id = V4L2_CID_RK_IEP_FILTER };
	struct v4l2_ext_controls batch = { .count = 1, .controls = &ctrl };
	int ret, value;

	KUNIT_ASSERT_EQ(test, v4l2_try_ext_ctrls(&ctx->handler, &ctx->video, NULL, &batch), 0);
	KUNIT_ASSERT_EQ(test, v4l2_s_ext_ctrls(NULL, &ctx->handler, &ctx->video, NULL, &batch), 0);
	for (value = 4; value <= 5; value++) {
		ctrl.value = value;
		KUNIT_EXPECT_EQ(test, v4l2_try_ext_ctrls(&ctx->handler, &ctx->video, NULL, &batch),
				-ERANGE);
		KUNIT_EXPECT_EQ(test, batch.error_idx, 0U);
		ret = v4l2_s_ext_ctrls(NULL, &ctx->handler, &ctx->video, NULL, &batch);
		KUNIT_EXPECT_EQ(test, ret, -ERANGE);
		/* S_EXT_CTRLS reports count for validation errors before commit. */
		KUNIT_EXPECT_EQ(test, batch.error_idx, batch.count);
		expect_value(test, ctrl.id, RK_IEP_FILTER_BYPASS);
	}
}

static void iep_control_batch_atomicity(struct kunit *test)
{
	struct iep_control_test *ctx = test->priv;
	struct v4l2_ext_control ctrls[] = {
		{ .id = V4L2_CID_BRIGHTNESS, .value = 12 },
		{ .id = V4L2_CID_RK_IEP_FILTER },
	};
	struct v4l2_ext_controls batch = { .count = 2, .controls = ctrls };
	int ret, value;

	for (value = 4; value <= 5; value++) {
		ctrls[1].value = value;
		KUNIT_EXPECT_EQ(test, v4l2_try_ext_ctrls(&ctx->handler, &ctx->video, NULL, &batch),
				-ERANGE);
		KUNIT_EXPECT_EQ(test, batch.error_idx, 1U);
		ret = v4l2_s_ext_ctrls(NULL, &ctx->handler, &ctx->video, NULL, &batch);
		KUNIT_EXPECT_EQ(test, ret, -ERANGE);
		KUNIT_EXPECT_EQ(test, batch.error_idx, batch.count);
		expect_value(test, V4L2_CID_BRIGHTNESS, 0);
		expect_value(test, V4L2_CID_RK_IEP_FILTER, RK_IEP_FILTER_BYPASS);
	}
	ctrls[1].value = RK_IEP_FILTER_BYPASS;
	KUNIT_ASSERT_EQ(test, v4l2_try_ext_ctrls(&ctx->handler, &ctx->video, NULL, &batch), 0);
	expect_value(test, V4L2_CID_BRIGHTNESS, 0);
	expect_value(test, V4L2_CID_RK_IEP_FILTER, RK_IEP_FILTER_BYPASS);
	KUNIT_ASSERT_EQ(test, v4l2_s_ext_ctrls(NULL, &ctx->handler, &ctx->video, NULL, &batch), 0);
	expect_value(test, V4L2_CID_BRIGHTNESS, 12);
	expect_value(test, V4L2_CID_RK_IEP_FILTER, RK_IEP_FILTER_BYPASS);
}

static void iep_control_radius(struct kunit *test)
{
	struct iep_control_test *ctx = test->priv;
	struct v4l2_control ctrl = { .id = V4L2_CID_RK_IEP_RADIUS };
	unsigned int value;

	KUNIT_EXPECT_EQ(test, iep_radius_control.max, 1LL);
	for (value = 0; value <= 1; value++) {
		ctrl.value = value;
		KUNIT_ASSERT_EQ(test, v4l2_s_ctrl(NULL, &ctx->handler, &ctrl), 0);
		expect_value(test, ctrl.id, value);
	}
	/* The shipping interface must not expose the hardware-failed radii. */
	for (value = 2; value <= 3; value++) {
		ctrl.value = value;
		KUNIT_EXPECT_EQ(test, v4l2_s_ctrl(NULL, &ctx->handler, &ctrl), 0);
		KUNIT_EXPECT_EQ(test, ctrl.value, 1);
		expect_value(test, ctrl.id, 1);
	}
}

static void iep_format_sanitization(struct kunit *test)
{
	struct v4l2_pix_format pix = { .pixelformat = 0xffffffff };
	struct v4l2_pix_format source = { .quantization = V4L2_QUANTIZATION_LIM_RANGE };

	/* TRY_FMT must accept an unknown/zero fourcc before normalizing it.
	 * Compliance deliberately does this; the color copy must not deref NULL.
	 */
	iep_copy_color(&pix, &source);
	iep_adjust_fmt(&pix, false);
	KUNIT_EXPECT_EQ(test, pix.pixelformat, (u32)V4L2_PIX_FMT_NV12);
	KUNIT_EXPECT_EQ(test, pix.sizeimage, 16U * 16U * 3U / 2U);
	for (unsigned int i = 0; i < ARRAY_SIZE(iep_formats); i++) {
		pix = (struct v4l2_pix_format) {
			.pixelformat = iep_formats[i].fourcc,
			.width = 68, .height = 36, .bytesperline = 1,
		};
		iep_adjust_fmt(&pix, true);
		KUNIT_EXPECT_GE(test, pix.bytesperline, 68U * iep_formats[i].bpp);
		KUNIT_EXPECT_EQ(test, pix.bytesperline % 8, 0U);
		KUNIT_EXPECT_EQ(test, pix.sizeimage,
			pix.bytesperline * pix.height * (iep_formats[i].vsub ?
			(iep_formats[i].vsub == 2 ? 3U : 4U) : 2U) / 2U);
	}
}

static void iep_compound_defaults(struct kunit *test)
{
	struct iep_control_test *ctx = test->priv;
	struct v4l2_ctrl *gamma = v4l2_ctrl_find(&ctx->handler, V4L2_CID_RK_IEP_CG_TABLE);
	struct v4l2_ctrl *weights = v4l2_ctrl_find(&ctx->handler, V4L2_CID_RK_IEP_DDE_WEIGHTS);

	KUNIT_ASSERT_NOT_NULL(test, gamma);
	KUNIT_ASSERT_NOT_NULL(test, weights);
	KUNIT_ASSERT_EQ(test, gamma->elems, 768U);
	for (unsigned int i = 0; i < gamma->elems; i++) {
		KUNIT_EXPECT_EQ(test, gamma->p_cur.p_u8[i], (u8)(i % 256));
	}
	{
		u8 defaults[768];
		union v4l2_ctrl_ptr ptr = { .p_u8 = defaults };

		gamma->type_ops->init(gamma, 0, ptr);
		for (unsigned int i = 0; i < 768; i++)
			KUNIT_EXPECT_EQ(test, defaults[i], (u8)(i % 256));
	}
	KUNIT_EXPECT_EQ(test, weights->elems, 337U);
	KUNIT_EXPECT_EQ(test, weights->minimum, 1LL);
	KUNIT_EXPECT_EQ(test, weights->maximum, 63LL);
	for (unsigned int i = 0; i < weights->elems; i++)
		KUNIT_EXPECT_EQ(test, weights->p_cur.p_u8[i], (u8)1);
}

static struct kunit_case iep_control_cases[] = {
	KUNIT_CASE(iep_control_enumeration),
	KUNIT_CASE(iep_control_radius),
	KUNIT_CASE(iep_format_sanitization),
	KUNIT_CASE(iep_compound_defaults),
	KUNIT_CASE(iep_control_legacy),
	KUNIT_CASE(iep_control_extended),
	KUNIT_CASE(iep_control_batch_atomicity),
	{ }
};

static struct kunit_suite iep_control_suite = {
	.name = "rk-iep-controls",
	.init = iep_control_init,
	.exit = iep_control_exit,
	.test_cases = iep_control_cases,
};

kunit_test_suite(iep_control_suite);

MODULE_LICENSE("GPL");
