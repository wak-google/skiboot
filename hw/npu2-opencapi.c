/* Copyright 2013-2018 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Support for OpenCAPI on POWER9 NPUs
 *
 * This file provides support for OpenCAPI as implemented on POWER9.
 *
 * At present, we initialise the NPU separately from the NVLink code in npu2.c.
 * As such, we don't currently support mixed NVLink and OpenCAPI configurations
 * on the same NPU for machines such as Witherspoon.
 *
 * Procedure references in this file are to the POWER9 OpenCAPI NPU Workbook
 * (IBM internal document).
 *
 * TODO:
 *   - Support for mixed NVLink and OpenCAPI on the same NPU
 *   - Support for link ganging (one AFU using multiple links)
 *   - Link reset and error handling
 *   - Presence detection
 *   - Consume HDAT NPU information
 *   - LPC Memory support
 */

#include <skiboot.h>
#include <xscom.h>
#include <io.h>
#include <timebase.h>
#include <pci.h>
#include <pci-cfg.h>
#include <pci-slot.h>
#include <interrupts.h>
#include <opal.h>
#include <opal-api.h>
#include <npu2.h>
#include <npu2-regs.h>
#include <phys-map.h>
#include <xive.h>
#include <i2c.h>
#include <nvram.h>

#define NPU_IRQ_LEVELS		35
#define NPU_IRQ_LEVELS_XSL	23
#define MAX_PE_HANDLE		((1 << 15) - 1)
#define TL_MAX_TEMPLATE		63
#define TL_RATE_BUF_SIZE	32

enum npu2_link_training_state {
	NPU2_TRAIN_DEFAULT, /* fully train the link */
	NPU2_TRAIN_PRBS31,  /* used for Signal Integrity testing */
	NPU2_TRAIN_NONE,    /* used for testing with loopback cable */
};
static enum npu2_link_training_state npu2_ocapi_training_state = NPU2_TRAIN_DEFAULT;

static const struct phb_ops npu2_opencapi_ops;

static inline uint64_t index_to_stack(uint64_t index) {
	switch (index) {
	case 2:
	case 3:
		return NPU2_STACK_STCK_1;
		break;
	case 4:
	case 5:
		return NPU2_STACK_STCK_2;
		break;
	default:
		assert(false);
	}
}

static inline uint64_t index_to_stacku(uint64_t index) {
	switch (index) {
	case 2:
	case 3:
		return NPU2_STACK_STCK_1U;
		break;
	case 4:
	case 5:
		return NPU2_STACK_STCK_2U;
		break;
	default:
		assert(false);
	}
}

static inline uint64_t index_to_block(uint64_t index) {
	switch (index) {
	case 2:
	case 4:
		return NPU2_BLOCK_OTL0;
		break;
	case 3:
	case 5:
		return NPU2_BLOCK_OTL1;
		break;
	default:
		assert(false);
	}
}

static uint64_t get_odl_status(uint32_t gcid, uint64_t index) {
	uint64_t reg, status_xscom;
	switch (index) {
	case 2:
		status_xscom = OB0_ODL0_STATUS;
		break;
	case 3:
		status_xscom = OB0_ODL1_STATUS;
		break;
	case 4:
		status_xscom = OB3_ODL1_STATUS;
		break;
	case 5:
		status_xscom = OB3_ODL0_STATUS;
		break;
	default:
		assert(false);
	}
	xscom_read(gcid, status_xscom, &reg);
	return reg;
}

static void disable_nvlink(uint32_t gcid, int index)
{
	uint64_t phy_config_scom, reg;

	switch (index) {
	case 2:
	case 3:
		phy_config_scom = OBUS_LL0_IOOL_PHY_CONFIG;
		break;
	case 4:
	case 5:
		phy_config_scom = OBUS_LL3_IOOL_PHY_CONFIG;
		break;
	default:
		assert(false);
	}
	/* Disable NV-Link link layers */
	xscom_read(gcid, phy_config_scom, &reg);
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV0_NPU_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV1_NPU_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV2_NPU_ENABLED;
	xscom_write(gcid, phy_config_scom, reg);
}

/* Procedure 13.1.3.1 - select OCAPI vs NVLink for bricks 2-3/4-5 */

static void set_transport_mux_controls(uint32_t gcid, uint32_t scom_base,
				       int index, enum npu2_dev_type type)
{
	/* Step 1 - Set Transport MUX controls to select correct OTL or NTL */
	uint64_t reg;
	uint64_t field;

	/* TODO: Rework this to select for NVLink too */
	assert(type == NPU2_DEV_TYPE_OPENCAPI);

	prlog(PR_DEBUG, "OCAPI: %s: Setting transport mux controls\n", __func__);

	/* Optical IO Transport Mux Config for Bricks 0-2 and 4-5 */
	reg = npu2_scom_read(gcid, scom_base, NPU2_MISC_OPTICAL_IO_CFG0,
			     NPU2_MISC_DA_LEN_8B);
	switch (index) {
	case 0:
	case 1:
		/* not valid for OpenCAPI */
		assert(false);
		break;
	case 2:	 /* OTL1.0 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg);
		field &= ~0b100;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg,
			       field);
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg);
		field |= 0b10;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg,
			       field);
		break;
	case 3:	 /* OTL1.1 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg);
		field &= ~0b010;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg,
			       field);
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg);
		field |= 0b01;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg,
			       field);
		break;
	case 4:	 /* OTL2.0 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg);
		field |= 0b10;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg,
			       field);
		break;
	case 5:	 /* OTL2.1 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg);
		field |= 0b01;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg,
			       field);
		break;
	default:
		assert(false);
	}
	npu2_scom_write(gcid, scom_base, NPU2_MISC_OPTICAL_IO_CFG0,
			NPU2_MISC_DA_LEN_8B, reg);

	/*
	 * PowerBus Optical Miscellaneous Config Register - select
	 * OpenCAPI for b4/5 and A-Link for b3
	 */
	xscom_read(gcid, PU_IOE_PB_MISC_CFG, &reg);
	switch (index) {
	case 0:
	case 1:
	case 2:
	case 3:
		break;
	case 4:
		reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_04_NPU_NOT_PB, reg, 1);
		break;
	case 5:
		reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_05_NPU_NOT_PB, reg, 1);
		break;
	}
	xscom_write(gcid, PU_IOE_PB_MISC_CFG, reg);
}

static void enable_odl_phy_mux(uint32_t gcid, int index)
{
	uint64_t reg;
	uint64_t phy_config_scom;
	prlog(PR_DEBUG, "OCAPI: %s: Enabling ODL to PHY MUXes\n", __func__);
	/* Step 2 - Enable MUXes for ODL to PHY connection */
	switch (index) {
	case 2:
	case 3:
		phy_config_scom = OBUS_LL0_IOOL_PHY_CONFIG;
		break;
	case 4:
	case 5:
		phy_config_scom = OBUS_LL3_IOOL_PHY_CONFIG;
		break;
	default:
		assert(false);
	}

	/* PowerBus OLL PHY Training Config Register */
	xscom_read(gcid, phy_config_scom, &reg);

	/*
	 * Enable ODL to use shared PHYs
	 *
	 * On obus3, OTL0 is connected to ODL1 (and OTL1 to ODL0), so
	 * even if it may look odd at first, we do want to enable ODL0
	 * for links 2 and 5
	 */
	switch (index) {
	case 2:
	case 5:
		reg |= OBUS_IOOL_PHY_CONFIG_ODL0_ENABLED;
		break;
	case 3:
	case 4:
		reg |= OBUS_IOOL_PHY_CONFIG_ODL1_ENABLED;
		break;
	}

	/*
	 * Based on the platform, we may have to activate an extra mux
	 * to connect the ODL to the right set of lanes.
	 *
	 * FIXME: to be checked once we have merged with nvlink
	 * code. Need to verify that it's a platform parameter and not
	 * slot-dependent
	 */
	if (platform.ocapi->odl_phy_swap)
		reg |= OBUS_IOOL_PHY_CONFIG_ODL_PHY_SWAP;
	else
		reg &= ~OBUS_IOOL_PHY_CONFIG_ODL_PHY_SWAP;

	/* Disable A-Link link layers */
	reg &= ~OBUS_IOOL_PHY_CONFIG_LINK0_OLL_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_LINK1_OLL_ENABLED;

	xscom_write(gcid, phy_config_scom, reg);
}

static void disable_alink_fp(uint32_t gcid)
{
	uint64_t reg = 0;

	prlog(PR_DEBUG, "OCAPI: %s: Disabling A-Link framer/parsers\n", __func__);
	/* Step 3 - Disable A-Link framers/parsers */
	/* TODO: Confirm if needed on OPAL system */

	reg |= PU_IOE_PB_FP_CFG_FP0_FMR_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP0_PRS_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP1_FMR_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP1_PRS_DISABLE;
	xscom_write(gcid, PU_IOE_PB_FP01_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP23_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP45_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP67_CFG, reg);
}

static void enable_xsl_clocks(uint32_t gcid, uint32_t scom_base, int index)
{
	/* Step 5 - Enable Clocks in XSL */

	prlog(PR_DEBUG, "OCAPI: %s: Enable clocks in XSL\n", __func__);

	npu2_scom_write(gcid, scom_base, NPU2_REG_OFFSET(index_to_stack(index),
							 NPU2_BLOCK_XSL,
							 NPU2_XSL_WRAP_CFG),
			NPU2_MISC_DA_LEN_8B, NPU2_XSL_WRAP_CFG_XSLO_CLOCK_ENABLE);
}

#define CQ_CTL_STATUS_TIMEOUT	10 /* milliseconds */

static int set_fence_control(uint32_t gcid, uint32_t scom_base,
			     int index, uint8_t status)
{
	int stack, block;
	uint64_t reg, status_field;
	uint8_t status_val;
	uint64_t fence_control;
	uint64_t timeout = mftb() + msecs_to_tb(CQ_CTL_STATUS_TIMEOUT);

	stack = index_to_stack(index);
	block = index_to_block(index);

	fence_control = NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					block == NPU2_BLOCK_OTL0 ?
					NPU2_CQ_CTL_FENCE_CONTROL_0 :
					NPU2_CQ_CTL_FENCE_CONTROL_1);

	reg = SETFIELD(NPU2_CQ_CTL_FENCE_CONTROL_REQUEST_FENCE, 0ull, status);
	npu2_scom_write(gcid, scom_base, fence_control,
			NPU2_MISC_DA_LEN_8B, reg);

	/* Wait for fence status to update */
	if (index_to_block(index) == NPU2_BLOCK_OTL0)
		status_field = NPU2_CQ_CTL_STATUS_BRK0_AM_FENCED;
	else
		status_field = NPU2_CQ_CTL_STATUS_BRK1_AM_FENCED;

	do {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(index_to_stack(index),
						     NPU2_BLOCK_CTL,
						     NPU2_CQ_CTL_STATUS),
				     NPU2_MISC_DA_LEN_8B);
		status_val = GETFIELD(status_field, reg);
		if (status_val == status)
			return OPAL_SUCCESS;
		time_wait_ms(1);
	} while (tb_compare(mftb(), timeout) == TB_ABEFOREB);

	/**
	 * @fwts-label OCAPIFenceStatusTimeout
	 * @fwts-advice The NPU fence status did not update as expected. This
	 * could be the result of a firmware or hardware bug. OpenCAPI
	 * functionality could be broken.
	 */
	prlog(PR_ERR,
	      "OCAPI: Fence status for brick %d stuck: expected 0x%x, got 0x%x\n",
	      index, status, status_val);
	return OPAL_HARDWARE;
}

static void set_npcq_config(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg, stack, block;

	prlog(PR_DEBUG, "OCAPI: %s: Set NPCQ Config\n", __func__);
	/* Step 6 - Set NPCQ configuration */
	/* CQ_CTL Misc Config Register #0 */
	stack = index_to_stack(index);
	block = index_to_block(index);

	/* Enable OTL */
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG0(stack, block),
			NPU2_MISC_DA_LEN_8B, NPU2_OTL_CONFIG0_EN);
	set_fence_control(gcid, scom_base, index, 0b01);
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					     NPU2_CQ_CTL_MISC_CFG),
			     NPU2_MISC_DA_LEN_8B);
	/* Set OCAPI mode */
	reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OCAPI_MODE;
	if (block == NPU2_BLOCK_OTL0)
		reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OTL0_ENABLE;
	else
		reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OTL1_ENABLE;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					NPU2_CQ_CTL_MISC_CFG),
			NPU2_MISC_DA_LEN_8B, reg);

	/* NPU Fenced */
	set_fence_control(gcid, scom_base, index, 0b11);

	/* NPU Half Fenced */
	set_fence_control(gcid, scom_base, index, 0b10);

	/* CQ_DAT Misc Config Register #1 */
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_DAT,
					     NPU2_CQ_DAT_MISC_CFG),
			     NPU2_MISC_DA_LEN_8B);
	/* Set OCAPI mode for bricks 2-5 */
	reg |= NPU2_CQ_DAT_MISC_CFG_CONFIG_OCAPI_MODE;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_DAT,
					NPU2_CQ_DAT_MISC_CFG),
			NPU2_MISC_DA_LEN_8B, reg);

	/* CQ_SM Misc Config Register #0 */
	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_CQ_SM_MISC_CFG0),
				     NPU2_MISC_DA_LEN_8B);
		/* Set OCAPI mode for bricks 2-5 */
		reg |= NPU2_CQ_SM_MISC_CFG0_CONFIG_OCAPI_MODE;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_CQ_SM_MISC_CFG0),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void enable_xsl_xts_interfaces(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg;

	prlog(PR_DEBUG, "OCAPI: %s: Enable XSL-XTS Interfaces\n", __func__);
	/* Step 7 - Enable XSL-XTS interfaces */
	/* XTS Config Register - Enable XSL-XTS interface */
	reg = npu2_scom_read(gcid, scom_base, NPU2_XTS_CFG, NPU2_MISC_DA_LEN_8B);
	reg |= NPU2_XTS_CFG_OPENCAPI;
	npu2_scom_write(gcid, scom_base, NPU2_XTS_CFG, NPU2_MISC_DA_LEN_8B, reg);

	/* XTS Config2 Register - Enable XSL1/2 */
	reg = npu2_scom_read(gcid, scom_base, NPU2_XTS_CFG2, NPU2_MISC_DA_LEN_8B);
	switch (index_to_stack(index)) {
	case NPU2_STACK_STCK_1:
		reg |= NPU2_XTS_CFG2_XSL1_ENA;
		break;
	case NPU2_STACK_STCK_2:
		reg |= NPU2_XTS_CFG2_XSL2_ENA;
		break;
	}
	npu2_scom_write(gcid, scom_base, NPU2_XTS_CFG2, NPU2_MISC_DA_LEN_8B, reg);
}

static void enable_sm_allocation(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg, block;
	int stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Enable State Machine Allocation\n", __func__);
	/* Step 8 - Enable state-machine allocation */
	/* Low-Water Marks Registers - Enable state machine allocation */
	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_LOW_WATER_MARKS),
				     NPU2_MISC_DA_LEN_8B);
		reg |= NPU2_LOW_WATER_MARKS_ENABLE_MACHINE_ALLOC;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_LOW_WATER_MARKS),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void enable_pb_snooping(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg, block;
	int stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Enable PowerBus snooping\n", __func__);
	/* Step 9 - Enable PowerBus snooping */
	/* CQ_SM Misc Config Register #0 - Enable PowerBus snooping */
	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_CQ_SM_MISC_CFG0),
				     NPU2_MISC_DA_LEN_8B);
		reg |= NPU2_CQ_SM_MISC_CFG0_CONFIG_ENABLE_PBUS;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_CQ_SM_MISC_CFG0),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void brick_config(uint32_t gcid, uint32_t scom_base, int index)
{
	/*
	 * We assume at this point that the PowerBus Hotplug Mode Control
	 * register is correctly set by Hostboot
	 */
	disable_nvlink(gcid, index);
	set_transport_mux_controls(gcid, scom_base, index,
				   NPU2_DEV_TYPE_OPENCAPI);
	enable_odl_phy_mux(gcid, index);
	disable_alink_fp(gcid);
	enable_xsl_clocks(gcid, scom_base, index);
	set_npcq_config(gcid, scom_base, index);
	enable_xsl_xts_interfaces(gcid, scom_base, index);
	enable_sm_allocation(gcid, scom_base, index);
	enable_pb_snooping(gcid, scom_base, index);
}

/* Procedure 13.1.3.5 - TL Configuration */
static void tl_config(uint32_t gcid, uint32_t scom_base, uint64_t index)
{
	uint64_t reg;
	uint64_t stack = index_to_stack(index);
	uint64_t block = index_to_block(index);

	prlog(PR_DEBUG, "OCAPI: %s: TL Configuration\n", __func__);
	/* OTL Config 0 Register */
	reg = 0;
	/* OTL Enable */
	reg |= NPU2_OTL_CONFIG0_EN;
	/* Block PE Handle from ERAT Index */
	reg |= NPU2_OTL_CONFIG0_BLOCK_PE_HANDLE;
	/* OTL Brick ID */
	reg = SETFIELD(NPU2_OTL_CONFIG0_BRICKID, reg, index - 2);
	/* ERAT Hash 0 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_0, reg, 0b011001);
	/* ERAT Hash 1 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_1, reg, 0b000111);
	/* ERAT Hash 2 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_2, reg, 0b101100);
	/* ERAT Hash 3 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_3, reg, 0b100110);
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG0(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);

	/* OTL Config 1 Register */
	reg = 0;
	/*
	 * We leave Template 1-3 bits at 0 to force template 0 as required
	 * for unknown devices.
	 *
	 * Template 0 Transmit Rate is set to most conservative setting which
	 * will always be supported. Other Template Transmit rates are left
	 * unset and will be set later by OS.
	 */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_TEMP0_RATE, reg, 0b1111);
	/* Extra wait cycles TXI-TXO */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_DRDY_WAIT, reg, 0b001);
	/* Minimum Frequency to Return TLX Credits to AFU */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_CRET_FREQ, reg, 0b001);
	/* Frequency to add age to Transmit Requests */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_AGE_FREQ, reg, 0b11000);
	/* Response High Priority Threshold */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RS2_HPWAIT, reg, 0b011011);
	/* 4-slot Request High Priority Threshold */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RQ4_HPWAIT, reg, 0b011011);
	/* 6-slot Request High Priority */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RQ6_HPWAIT, reg, 0b011011);
	/* Stop the OCAPI Link on Uncorrectable Error
	 * TODO: Confirm final value - disabled for debug */

	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG1(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);

	/* TLX Credit Configuration Register */
	reg = 0;
	/* VC0/VC3/DCP0/DCP1 credits to send to AFU */
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_VC0_CREDITS, reg, 0x40);
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_VC3_CREDITS, reg, 0x40);
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_DCP0_CREDITS, reg, 0x80);
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_DCP1_CREDITS, reg, 0x80);
	npu2_scom_write(gcid, scom_base, NPU2_OTL_TLX_CREDITS(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);
}

/* Detect Nimbus DD2.0 and DD2.01 */
static int get_nimbus_level(void)
{
	struct proc_chip *chip = next_chip(NULL);

	if (chip && chip->type == PROC_CHIP_P9_NIMBUS)
		return chip->ec_level & 0xff;
	return -1;
}

/* Procedure 13.1.3.6 - Address Translation Configuration */
static void address_translation_config(uint32_t gcid, uint32_t scom_base,
				       uint64_t index)
{
	int chip_level;
	uint64_t reg;
	uint64_t stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Address Translation Configuration\n", __func__);
	/* PSL_SCNTL_A0 Register */
	/*
	 * ERAT shared between multiple AFUs
	 *
	 * The workbook has this bit around the wrong way from the hardware.
	 *
	 * TODO: handle correctly with link ganging
	 */
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
					     NPU2_XSL_PSL_SCNTL_A0),
			     NPU2_MISC_DA_LEN_8B);
	reg |= NPU2_XSL_PSL_SCNTL_A0_MULTI_AFU_DIAL;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
					NPU2_XSL_PSL_SCNTL_A0),
			NPU2_MISC_DA_LEN_8B, reg);

	chip_level = get_nimbus_level();
	if (chip_level == 0x20) {
		/*
		 * Errata HW408041 (section 15.1.10 of NPU workbook)
		 * "RA mismatch when both tlbie and checkout response
		 * are seen in same cycle"
		 */
		/* XSL_GP Register - Bloom Filter Disable */
		reg = npu2_scom_read(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP),
				NPU2_MISC_DA_LEN_8B);
		/* To update XSL_GP, we must first write a magic value to it */
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP),
				NPU2_MISC_DA_LEN_8B, 0x0523790323000000);
		reg &= ~NPU2_XSL_GP_BLOOM_FILTER_ENABLE;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP),
				NPU2_MISC_DA_LEN_8B, reg);
	}

	if (chip_level == 0x20 || chip_level == 0x21) {
		/*
		 * DD2.0/2.1 EOA Bug. Fixed in DD2.2
		 */
		reg = 0x32F8000000000001;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
						NPU2_XSL_DEF),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

/* TODO: Merge this with NVLink implementation - we don't use the npu2_bar
 * wrapper for the PHY BARs yet */
static void write_bar(uint32_t gcid, uint32_t scom_base, uint64_t reg,
		      uint64_t addr, uint64_t size)
{
	uint64_t val;
	int block;
	switch (NPU2_REG(reg)) {
	case NPU2_PHY_BAR:
		val = SETFIELD(NPU2_PHY_BAR_ADDR, 0ul, addr >> 21);
		val = SETFIELD(NPU2_PHY_BAR_ENABLE, val, 1);
		break;
	case NPU2_NTL0_BAR:
	case NPU2_NTL1_BAR:
		val = SETFIELD(NPU2_NTL_BAR_ADDR, 0ul, addr >> 16);
		val = SETFIELD(NPU2_NTL_BAR_SIZE, val, ilog2(size >> 16));
		val = SETFIELD(NPU2_NTL_BAR_ENABLE, val, 1);
		break;
	case NPU2_GENID_BAR:
		val = SETFIELD(NPU2_GENID_BAR_ADDR, 0ul, addr >> 16);
		val = SETFIELD(NPU2_GENID_BAR_ENABLE, val, 1);
		break;
	default:
		val = 0ul;
	}

	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		npu2_scom_write(gcid, scom_base, NPU2_REG_OFFSET(0, block, reg),
				NPU2_MISC_DA_LEN_8B, val);
		prlog(PR_DEBUG, "OCAPI: Setting BAR %llx to %llx\n",
		      NPU2_REG_OFFSET(0, block, reg), val);
	}
}

static void setup_global_mmio_bar(uint32_t gcid, uint32_t scom_base,
				  uint64_t reg[])
{
	uint64_t addr, size;

	prlog(PR_DEBUG, "OCAPI: patching up PHY0 bar, %s\n", __func__);
	phys_map_get(gcid, NPU_PHY, 0, &addr, &size);
	write_bar(gcid, scom_base,
		  NPU2_REG_OFFSET(NPU2_STACK_STCK_2, 0, NPU2_PHY_BAR),
		addr, size);
	prlog(PR_DEBUG, "OCAPI: patching up PHY1 bar, %s\n", __func__);
	phys_map_get(gcid, NPU_PHY, 1, &addr, &size);
	write_bar(gcid, scom_base,
		  NPU2_REG_OFFSET(NPU2_STACK_STCK_1, 0, NPU2_PHY_BAR),
		addr, size);

	prlog(PR_DEBUG, "OCAPI: setup global mmio, %s\n", __func__);
	phys_map_get(gcid, NPU_REGS, 0, &addr, &size);
	write_bar(gcid, scom_base,
		  NPU2_REG_OFFSET(NPU2_STACK_STCK_0, 0, NPU2_PHY_BAR),
		addr, size);
	reg[0] = addr;
	reg[1] = size;
}

/* Procedure 13.1.3.8 - AFU MMIO Range BARs */
static void setup_afu_mmio_bars(uint32_t gcid, uint32_t scom_base,
				struct npu2_dev *dev)
{
	uint64_t stack = index_to_stack(dev->index);
	uint64_t offset = index_to_block(dev->index) == NPU2_BLOCK_OTL0 ?
		NPU2_NTL0_BAR : NPU2_NTL1_BAR;
	uint64_t pa_offset = index_to_block(dev->index) == NPU2_BLOCK_OTL0 ?
		NPU2_CQ_CTL_MISC_MMIOPA0_CONFIG :
		NPU2_CQ_CTL_MISC_MMIOPA1_CONFIG;
	uint64_t addr, size, reg;

	prlog(PR_DEBUG, "OCAPI: %s: Setup AFU MMIO BARs\n", __func__);
	phys_map_get(gcid, NPU_OCAPI_MMIO, dev->index, &addr, &size);

	prlog(PR_DEBUG, "OCAPI: AFU MMIO set to %llx, size %llx\n", addr, size);
	write_bar(gcid, scom_base, NPU2_REG_OFFSET(stack, 0, offset), addr,
		size);
	dev->bars[0].npu2_bar.base = addr;
	dev->bars[0].npu2_bar.size = size;

	reg = SETFIELD(NPU2_CQ_CTL_MISC_MMIOPA_ADDR, 0ull, addr >> 16);
	reg = SETFIELD(NPU2_CQ_CTL_MISC_MMIOPA_SIZE, reg, ilog2(size >> 16));
	prlog(PR_DEBUG, "OCAPI: PA translation %llx\n", reg);
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					pa_offset),
			NPU2_MISC_DA_LEN_8B, reg);
}

/* Procedure 13.1.3.9 - AFU Config BARs */
static void setup_afu_config_bars(uint32_t gcid, uint32_t scom_base,
				  struct npu2_dev *dev)
{
	uint64_t stack = index_to_stack(dev->index);
	int stack_num = stack - NPU2_STACK_STCK_0;
	uint64_t addr, size;

	prlog(PR_DEBUG, "OCAPI: %s: Setup AFU Config BARs\n", __func__);
	phys_map_get(gcid, NPU_GENID, stack_num, &addr, &size);
	prlog(PR_DEBUG, "OCAPI: Assigning GENID BAR: %016llx\n", addr);
	write_bar(gcid, scom_base, NPU2_REG_OFFSET(stack, 0, NPU2_GENID_BAR),
		addr, size);
	dev->bars[1].npu2_bar.base = addr;
	dev->bars[1].npu2_bar.size = size;
}

static void otl_enabletx(uint32_t gcid, uint32_t scom_base, uint64_t index)
{
	uint64_t stack = index_to_stack(index);
	uint64_t block = index_to_block(index);
	uint64_t reg;

	/* OTL Config 2 Register */
	/* Transmit Enable */
	prlog(PR_DEBUG, "OCAPI: %s: Enabling TX\n", __func__);
	reg = 0;
	reg |= NPU2_OTL_CONFIG2_TX_SEND_EN;
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG2(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);

	reg = npu2_scom_read(gcid, scom_base, NPU2_OTL_VC_CREDITS(stack, block),
			     NPU2_MISC_DA_LEN_8B);
	prlog(PR_DEBUG, "OCAPI: credit counter: %llx\n", reg);
	/* TODO: Abort if credits are zero */
}

static void reset_ocapi_device(struct npu2_dev *dev)
{
	uint8_t data[3];
	int rc;
	int i;

	switch (dev->index) {
	case 2:
	case 4:
		memcpy(data, platform.ocapi->i2c_odl0_data, sizeof(data));
		break;
	case 3:
	case 5:
		memcpy(data, platform.ocapi->i2c_odl1_data, sizeof(data));
		break;
	default:
		assert(false);
	}

	for (i = 0; i < 3; i++) {
		rc = i2c_request_send(dev->i2c_port_id_ocapi, 0x20, SMBUS_WRITE,
				      platform.ocapi->i2c_offset[i], 1,
				      &data[i], sizeof(data[i]), 120);
		if (rc) {
			/**
			 * @fwts-label OCAPIDeviceResetFailed
			 * @fwts-advice There was an error attempting to send
			 * a reset signal over I2C to the OpenCAPI device.
			 */
			prlog(PR_ERR, "OCAPI: Error writing I2C reset signal: %d\n", rc);
			break;
		}
		if (i != 0)
			time_wait_ms(5);
	}
}

static int odl_train(uint32_t gcid, uint32_t index, struct npu2_dev *dev)
{
	uint64_t reg, config_xscom;
	int timeout = 3000;
	prlog(PR_DEBUG, "OCAPI: %s: Training ODL\n", __func__);

	switch (index) {
	case 2:
		config_xscom = OB0_ODL0_CONFIG;
		break;
	case 3:
		config_xscom = OB0_ODL1_CONFIG;
		break;
	case 4:
		config_xscom = OB3_ODL1_CONFIG;
		break;
	case 5:
		config_xscom = OB3_ODL0_CONFIG;
		break;
	default:
		assert(false);
	}

	/* Reset ODL */
	reg = OB_ODL_CONFIG_RESET;
	reg = SETFIELD(OB_ODL_CONFIG_VERSION, reg, 0b000001);
	reg = SETFIELD(OB_ODL_CONFIG_TRAIN_MODE, reg, 0b0110);
	reg = SETFIELD(OB_ODL_CONFIG_SUPPORTED_MODES, reg, 0b0010);
	reg |= OB_ODL_CONFIG_X4_BACKOFF_ENABLE;
	reg = SETFIELD(OB_ODL_CONFIG_PHY_CNTR_LIMIT, reg, 0b1111);
	reg |= OB_ODL_CONFIG_DEBUG_ENABLE;
	reg = SETFIELD(OB_ODL_CONFIG_FWD_PROGRESS_TIMER, reg, 0b0110);
	xscom_write(gcid, config_xscom, reg);

	reg &= ~OB_ODL_CONFIG_RESET;
	xscom_write(gcid, config_xscom, reg);

	reset_ocapi_device(dev);

	/* Transmit Pattern A */
	reg = SETFIELD(OB_ODL_CONFIG_TRAIN_MODE, reg, 0b0001);
	xscom_write(gcid, config_xscom, reg);
	time_wait_ms(5);

	/* Bump lanes - this improves training reliability */
	npu2_opencapi_bump_ui_lane(dev);

	/* Start training */
	reg = SETFIELD(OB_ODL_CONFIG_TRAIN_MODE, reg, 0b1000);
	xscom_write(gcid, config_xscom, reg);

	do {
		reg = get_odl_status(gcid, index);
		if (GETFIELD(OB_ODL_STATUS_TRAINING_STATE_MACHINE, reg) == 0x7) {
			prlog(PR_NOTICE,
			      "OCAPI: Link %d on chip %u trained in %dms\n",
			      index, gcid, 3000 - timeout);
			return OPAL_SUCCESS;
		}
		time_wait_ms(1);
	} while (timeout--);
	prlog(PR_INFO, "OCAPI: Link %d on chip %u failed to train, retrying\n",
	      index, gcid);
	prlog(PR_INFO, "OCAPI: Link status: %016llx\n", reg);
	return OPAL_HARDWARE;
}

static int64_t npu2_opencapi_get_link_state(struct pci_slot *slot, uint8_t *val)
{
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(slot->phb);
	uint64_t reg;
	int64_t link_width, rc = OPAL_SUCCESS;

	reg = get_odl_status(dev->npu->chip_id, dev->index);
	link_width = GETFIELD(OB_ODL_STATUS_TRAINED_MODE, reg);
	switch (link_width) {
	case 0b0001:
		*val = OPAL_SHPC_LINK_UP_x4;
		break;
	case 0b0010:
		*val = OPAL_SHPC_LINK_UP_x8;
		break;
	default:
		rc = OPAL_HARDWARE;
	}
	return rc;
}

static struct pci_slot *npu2_opencapi_slot_create(struct phb *phb)
{
	struct pci_slot *slot;

	slot = pci_slot_alloc(phb, NULL);
	if (!slot)
		return slot;

	/* TODO: Figure out other slot functions */
	slot->ops.get_presence_state = NULL;
	slot->ops.get_link_state = npu2_opencapi_get_link_state;
	slot->ops.get_power_state = NULL;
	slot->ops.get_attention_state = NULL;
	slot->ops.get_latch_state     = NULL;
	slot->ops.set_power_state     = NULL;
	slot->ops.set_attention_state = NULL;
	/*
	 * Temporarily erase the run_sm callback until we support
	 * dynamic reset of the link. Otherwise, run_sm may call
	 * freset, creset, ... and we don't define them. The run_sm
	 * pointer is always tested before being called, at least at
	 * the time of this writing :-) It will go away when we
	 * implement dynamic reset of the link
	 */
	slot->ops.run_sm = NULL;

	return slot;
}

static int64_t npu2_opencapi_pcicfg_check(struct npu2_dev *dev, uint32_t offset,
					  uint32_t size)
{
	if (!dev || offset > 0xfff || (offset & (size - 1)))
		return OPAL_PARAMETER;

	return OPAL_SUCCESS;
}

static int64_t npu2_opencapi_pcicfg_read(struct phb *phb, uint32_t bdfn,
					 uint32_t offset, uint32_t size,
					 void *data)
{
	uint64_t cfg_addr;
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(phb);
	uint64_t genid_base;
	int64_t rc;

	rc = npu2_opencapi_pcicfg_check(dev, offset, size);
	if (rc)
		return rc;

	genid_base = dev->bars[1].npu2_bar.base +
		(index_to_block(dev->index) == NPU2_BLOCK_OTL1 ? 256 : 0);

	cfg_addr = NPU2_CQ_CTL_CONFIG_ADDR_ENABLE;
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_BUS_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_DEVICE_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_FUNCTION_NUMBER,
			    cfg_addr, bdfn);
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_REGISTER_NUMBER,
			    cfg_addr, offset & ~3u);

	out_be64((uint64_t *)genid_base, cfg_addr);
	sync();

	switch (size) {
	case 1:
		*((uint8_t *)data) =
			in_8((volatile uint8_t *)(genid_base + 128 + (offset & 3)));
		break;
	case 2:
		*((uint16_t *)data) =
			in_le16((volatile uint16_t *)(genid_base + 128 + (offset & 2)));
		break;
	case 4:
		*((uint32_t *)data) = in_le32((volatile uint32_t *)(genid_base + 128));
		break;
	default:
		return OPAL_PARAMETER;
	}

	return OPAL_SUCCESS;
}

#define NPU2_OPENCAPI_PCI_CFG_READ(size, type)				\
static int64_t npu2_opencapi_pcicfg_read##size(struct phb *phb,		\
					       uint32_t bdfn,		\
					       uint32_t offset,		\
					       type *data)		\
{									\
	/* Initialize data in case of error */				\
	*data = (type)0xffffffff;					\
	return npu2_opencapi_pcicfg_read(phb, bdfn, offset,		\
					 sizeof(type), data);		\
}

static int64_t npu2_opencapi_pcicfg_write(struct phb *phb, uint32_t bdfn,
					  uint32_t offset, uint32_t size,
					  uint32_t data)
{
	uint64_t cfg_addr;
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(phb);
	uint64_t genid_base;
	int64_t rc;

	rc = npu2_opencapi_pcicfg_check(dev, offset, size);
	if (rc)
		return rc;

	genid_base = dev->bars[1].npu2_bar.base +
		(index_to_block(dev->index) == NPU2_BLOCK_OTL1 ? 256 : 0);

	cfg_addr = NPU2_CQ_CTL_CONFIG_ADDR_ENABLE;
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_BUS_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_DEVICE_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_FUNCTION_NUMBER,
			    cfg_addr, bdfn);
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_REGISTER_NUMBER,
			    cfg_addr, offset & ~3u);

	out_be64((uint64_t *)genid_base, cfg_addr);
	sync();

	switch (size) {
	case 1:
		out_8((volatile uint8_t *)(genid_base + 128 + (offset & 3)),
		      data);
		break;
	case 2:
		out_le16((volatile uint16_t *)(genid_base + 128 + (offset & 2)),
					       data);
		break;
	case 4:
		out_le32((volatile uint32_t *)(genid_base + 128), data);
		break;
	default:
		return OPAL_PARAMETER;
	}

	return OPAL_SUCCESS;
}

#define NPU2_OPENCAPI_PCI_CFG_WRITE(size, type)				\
static int64_t npu2_opencapi_pcicfg_write##size(struct phb *phb,	\
						uint32_t bdfn,		\
						uint32_t offset,	\
						type data)		\
{									\
	return npu2_opencapi_pcicfg_write(phb, bdfn, offset,		\
					  sizeof(type), data);		\
}

NPU2_OPENCAPI_PCI_CFG_READ(8, u8)
NPU2_OPENCAPI_PCI_CFG_READ(16, u16)
NPU2_OPENCAPI_PCI_CFG_READ(32, u32)
NPU2_OPENCAPI_PCI_CFG_WRITE(8, u8)
NPU2_OPENCAPI_PCI_CFG_WRITE(16, u16)
NPU2_OPENCAPI_PCI_CFG_WRITE(32, u32)

static int64_t npu2_opencapi_ioda_reset(struct phb __unused *phb,
				    bool __unused purge)
{
	/* Not relevant to OpenCAPI - we do this just to silence the error */
	return OPAL_SUCCESS;
}

static int64_t npu2_opencapi_set_pe(struct phb *phb,
				    uint64_t pe_num,
				    uint64_t bdfn,
				    uint8_t bcompare,
				    uint8_t dcompare,
				    uint8_t fcompare,
				    uint8_t action)
{
	struct npu2 *p;
	struct npu2_dev *dev;
	uint64_t reg, val, pe_bdfn;

	/* Sanity check */
	if (action != OPAL_MAP_PE && action != OPAL_UNMAP_PE)
		return OPAL_PARAMETER;
	if (pe_num >= NPU2_MAX_PE_NUM)
		return OPAL_PARAMETER;
	if (bdfn >> 8)
		return OPAL_PARAMETER;
	if (bcompare != OpalPciBusAll ||
	    dcompare != OPAL_COMPARE_RID_DEVICE_NUMBER ||
	    fcompare != OPAL_COMPARE_RID_FUNCTION_NUMBER)
		return OPAL_UNSUPPORTED;

	/* Get the NPU2 device */
	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	p = dev->npu;

	pe_bdfn = dev->bdfn;
	
	val = NPU2_MISC_BRICK_BDF2PE_MAP_ENABLE;
	val = SETFIELD(NPU2_MISC_BRICK_BDF2PE_MAP_PE, val, pe_num);
	val = SETFIELD(NPU2_MISC_BRICK_BDF2PE_MAP_BDF, val, pe_bdfn);
	reg = NPU2_REG_OFFSET(NPU2_STACK_MISC, NPU2_BLOCK_MISC,
			      NPU2_MISC_BRICK0_BDF2PE_MAP0 + (dev->index * 0x18));
	p->bdf2pe_cache[dev->index] = val;
	npu2_write(p, reg, val);

	return OPAL_SUCCESS;
}

static int npu2_add_mmio_regs(struct phb *phb, struct pci_device *pd,
			      void *data __unused)
{
	uint32_t irq;
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(phb);
	uint64_t block = index_to_block(dev->index);
	uint64_t stacku = index_to_stacku(dev->index);
	uint64_t dsisr, dar, tfc, handle;

	/*
	 * Pass the hw irq number for the translation fault irq
	 * irq levels 23 -> 26 are for translation faults, 1 per brick
	 */
	irq = dev->npu->irq_base + NPU_IRQ_LEVELS_XSL;
	if (stacku == NPU2_STACK_STCK_2U)
		irq += 2;
	if (block == NPU2_BLOCK_OTL1)
		irq++;

	/*
	 * Add the addresses of the registers needed by the OS to handle
	 * faults. The OS accesses them by mmio.
	 */
	dsisr  = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_DSISR(stacku, block);
	dar    = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_DAR(stacku, block);
	tfc    = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_TFC(stacku, block);
	handle = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_PEHANDLE(stacku,
								block);
	dt_add_property_cells(pd->dn, "ibm,opal-xsl-irq", irq);
	dt_add_property_cells(pd->dn, "ibm,opal-xsl-mmio",
			hi32(dsisr), lo32(dsisr),
			hi32(dar), lo32(dar),
			hi32(tfc), lo32(tfc),
			hi32(handle), lo32(handle));
	return 0;
}

static void npu2_opencapi_final_fixup(struct phb *phb)
{
	pci_walk_dev(phb, NULL, npu2_add_mmio_regs, NULL);
}

static void mask_nvlink_fir(struct npu2 *p)
{
	uint64_t reg;

	/*
	 * From section 13.1.3.10 of the NPU workbook: "the NV-Link
	 * Datalink Layer Stall and NoStall signals are used for a
	 * different purpose when the link is configured for
	 * OpenCAPI. Therefore, the corresponding bits in NPU FIR
	 * Register 1 must be masked and configured to NOT cause the
	 * NPU to go into Freeze or Fence mode or send an Interrupt."
	 *
	 * FIXME: will need to revisit when mixing nvlink with
	 * opencapi. Assumes an opencapi-only setup on both PHYs for
	 * now.
	 */

	/* Mask FIRs */
	xscom_read(p->chip_id, p->xscom_base + NPU2_MISC_FIR_MASK1, &reg);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0xFFF);
	xscom_write(p->chip_id, p->xscom_base + NPU2_MISC_FIR_MASK1, reg);

	/* freeze disable */
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_MISC_FREEZE_ENABLE1, NPU2_MISC_DA_LEN_8B);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0);
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_MISC_FREEZE_ENABLE1, NPU2_MISC_DA_LEN_8B, reg);

	/* fence disable */
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_MISC_FENCE_ENABLE1, NPU2_MISC_DA_LEN_8B);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0);
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_MISC_FENCE_ENABLE1, NPU2_MISC_DA_LEN_8B, reg);

	/* irq disable */
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_MISC_IRQ_ENABLE1, NPU2_MISC_DA_LEN_8B);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0);
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_MISC_IRQ_ENABLE1, NPU2_MISC_DA_LEN_8B, reg);
}

static int setup_irq(struct npu2 *p)
{
	uint64_t reg, mmio_addr;
	uint32_t base;

	base = xive_alloc_ipi_irqs(p->chip_id, NPU_IRQ_LEVELS, 64);
	if (base == XIVE_IRQ_ERROR) {
		/**
		 * @fwts-label OCAPIIRQAllocationFailed
		 * @fwts-advice OpenCAPI IRQ setup failed. This is probably
		 * a firmware bug. OpenCAPI functionality will be broken.
		 */
		prlog(PR_ERR, "OCAPI: Couldn't allocate interrupts for NPU\n");
		return -1;
	}
	p->irq_base = base;

	xive_register_ipi_source(base, NPU_IRQ_LEVELS, NULL, NULL);
	mmio_addr = (uint64_t ) xive_get_trigger_port(base);
	prlog(PR_DEBUG, "OCAPI: NPU base irq %d @%llx\n", base, mmio_addr);
	reg = (mmio_addr & NPU2_MISC_IRQ_BASE_MASK) << 13;
	npu2_scom_write(p->chip_id, p->xscom_base, NPU2_MISC_IRQ_BASE,
			NPU2_MISC_DA_LEN_8B, reg);
	/*
	 * setup page size = 64k
	 *
	 * OS type is set to AIX: opal also runs with 2 pages per interrupt,
	 * so to cover the max offset for 35 levels of interrupt, we need
	 * bits 41 to 46, which is what the AIX setting does. There's no
	 * other meaning for that AIX setting.
	 */
	reg = npu2_scom_read(p->chip_id, p->xscom_base, NPU2_MISC_CFG,
			NPU2_MISC_DA_LEN_8B);
	reg |= NPU2_MISC_CFG_IPI_PS;
	reg &= ~NPU2_MISC_CFG_IPI_OS;
	npu2_scom_write(p->chip_id, p->xscom_base, NPU2_MISC_CFG,
			NPU2_MISC_DA_LEN_8B, reg);

	/* enable translation interrupts for all bricks */
	reg = npu2_scom_read(p->chip_id, p->xscom_base, NPU2_MISC_IRQ_ENABLE2,
			     NPU2_MISC_DA_LEN_8B);
	reg |= PPC_BIT(0) | PPC_BIT(1) | PPC_BIT(2) | PPC_BIT(3);
	npu2_scom_write(p->chip_id, p->xscom_base, NPU2_MISC_IRQ_ENABLE2,
			NPU2_MISC_DA_LEN_8B, reg);

	mask_nvlink_fir(p);
	return 0;
}

#define LINK_TRAINING_RETRIES	5

static void npu2_opencapi_setup_device(struct dt_node *dn_link, struct npu2 *n,
				       struct npu2_dev *dev)
{
	uint32_t dev_index, npu_index;
	struct dt_node *dn_phb, *dn;
	struct pci_slot *slot;
	char port_name[17];
	uint64_t mm_win[2];
	int retries = LINK_TRAINING_RETRIES;
	int rc;

	dev_index = dt_prop_get_u32(dn_link, "ibm,npu-link-index");
	npu_index = dt_prop_get_u32(n->dt_node, "ibm,npu-index");

	/* Populate PHB device node */
	phys_map_get(n->chip_id, NPU_OCAPI_MMIO, dev_index, &mm_win[0],
		     &mm_win[1]);
	prlog(PR_DEBUG, "OCAPI: Setting MMIO window to %016llx + %016llx\n",
	      mm_win[0], mm_win[1]);
	dn_phb = dt_new_addr(dt_root, "pciex", mm_win[0]);
	assert(dn_phb);
	dt_add_property_strings(dn_phb,
				"compatible",
				"ibm,power9-npu-opencapi-pciex",
				"ibm,ioda2-npu2-opencapi-phb");

	dt_add_property_cells(dn_phb, "#address-cells", 3);
	dt_add_property_cells(dn_phb, "#size-cells", 2);
	dt_add_property_cells(dn_phb, "#interrupt-cells", 1);
	dt_add_property_cells(dn_phb, "bus-range", 0, 0xff);
	dt_add_property_cells(dn_phb, "clock-frequency", 0x200, 0);
        dt_add_property_cells(dn_phb, "interrupt-parent", get_ics_phandle());

	dt_add_property_strings(dn_phb, "device_type", "pciex");
	dt_add_property(dn_phb, "reg", mm_win, sizeof(mm_win));
	dt_add_property_cells(dn_phb, "ibm,npu-index", npu_index);
	dt_add_property_cells(dn_phb, "ibm,chip-id", n->chip_id);
	dt_add_property_cells(dn_phb, "ibm,xscom-base", n->xscom_base);
	dt_add_property_cells(dn_phb, "ibm,npcq", n->dt_node->phandle);
	dt_add_property_cells(dn_phb, "ibm,links", 1);
	dt_add_property(dn_phb, "ibm,mmio-window", mm_win, sizeof(mm_win));
	dt_add_property_cells(dn_phb, "ibm,phb-diag-data-size", 0);
	dt_add_property_cells(dn_phb, "ibm,opal-num-pes", NPU2_MAX_PE_NUM);

	n->mm_base = mm_win[0];
	n->mm_size = mm_win[1];

	dt_add_property_cells(dn_phb, "ranges", 0x02000000,
			      hi32(n->mm_base), lo32(n->mm_base),
			      hi32(n->mm_base), lo32(n->mm_base),
			      hi32(n->mm_size), lo32(n->mm_size));

	dev->type = NPU2_DEV_TYPE_OPENCAPI;
	dev->npu = n;
	dev->dt_node = dn_link;
	dev->phb_ocapi.dt_node = dn_phb;
	dev->phb_ocapi.ops = &npu2_opencapi_ops;
	dev->phb_ocapi.phb_type = phb_type_npu_v2_opencapi;
	dev->phb_ocapi.scan_map = 1;
	dev->index = dt_prop_get_u32(dn_link, "ibm,npu-link-index");
	dev->pl_xscom_base = dt_prop_get_u64(dn_link, "ibm,npu-phy");
	dev->lane_mask = dt_prop_get_u32(dn_link, "ibm,npu-lane-mask");
	dev->link_speed = dt_prop_get_u64(dn_link, "ibm,link-speed");
	dev->bdfn = 0;
	n->total_devices++;

	/* Find I2C port for handling device reset */
	snprintf(port_name, sizeof(port_name), "p8_%08x_e%dp%d",
		 dev->npu->chip_id, platform.ocapi->i2c_engine,
		 platform.ocapi->i2c_port);
	prlog(PR_DEBUG, "OCAPI: Looking for I2C port %s\n", port_name);

	dt_for_each_compatible(dt_root, dn, "ibm,power9-i2c-port") {
		if (streq(port_name, dt_prop_get(dn, "ibm,port-name"))) {
			dev->i2c_port_id_ocapi = dt_prop_get_u32(dn, "ibm,opal-id");
			break;
		}
	}

	if (!dev->i2c_port_id_ocapi) {
		prlog(PR_ERR, "OCAPI: Couldn't find I2C port %s\n", port_name);
		goto failed;
	}

	/* TODO: Procedure 13.1.3.7 - AFU Memory Range BARs */
	/* Procedure 13.1.3.8 - AFU MMIO Range BARs */
	setup_afu_mmio_bars(n->chip_id, n->xscom_base, dev);
	/* Procedure 13.1.3.9 - AFU Config BARs */
	setup_afu_config_bars(n->chip_id, n->xscom_base, dev);

	set_fence_control(n->chip_id, n->xscom_base, dev->index, 0b00);

	npu2_opencapi_phy_setup(dev);

	switch (npu2_ocapi_training_state) {
	case NPU2_TRAIN_PRBS31:
		prlog(PR_INFO,
			"OCAPI: Link %d sending PRBS31 pattern per NVRAM setting\n",
			dev->index);
		npu2_opencapi_phy_prbs31(dev);
		break;

	case NPU2_TRAIN_DEFAULT:
		do {
			rc = odl_train(n->chip_id, dev->index, dev);
		} while (rc != OPAL_SUCCESS && --retries);

		if (rc != OPAL_SUCCESS && retries == 0) {
			/**
			 * @fwts-label OCAPILinkTrainingFailed
			 * @fwts-advice The OpenCAPI link training procedure failed.
			 * This indicates a hardware or firmware bug. OpenCAPI
			 * functionality will not be available on this link.
			 */
			prlog(PR_ERR,
				"OCAPI: Link %d on chip %u failed to train\n",
				dev->index, n->chip_id);
			prlog(PR_ERR, "OCAPI: Final link status: %016llx\n",
				get_odl_status(n->chip_id, dev->index));
			goto failed;
		}

		otl_enabletx(n->chip_id, n->xscom_base, dev->index);

		slot = npu2_opencapi_slot_create(&dev->phb_ocapi);
		if (!slot) {
			/**
			 * @fwts-label OCAPICannotCreatePHBSlot
			 * @fwts-advice Firmware probably ran out of memory creating
			 * NPU slot. OpenCAPI functionality could be broken.
			 */
			prlog(PR_ERR, "OCAPI: Cannot create PHB slot\n");
		}
		break;

	case NPU2_TRAIN_NONE:
		prlog(PR_INFO, "OCAPI: Link %d not trained per NVRAM setting\n",
			dev->index);
		break;
	}

	pci_register_phb(&dev->phb_ocapi, OPAL_DYNAMIC_PHB_ID);
	return;
failed:
	dt_add_property_string(dn_phb, "status", "error");
	return;
}

static void npu2_opencapi_probe(struct dt_node *dn)
{
	struct dt_node *link;
	char *path;
	uint32_t gcid, index, links, scom_base;
	uint64_t reg[2];
	uint64_t dev_index;
	struct npu2 *n;
	int rc, i = 0;

	gcid = dt_get_chip_id(dn);
	index = dt_prop_get_u32(dn, "ibm,npu-index");
	links = dt_prop_get_u32(dn, "ibm,npu-links");

	/* Don't try to init when we have an NVLink link */
	dt_for_each_compatible(dn, link, "ibm,npu-link") {
		prlog(PR_DEBUG, "OCAPI: NPU%d: NVLink link found, skipping\n",
		      index);
		return;
	}

	path = dt_get_path(dn);
	prlog(PR_INFO, "OCAPI: Chip %d Found OpenCAPI NPU%d (%d links) at %s\n",
	      gcid, index, links, path);
	free(path);

	assert(platform.ocapi);

	/* TODO: Test OpenCAPI with fast reboot and make it work */
	disable_fast_reboot("OpenCAPI device enabled");

	scom_base = dt_get_address(dn, 0, NULL);
	prlog(PR_INFO, "OCAPI:	 SCOM Base:  %08x\n", scom_base);

	setup_global_mmio_bar(gcid, scom_base, reg);

	n = zalloc(sizeof(struct npu2) + links * sizeof(struct npu2_dev));
	n->devices = (struct npu2_dev *)(n + 1);
	n->chip_id = gcid;
	n->xscom_base = scom_base;
	n->regs = (void *)reg[0];
	n->dt_node = dn;

	dt_for_each_compatible(dn, link, "ibm,npu-link-opencapi") {
		dev_index = dt_prop_get_u32(link, "ibm,npu-link-index");
		prlog(PR_INFO, "OCAPI: Configuring link index %lld\n",
		      dev_index);

		/* Procedure 13.1.3.1 - Select OCAPI vs NVLink */
		brick_config(gcid, scom_base, dev_index);

		/* Procedure 13.1.3.5 - Transaction Layer Configuration */
		tl_config(gcid, scom_base, dev_index);

		/* Procedure 13.1.3.6 - Address Translation Configuration */
		address_translation_config(gcid, scom_base, dev_index);
	}

	/* Procedure 13.1.3.10 - Interrupt Configuration */
	rc = setup_irq(n);
	if (rc)
		goto failed;

	dt_for_each_compatible(dn, link, "ibm,npu-link-opencapi") {
		npu2_opencapi_setup_device(link, n, &n->devices[i]);
		i++;
	}

	return;
failed:
	free(n);
}

static void read_nvram_training_state(void)
{
	const char *state;

	state = nvram_query("opencapi-link-training");
	if (state) {
		if (!strcmp(state, "prbs31"))
			npu2_ocapi_training_state = NPU2_TRAIN_PRBS31;
		else if (!strcmp(state, "none"))
			npu2_ocapi_training_state = NPU2_TRAIN_NONE;
		else
			prlog(PR_WARNING,
			      "OCAPI: invalid training state in NVRAM: %s\n",
			      state);
	}
}

void probe_npu2_opencapi(void)
{
	struct dt_node *np_npu;

	read_nvram_training_state();

	dt_for_each_compatible(dt_root, np_npu, "ibm,power9-npu")
		npu2_opencapi_probe(np_npu);
}

static const struct phb_ops npu2_opencapi_ops = {
	.cfg_read8		= npu2_opencapi_pcicfg_read8,
	.cfg_read16		= npu2_opencapi_pcicfg_read16,
	.cfg_read32		= npu2_opencapi_pcicfg_read32,
	.cfg_write8		= npu2_opencapi_pcicfg_write8,
	.cfg_write16		= npu2_opencapi_pcicfg_write16,
	.cfg_write32		= npu2_opencapi_pcicfg_write32,
	.choose_bus		= NULL,
	.device_init		= NULL,
	.phb_final_fixup	= npu2_opencapi_final_fixup,
	.ioda_reset		= npu2_opencapi_ioda_reset,
	.papr_errinjct_reset	= NULL,
	.pci_reinit		= NULL,
	.set_phb_mem_window	= NULL,
	.phb_mmio_enable	= NULL,
	.map_pe_mmio_window	= NULL,
	.map_pe_dma_window	= NULL,
	.map_pe_dma_window_real	= NULL,
	.pci_msi_eoi		= NULL,
	.set_xive_pe		= NULL,
	.get_msi_32		= NULL,
	.get_msi_64		= NULL,
	.set_pe			= npu2_opencapi_set_pe,
	.set_peltv		= NULL,
	.eeh_freeze_status	= npu2_freeze_status,  /* TODO */
	.eeh_freeze_clear	= NULL,
	.eeh_freeze_set		= NULL,
	.next_error		= NULL,
	.err_inject		= NULL,
	.get_diag_data		= NULL,
	.get_diag_data2		= NULL,
	.set_capi_mode		= NULL,
	.set_capp_recovery	= NULL,
	.tce_kill		= NULL,
};

static int64_t opal_npu_spa_setup(uint64_t phb_id, uint32_t __unused bdfn,
				uint64_t addr, uint64_t PE_mask)
{
	uint64_t stack, block, offset, reg;
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2_dev *dev;
	int rc;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi)
		return OPAL_PARAMETER;

	/* 4k aligned */
	if (addr & 0xFFF)
		return OPAL_PARAMETER;

	if (PE_mask > 15)
		return OPAL_PARAMETER;

	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	block = index_to_block(dev->index);
	stack = index_to_stack(dev->index);
	if (block == NPU2_BLOCK_OTL1)
		offset = NPU2_XSL_PSL_SPAP_A1;
	else
		offset = NPU2_XSL_PSL_SPAP_A0;


	lock(&dev->npu->lock);
	/*
	 * set the SPAP used by the device
	 */
	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, offset),
			NPU2_MISC_DA_LEN_8B);
	if ((addr && (reg & NPU2_XSL_PSL_SPAP_EN)) ||
		(!addr && !(reg & NPU2_XSL_PSL_SPAP_EN))) {
		rc = OPAL_BUSY;
		goto out;
	}
	/* SPA is disabled by passing a NULL address */
	reg = addr;
	if (addr)
		reg = addr | NPU2_XSL_PSL_SPAP_EN;

	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, offset),
			NPU2_MISC_DA_LEN_8B, reg);

	/*
	 * set the PE mask that the OS uses for PASID -> PE handle
	 * conversion
	 */
	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_OTL_CONFIG0(stack, block), NPU2_MISC_DA_LEN_8B);
	reg &= ~NPU2_OTL_CONFIG0_PE_MASK;
	reg |= (PE_mask << (63-7));
	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_OTL_CONFIG0(stack, block), NPU2_MISC_DA_LEN_8B,
			reg);
	rc = OPAL_SUCCESS;
out:
	unlock(&dev->npu->lock);
	return rc;
}
opal_call(OPAL_NPU_SPA_SETUP, opal_npu_spa_setup, 4);

static int64_t opal_npu_spa_clear_cache(uint64_t phb_id, uint32_t __unused bdfn,
					uint64_t PE_handle)
{
	uint64_t cc_inv, stack, block, reg, rc;
	uint32_t retries = 5;
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2_dev *dev;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi)
		return OPAL_PARAMETER;

	if (PE_handle > MAX_PE_HANDLE)
		return OPAL_PARAMETER;

	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	block = index_to_block(dev->index);
	stack = index_to_stack(dev->index);
	cc_inv = NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_PSL_LLCMD_A0);

	lock(&dev->npu->lock);
	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base, cc_inv,
			NPU2_MISC_DA_LEN_8B);
	if (reg & PPC_BIT(16)) {
		rc = OPAL_BUSY;
		goto out;
	}

	reg = PE_handle | PPC_BIT(15);
	if (block == NPU2_BLOCK_OTL1)
		reg |= PPC_BIT(48);
	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base, cc_inv,
			NPU2_MISC_DA_LEN_8B, reg);

	rc = OPAL_HARDWARE;
	while (retries--) {
		reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
				     cc_inv, NPU2_MISC_DA_LEN_8B);
		if (!(reg & PPC_BIT(16))) {
			rc = OPAL_SUCCESS;
			break;
		}
		/* the bit expected to flip in less than 200us */
		time_wait_us(200);
	}
out:
	unlock(&dev->npu->lock);
	return rc;
}
opal_call(OPAL_NPU_SPA_CLEAR_CACHE, opal_npu_spa_clear_cache, 3);

static int get_template_rate(unsigned int templ, char *rate_buf)
{
	int shift, idx, val;

	/*
	 * Each rate is encoded over 4 bits (0->15), with 15 being the
	 * slowest. The buffer is a succession of rates for all the
	 * templates. The first 4 bits are for template 63, followed
	 * by 4 bits for template 62, ... etc. So the rate for
	 * template 0 is at the very end of the buffer.
	 */
	idx = (TL_MAX_TEMPLATE - templ) / 2;
	shift = 4 * (1 - ((TL_MAX_TEMPLATE - templ) % 2));
	val = rate_buf[idx] >> shift;
	return val;
}

static bool is_template_supported(unsigned int templ, long capabilities)
{
	return !!(capabilities & (1ull << templ));
}

static int64_t opal_npu_tl_set(uint64_t phb_id, uint32_t bdfn,
			long capabilities, uint64_t rate_phys, int rate_sz)
{
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2_dev *dev;
	uint64_t stack, block, reg, templ_rate;
	int i, rate_pos;
	char *rate = (char *) rate_phys;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi)
		return OPAL_PARAMETER;
	if (!opal_addr_valid(rate) || rate_sz != TL_RATE_BUF_SIZE)
		return OPAL_PARAMETER;

	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	block = index_to_block(dev->index);
	stack = index_to_stack(dev->index);
	/*
	 * The 'capabilities' argument defines what TL template the
	 * device can receive. OpenCAPI 3.0 and 4.0 define 64 templates, so
	 * that's one bit per template.
	 *
	 * For each template, the device processing time may vary, so
	 * the device advertises at what rate a message of a given
	 * template can be sent. That's encoded in the 'rate' buffer.
	 *
	 * On P9, NPU only knows about TL templates 0 -> 3.
	 * Per the spec, template 0 must be supported.
	 */
	if (!is_template_supported(0, capabilities))
		return OPAL_PARAMETER;

	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
			     NPU2_OTL_CONFIG1(stack, block),
			     NPU2_MISC_DA_LEN_8B);
	reg &= ~(NPU2_OTL_CONFIG1_TX_TEMP1_EN | NPU2_OTL_CONFIG1_TX_TEMP3_EN |
		 NPU2_OTL_CONFIG1_TX_TEMP1_EN);
	for (i = 0; i < 4; i++) {
		/* Skip template 0 as it is implicitly enabled */
		if (i && is_template_supported(i, capabilities))
			reg |= PPC_BIT(i);
		/* The tx rate should still be set for template 0 */
		templ_rate = get_template_rate(i, rate);
		rate_pos = 8 + i * 4;
		reg = SETFIELD(PPC_BITMASK(rate_pos, rate_pos + 3), reg,
			       templ_rate);
	}
	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_OTL_CONFIG1(stack, block), NPU2_MISC_DA_LEN_8B,
			reg);
	prlog(PR_DEBUG, "OCAPI: Link %llx:%x, TL conf1 register set to %llx\n",
	      phb_id, bdfn, reg);
	return OPAL_SUCCESS;
}
opal_call(OPAL_NPU_TL_SET, opal_npu_tl_set, 5);
