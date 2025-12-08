#include <config.h>
 
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <virconftypes.h>
#include "device_conf.h"
#include "domain_addr.h"
#include "domain_conf.h"
#include "glib.h"
#include "libvirt/libvirt.h"
#include "libvirt/virterror.h"
#include "virerror.h"
#include "virlog.h"
#include "ch_domain.h"
#include "ch_pci_addr.h"
#include "virerror.h"
#include "virlog.h"
#include "virpci.h"

#define VIR_FROM_THIS VIR_FROM_CH
VIR_LOG_INIT("ch.ch_pci_addr");

// Helper to collect `virDomainDeviceInfo`s that need to allocate a completely new PCI device ID. 
typedef struct _dynamicAddressQueue {
    // Current length of the queue
    size_t size;
    // Index of the next free slot in the queue; Equals the number of items in the queue
    size_t nextIdx;
    // Array of pointers that hold pointers to the device infos
    virDomainDeviceInfo **devInfos;
} dynamicAddressQueue;

/**
 * Initialize a new empty queue
 * @queue: Out; Pointer to the newly allocated queue
 *
 * Returns -1 if memory allocation fails, otherwise 0 to indicate success
 */ 
static int new_addr_queue(dynamicAddressQueue **queue) {
    dynamicAddressQueue *new_queue = g_new0 (dynamicAddressQueue, 1);
    if (new_queue) {
        new_queue->size = 10;
        new_queue->nextIdx = 0;
        new_queue->devInfos = g_new0(virDomainDeviceInfo*, new_queue->size);
        *queue = new_queue;
        return 0;
    } 
    return -1;
}

/**
 * Add a new `virDomainDeviceInfo` to the queue
 * @queue: Queue to add the virDomainDeviceInfo to
 * @devInfo: virDomainDeviceInfo to add to the queue
 *
 *  Returns 0 on success and -1 if the queue grows and reallocation fails.
 */ 
static int add_info_to_queue(dynamicAddressQueue *queue, virDomainDeviceInfo *devInfo) {
    if (queue->nextIdx >= queue->size) {
        queue->size *= 2;
        queue->devInfos = g_realloc(queue->devInfos, queue->size);
        if (!queue->devInfos) {
            return -1;
        }
    }
    queue->devInfos[queue->nextIdx++] = devInfo;
    return 0;
}

/**
 * Frees all memory owned by the queue struct and sets queue to NULL on success.
 * @queue: Pointer to a pointer of the queue to free. Will be overwritten with 0
 */ 
static void free_queue(dynamicAddressQueue **queue) {
    g_free((*queue)->devInfos);
    g_free(*queue);
    *queue = NULL;
}

/**
 * Creates a PCIAddressSet and initializes the structure for a single PCI segment.
 * @obj: Vir domain for which a bus is to be created and added to
 *
 * Returns 0 if the bus bus successfully created and added to the domain. Otherwise -1.
 */
static virDomainPCIAddressSet* chDomainPCIAddressSetCreate(virDomainDef *def) {
    int nbuses = 0;
    virDomainPCIAddressSet *addrs = NULL;

    if (!def) {
        virReportError(VIR_ERR_INVALID_ARG, "Domain def pointer is NULL!");
        return NULL;
    }

    if (def->ncontrollers > 0) {
        virReportError(VIR_ERR_INVALID_ARG, 
            _("No additional PCI controllers supported right now!"));
        return NULL;
    }

    // We currently support only one bus
    nbuses = 1;
    // Currently we do *not* support more than 1 PCI bus so abort with error in case
    if (nbuses > 1) {
        virReportError(VIR_ERR_INVALID_ARG,
            _("CHV currently does't support more than one PCI bus!"));
        return NULL;
    }
    
    // We need to allocate some representation of the PCI bus for libvirt to manage devices
    if (!(addrs = virDomainPCIAddressSetAlloc(nbuses, VIR_PCI_ADDRESS_EXTENSION_NONE)))
        return NULL;

    return addrs;
}

/**
 * Reserve a PCI slot ID for an device, if one is given by the XML or queue it for later assignment.
 *
 * @addrSet: PciAddressSet to reserve the address from
 * @devInfo: Pointer to the DeviceInfo of the device to reserve a PCI slot ID for
 * @queue: Queue of DeviceInfos for which a slot ID is reserved later
 *
 * Returns: 0 on success, -1 in case of error 
 */
static int chReserveOrQueueForPciSlotId(virDomainPCIAddressSet *addrSet,
                                        virDomainDeviceInfo *devInfo,
                                        dynamicAddressQueue* queue)
{
    // Enforce correct connection flags
    devInfo->pciConnectFlags = VIR_PCI_CONNECT_TYPE_PCI_DEVICE;
    // If the device is configured per XML as PCI device with a fixed slot ID, reserve this ID
    if (virDeviceInfoPCIAddressIsWanted(devInfo)) {
        // ... add it to the queue for later slot id assignment to prevent conflicts
        if (add_info_to_queue(queue, devInfo)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Failed to add device '%s' to the queue for PCI ID assignment!"), 
                                            devInfo->alias);
            return -1;
        }
        DBG("Queued device '%s' for later PCI slot assignment", devInfo->alias);
    // ... else, if the device is not configured to be of any type...
    } else {
        if ((devInfo->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            && (devInfo->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Found non-PCI device '%s'. CHV only supports PCI virtio devices!"),
                                            devInfo->alias);
            return -1;
        }
        // We don't support multi function devices currently, so fail if we see a function ID set.
        if (devInfo->addr.pci.function) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
  _("Found non-zero function ID '%d' for device '%s'. CHV does not support multi function devices!"),
                           devInfo->addr.pci.function,
                           devInfo->alias);
            return -1;
        }
        // We don't need to assign a new slot, but must mark the slot as taken
        if (virDomainPCIAddressReserveAddr(addrSet, &devInfo->addr.pci, devInfo->pciConnectFlags, 0)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Couldn't reserve PCI slot %d from config of device with alias '%s'!"),
                                            devInfo->addr.pci.slot, devInfo->alias);
            return -1;    
        }
        DBG("Assigned Slot %d to device '%s", devInfo->addr.pci.slot, devInfo->alias);
    }
    return 0;
}

/**
 * Initialize all RNG devices in the domain with an PCI slot ID. Adds a default RNG device if given an empty list of
 * devices.
 *
 * CHV adds a default RNG device if none is provided in the VM config. We mimic this behavior by creating 
 * an RNG device with CHV defaults if the domain does not contain an RNG device. This function allocates a new 
 * array in case an empty one is provided.
 * Device without a slot ID given in the XML are queued for later ID assignment and therefore not initialized by this 
 * function.
 *
 * @addrSet: PciAddressSet to reserve the slot IDs from
 * @numDefs: Pointer to the length of the RNG device array
 * @deviceDevs: Pointer to the array of RNG devices of a domain
 * @queue: Queue of DeviceInfos for which a slot ID is reserved later
 *
 * Returns: 0 on success, -1 in case of error 
 */
static int chInitRngVirtioPciDevices(virDomainPCIAddressSet *addrSet,
                                     size_t *numDefs,
                                     virDomainRNGDef ***deviceDefs,
                                     dynamicAddressQueue* queue)
{
    size_t idx;
    // Assign an address to all RNG devices
    for (idx = 0; idx < *numDefs; ++idx) {
        virDomainRNGDef *rng = (*deviceDefs)[idx];
        if (chReserveOrQueueForPciSlotId(addrSet, &rng->info, queue)) 
            return -1;
    }
    return 0;
}

/**
 * Initialize all network devices in the domain with an PCI slot ID or places them in a queue for later assignment.
 *
 * Device without a slot ID given in the XML are queued for later ID assignment and therefore not initialized by this 
 * function.
 *
 * @addrSet: PciAddressSet to reserve the slot IDs from
 * @numDefs: Length of the network device array
 * @deviceDevs: Array of network devices definitions of a domain
 * @queue: Queue of DeviceInfos for which a slot ID is reserved later
 *
 * Returns: 0 on success, -1 in case of error 
 */
static int chInitNetworkVirtioPciDevices(virDomainPCIAddressSet *addrSet,
                                         size_t numDefs,
                                         virDomainNetDef **netDevDefs,
                                         dynamicAddressQueue* queue)
{
    size_t idx;
    for (idx = 0; idx < numDefs; ++idx) {
        virDomainNetDef *net = netDevDefs[idx];
        if (!virDomainNetIsVirtioModel(net)) {
            DBG("Found non PCI net device with model type %s", virDomainNetModelTypeToString(net->model));
            continue;
        }
        if (chReserveOrQueueForPciSlotId(addrSet, &net->info, queue))
            return -1;
    }
    return 0;
}

/**
 * Initialize all disk devices in the domain with an PCI slot ID or places them in a queue for later assignment.
 *
 * Device without a slot ID given in the XML are queued for later ID assignment and therefore not initialized by this 
 * function.
 *
 * @addrSet: PciAddressSet to reserve the slot IDs from
 * @numDefs: Length of the disk device array
 * @deviceDevs: Array of disk devices definitions of a domain
 * @queue: Queue of DeviceInfos for which a slot ID is reserved later
 *
 * Returns: 0 on success, -1 in case of error 
 */
static int chInitDiskVirtioPciDevices(virDomainPCIAddressSet *addrSet,
                                      size_t numDefs,
                                      virDomainDiskDef **diskDevDefs,
                                      dynamicAddressQueue* queue)
{
    size_t idx;
    for (idx = 0; idx < numDefs; ++idx) {
        virDomainDiskDef *disk = diskDevDefs[idx];
        if (disk->bus != VIR_DOMAIN_DISK_BUS_VIRTIO) {
            DBG("Found unsupported disk model with type %d", disk->model);
            continue;
        }
        if (chReserveOrQueueForPciSlotId(addrSet, &disk->info, queue))
            return -1;
    }
    return 0;
}

/**
 * Assigns static PCI device BDFs to devices with not BDF specified.
 *
 * @addrSet: PciAddrSet to reserve the PCI BDFs from
 * @queue: Queue of devices for which a PCI BDF will be reserved
 *
 * Returns: 0 on success, -1 in case of error 
 */
static int chReserveDynamicPciIds(virDomainPCIAddressSet *addrSet, dynamicAddressQueue *queue) {
    for (size_t idx = 0; idx < queue->nextIdx; ++idx) {
        queue->devInfos[idx]->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
        if (virDomainPCIAddressReserveNextAddr(addrSet, queue->devInfos[idx],
                queue->devInfos[idx]->pciConnectFlags, -1))
        {
            virReportError(VIR_ERR_INTERNAL_ERROR,
            _("Failed to fix the PCI device id for '%s'!"), queue->devInfos[idx]->alias);
            return -1;
        }
        DBG("Assigned new Slot %d to device '%s' without BDF",
            queue->devInfos[idx]->addr.pci.slot,
            queue->devInfos[idx]->alias);
    }
    return 0;
}

/* Initializes all PCI devices in the configuration with PCI bus addresses.
 * 
 * First allocates all PCI slot IDs given by the XML. Then allocates slot IDs for all devices without a fixed slot ID.
 * 
 * Succeeds if after successfully assigning an address to each device.
 *
 * @dom: Domain object for which all PCI devices are initialized with an PCI address.
 * 
 * 0 indicates success, -1 failure 
 */
static int chInitPciDevices(virDomainDef *def, virDomainPCIAddressSet *addrSet) {
    dynamicAddressQueue* queue = NULL;
    int rc = 0;
    new_addr_queue(&queue);

    /* Either allocate PCI slot IDs for all devices of all device classes if their XML contains a fixed slot id
    * or place them in a queue to later assign fixes slot IDs. This is necessary as there is no order with respect
    * to fixed IDs. Moreover, there is no guarantee that even if there was an order in one device class, there would be
    * fixed slot ID in a class that is later initialized. So we need a global queue of all devices without a fixed slot 
    * ID.
    */ 
    if (chInitRngVirtioPciDevices(addrSet, &def->nrngs, &def->rngs, queue)) {
        rc = -1;
        VIR_ERROR("%s:%d: Failed to attach Virtio RNG devices.", __FILE_NAME__, __LINE__);
        goto cleanup;
    }
    if (chInitNetworkVirtioPciDevices(addrSet,def->nnets, def->nets, queue)) {
        rc = -1;
        VIR_ERROR("%s:%d: Failed to attach Virtio net devices.", __FILE_NAME__, __LINE__);
        goto cleanup;
    }
    if (chInitDiskVirtioPciDevices(addrSet,def->ndisks, def->disks, queue)) {
        rc = -1;
        VIR_ERROR("%s:%d: Failed to attach Virtio disk devices.", __FILE_NAME__, __LINE__);
        goto cleanup;
    }
    // Assign a fixed slot ID to all devices in the queue
    if (chReserveDynamicPciIds(addrSet, queue)) {
        rc = -1;
        VIR_ERROR("%s:%d: Could not assigned fixed PCI device IDs", __FILE_NAME__, __LINE__);
    }

    cleanup:
    free_queue(&queue);

    return rc;
}

/*
 * Assigns PCI Addresses to all devices in a definition and persists them if 
 * there is a domain object.
 */
int chAssignPciAddresses(virDomainDef *def, virDomainObj *obj) {
    virDomainPCIAddressSet *addr_set = NULL;
    virCHDomainObjPrivate *priv = NULL;
    int rc = -1;
    // If there is an object, we want to persist `addr_set`. If not, we just ditch it.
    if (obj) {
        if(obj->privateData) {
            priv = obj->privateData;
            addr_set = priv->pciAddrSet;
        } else {
            // An object without privateData initialized seems like a bad thing (shouldn't exists).
            VIR_ERROR("%s:%d: Cannot allocate PCI addresses! Domain object supplied but private Data not initialized!", 
                      __FILE_NAME__,
                      __LINE__);
            return -1;
        }
    }

    if (!addr_set) {
        if (!(addr_set = chDomainPCIAddressSetCreate(def))) {
            VIR_ERROR("%s:%d: Address set allocation failed!", __FILE_NAME__, __LINE__);
            return -1;
        };
    }

    if (!addr_set->buses) {
        VIR_ERROR("%s:%d: Number of PCI busses is zero!", __FILE_NAME__, __LINE__);
        goto cleanup;
    }

    // We support exactly one PCI bus currently. We need to set the respective model for usign it
    if (virDomainPCIAddressBusSetModel(&addr_set->buses[0], VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT, true) < 0)
        goto cleanup;

    // Add all devices to the address set
    if (chInitPciDevices(def, addr_set)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "PCI address assignment failed! See above.");
        goto cleanup;
    }

    // We got a running domain object, so we persist the PCI address set
    if (priv) {
        priv->pciAddrSet = g_steal_pointer(&addr_set);
    }
    rc = 0;

    cleanup:
    virDomainPCIAddressSetFree(addr_set);

    return rc;
}

/* Ensures that a hotplugged PCI devices has a associated address.
 *
 * Performs all necessary configuration for a hotplugable PCI device. 
 * Succeeds after successfully assigning an address to the device.
 * 
 * 0 indicates success, -1 failure 
 */
int chEnsurePciAddress(virDomainObj *obj, virDomainDeviceDef *dev) {
    virCHDomainObjPrivate *priv = obj->privateData;
    virDomainDeviceInfo *info = virDomainDeviceGetInfo(dev);
    if (!info)
        return 0;

    info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
    info->pciConnectFlags = VIR_PCI_CONNECT_TYPE_PCI_DEVICE | VIR_PCI_CONNECT_HOTPLUGGABLE;

    return virDomainPCIAddressEnsureAddr(priv->pciAddrSet, info,
                                         info->pciConnectFlags);
}

/**
 * Releases the PCI bus address reserved for a PCI device.
 *
 * @vm: VM from which the PCI address is released
 * @info: DeviceInfo that holds information about the PCI address to release.
 */
void chDomainReleaseDeviceAddress(virDomainObj *vm, virDomainDeviceInfo *info)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    if (virDeviceInfoPCIAddressIsPresent(info)) {
        virDomainPCIAddressReleaseAddr(priv->pciAddrSet, &info->addr.pci);
    }
}
