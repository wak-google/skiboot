/* Copyright 2013-2016 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __PLATFORM_H
#define __PLATFORM_H

/* Some fwd declarations for types used further down */
struct phb;
struct pci_device;
struct pci_slot;
struct errorlog;

enum resource_id {
	RESOURCE_ID_KERNEL,
	RESOURCE_ID_INITRAMFS,
	RESOURCE_ID_CAPP,
	RESOURCE_ID_IMA_CATALOG,
	RESOURCE_ID_VERSION,
};
#define RESOURCE_SUBID_NONE 0
#define RESOURCE_SUBID_SUPPORTED 1

struct bmc_platform {
	const char *name;

	/*
	 * Map IPMI_OEM_X to vendor commands for this BMC
	 * 0 = unimplimented
	 */
	uint32_t ipmi_oem_partial_add_esel;
	uint32_t ipmi_oem_pnor_access_status;
};

/* OpenCAPI platform-specific I2C information */
struct platform_ocapi {
	uint8_t i2c_engine;		/* I2C engine number */
	uint8_t i2c_port;		/* I2C port number */
	uint32_t i2c_offset[3];		/* Offsets on I2C device */
	uint8_t i2c_odl0_data[3];	/* Data to reset ODL0 */
	uint8_t i2c_odl1_data[3];	/* Data to reset ODL1 */
	bool odl_phy_swap;		/* Swap ODL1 to use brick 2 rather than
					 * brick 1 lanes */
};

/*
 * Each platform can provide a set of hooks
 * that can affect the generic code
 */
struct platform {
	const char	*name;

	/*
	 * If BMC is constant, bmc platform specified here.
	 * Platforms can also call set_bmc_platform() if BMC platform is
	 * not a constant.
	 */
	const struct bmc_platform *bmc;

	/* OpenCAPI platform-specific I2C information */
	const struct platform_ocapi *ocapi;

	/*
	 * Probe platform, return true on a match, called before
	 * any allocation has been performed outside of the heap
	 * so the platform can perform additional memory reservations
	 * here if needed.
	 *
	 * Only the boot CPU is running at this point and the cpu_thread
	 * structure for secondaries have not been initialized yet. The
	 * timebases are not synchronized.
	 *
	 * Services available:
	 *
	 * - Memory allocations / reservations
	 * - XSCOM
	 * - FSI
	 * - Host Services
	 */
	bool		(*probe)(void);

	/*
	 * This is called right after the secondary processors are brought
	 * up and the timebases in sync to perform any additional platform
	 * specific initializations. On FSP based machines, this is where
	 * the FSP driver is brought up.
	 */
	void		(*init)(void);

	/*
	 * These are used to power down and reboot the machine
	 */
	int64_t		(*cec_power_down)(uint64_t request);
	int64_t		(*cec_reboot)(void);

	/*
	 * This is called once per PHB before probing. It allows the
	 * platform to setup some PHB private data that can be used
	 * later on by calls such as pci_get_slot_info() below. The
	 * "index" argument is the PHB index within the IO HUB (or
	 * P8 chip).
	 *
	 * This is called before the PHB HW has been initialized.
	 */
	void		(*pci_setup_phb)(struct phb *phb, unsigned int index);

	/*
	 * This is called before resetting the PHBs (lift PERST) and
	 * probing the devices. The PHBs have already been initialized.
	 */
	void		(*pre_pci_fixup)(void);
	/*
	 * Called during PCI scan for each device. For bridges, this is
	 * called before its children are probed. This is called for
	 * every device and for the PHB itself with a NULL pd though
	 * typically the implementation will only populate the slot
	 * info structure for bridge ports
	 */
	void		(*pci_get_slot_info)(struct phb *phb,
					     struct pci_device *pd);

	/*
	 * Called after PCI probe is complete and before inventory is
	 * displayed in console. This can either run platform fixups or
	 * can be used to send the inventory to a service processor.
	 */
	void		(*pci_probe_complete)(void);

	/*
	 * If the above is set to skiboot, the handler is here
	 */
	void		(*external_irq)(unsigned int chip_id);

	/*
	 * nvram ops.
	 *
	 * Note: To keep the FSP driver simple, we only ever read the
	 * whole nvram once at boot and we do this passing a dst buffer
	 * that is 4K aligned. The read is asynchronous, the backend
	 * must call nvram_read_complete() when done (it's allowed to
	 * do it recursively from nvram_read though).
	 */
	int		(*nvram_info)(uint32_t *total_size);
	int		(*nvram_start_read)(void *dst, uint32_t src,
					    uint32_t len);
	int		(*nvram_write)(uint32_t dst, void *src, uint32_t len);

	/*
	 * OCC timeout. This return how long we should wait for the OCC
	 * before timing out. This lets us use a high value on larger FSP
	 * machines and cut it off completely on BML boots and OpenPower
	 * machines without pre-existing OCC firmware. Returns a value in
	 * seconds.
	 */
	uint32_t	(*occ_timeout)(void);

	int		(*elog_commit)(struct errorlog *buf);

	/*
	 * Initiate loading an external resource (e.g. kernel payload, OCC)
	 * into a preallocated buffer.
	 * This is designed to asynchronously load external resources.
	 * Returns OPAL_SUCCESS or error.
	 */
	int		(*start_preload_resource)(enum resource_id id,
						  uint32_t idx,
						  void *buf, size_t *len);

	/*
	 * Returns true when resource is loaded.
	 * Only has to return true once, for the
	 * preivous start_preload_resource call for this resource.
	 * If not implemented, will return true and start_preload_resource
	 * *must* have synchronously done the load.
	 * Retruns OPAL_SUCCESS, OPAL_BUSY or an error code
	 */
	int		(*resource_loaded)(enum resource_id id, uint32_t idx);

	/*
	 * Executed just prior to handing control over to the payload.
	 */
	void		(*exit)(void);

	/*
	 * Read a sensor value
	 */
	int64_t		(*sensor_read)(uint32_t sensor_hndl, int token,
				       uint64_t *sensor_data);
	/*
	 * Return the heartbeat time
	 */
	int		(*heartbeat_time)(void);

	/*
	 * OPAL terminate
	 */
	void __attribute__((noreturn)) (*terminate)(const char *msg);
};

extern struct platform __platforms_start;
extern struct platform __platforms_end;

extern struct platform	platform;
extern const struct bmc_platform *bmc_platform;

extern bool manufacturing_mode;

#define DECLARE_PLATFORM(name)\
static const struct platform __used __section(".platforms") name ##_platform

extern void probe_platform(void);

extern int start_preload_resource(enum resource_id id, uint32_t subid,
				  void *buf, size_t *len);

extern int resource_loaded(enum resource_id id, uint32_t idx);

extern int wait_for_resource_loaded(enum resource_id id, uint32_t idx);

extern void set_bmc_platform(const struct bmc_platform *bmc);

#endif /* __PLATFORM_H */
