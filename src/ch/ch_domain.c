/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_domain.c: Domain manager functions for Cloud-Hypervisor driver
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "ch_domain.h"
#include "domain_driver.h"
#include "domain_validate.h"
#include "virchrdev.h"
#include "virlog.h"
#include "virtime.h"
#include "virsystemd.h"
#include "datatypes.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_LOG_INIT("ch.ch_domain");

void
virCHDomainRemoveInactive(virCHDriver *driver,
                          virDomainObj *vm)
{
    if (!vm->persistent) {
        virDomainObjListRemove(driver->domains, vm);
    }
}

static void *
virCHDomainObjPrivateAlloc(void *opaque)
{
    virCHDomainObjPrivate *priv;

    priv = g_new0(virCHDomainObjPrivate, 1);

    if (!(priv->chrdevs = virChrdevAlloc())) {
        g_free(priv);
        return NULL;
    }
    priv->driver = opaque;

    return priv;
}

static void
virCHDomainObjPrivateFree(void *data)
{
    virCHDomainObjPrivate *priv = data;

    virChrdevFree(priv->chrdevs);
    g_free(priv->machineName);
    virBitmapFree(priv->autoCpuset);
    virBitmapFree(priv->autoNodeset);
    virCgroupFree(priv->cgroup);
    g_free(priv->pidfile);
    g_free(priv);
}

static int
virCHDomainDefPostParseBasic(virDomainDef *def,
                             void *opaque G_GNUC_UNUSED)
{
    /* check for emulator and create a default one if needed */
    if (!def->emulator) {
        if (!(def->emulator = g_find_program_in_path(CH_CMD))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("No emulator found for cloud-hypervisor"));
            return 1;
        }
    }

    return 0;
}

static virClass *virCHDomainVcpuPrivateClass;

static void
virCHDomainVcpuPrivateDispose(void *obj G_GNUC_UNUSED)
{
}

static int
virCHDomainVcpuPrivateOnceInit(void)
{
    if (!VIR_CLASS_NEW(virCHDomainVcpuPrivate, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virCHDomainVcpuPrivate);

static virObject *
virCHDomainVcpuPrivateNew(void)
{
    virCHDomainVcpuPrivate *priv;

    if (virCHDomainVcpuPrivateInitialize() < 0)
        return NULL;

    if (!(priv = virObjectNew(virCHDomainVcpuPrivateClass)))
        return NULL;

    return (virObject *) priv;
}


static int
virCHDomainDefPostParse(virDomainDef *def,
                        unsigned int parseFlags G_GNUC_UNUSED,
                        void *opaque,
                        void *parseOpaque G_GNUC_UNUSED)
{
    virCHDriver *driver = opaque;
    g_autoptr(virCaps) caps = virCHDriverGetCapabilities(driver, false);

    if (!caps)
        return -1;

    if (!virCapabilitiesDomainSupported(caps, def->os.type,
                                        def->os.arch,
                                        def->virtType,
                                        true))
        return -1;

    return 0;
}

virDomainXMLPrivateDataCallbacks virCHDriverPrivateDataCallbacks = {
    .alloc = virCHDomainObjPrivateAlloc,
    .free = virCHDomainObjPrivateFree,
    .vcpuNew = virCHDomainVcpuPrivateNew,
};

static int
chValidateDomainDeviceDef(const virDomainDeviceDef *dev,
                          const virDomainDef *def,
                          void *opaque,
                          void *parseOpaque G_GNUC_UNUSED)
{
    virCHDriver *driver = opaque;
    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
    case VIR_DOMAIN_DEVICE_NET:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_RNG:
        break;

    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Cloud-Hypervisor doesn't support '%1$s' device"),
                       virDomainDeviceTypeToString(dev->type));
        return -1;

    case VIR_DOMAIN_DEVICE_NONE:
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unexpected VIR_DOMAIN_DEVICE_NONE"));
        return -1;

    case VIR_DOMAIN_DEVICE_LAST:
    default:
        virReportEnumRangeError(virDomainDeviceType, dev->type);
        return -1;
    }

    if (!virBitmapIsBitSet(driver->chCaps, CH_SERIAL_CONSOLE_IN_PARALLEL)) {
        if ((def->nconsoles &&
             def->consoles[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY) &&
            (def->nserials &&
             def->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only a single console or serial can be configured for this domain"));
            return -1;
        }
    }

    if (def->nconsoles > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Only a single console can be configured for this domain"));
        return -1;
    }

    if (def->nrngs > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Only a single RNG device can be configured for this domain"));
        return -1;
    }

    if (def->nserials > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Only a single serial can be configured for this domain"));
        return -1;
    }

    if (def->nconsoles && def->consoles[0]->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Console only works in PTY mode"));
        return -1;
    }

    if (def->nserials) {
        if (def->serials[0]->source->type != VIR_DOMAIN_CHR_TYPE_PTY &&
            def->serials[0]->source->type != VIR_DOMAIN_CHR_TYPE_UNIX) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Serial only works in UNIX/PTY modes"));
            return -1;
        }
        if (!virBitmapIsBitSet(driver->chCaps, CH_SOCKET_BACKEND_SERIAL_PORT) &&
            def->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_UNIX) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unix Socket backend is not supported by this version of ch."));
            return -1;
        }
    }

    return 0;
}

void
virCHDomainRefreshThreadInfo(virDomainObj *vm)
{
    unsigned int maxvcpus = virDomainDefGetVcpusMax(vm->def);
    virCHMonitorThreadInfo *info = NULL;
    size_t nthreads;
    size_t ncpus = 0;
    size_t i;

    nthreads = virCHMonitorGetThreadInfo(virCHDomainGetMonitor(vm),
                                         true, &info);

    for (i = 0; i < nthreads; i++) {
        virCHDomainVcpuPrivate *vcpupriv;
        virDomainVcpuDef *vcpu;
        virCHMonitorCPUInfo *vcpuInfo;

        if (info[i].type != virCHThreadTypeVcpu)
            continue;

        /* TODO: hotplug support */
        vcpuInfo = &info[i].vcpuInfo;

        if ((vcpu = virDomainDefGetVcpu(vm->def, vcpuInfo->cpuid))) {
            vcpupriv = CH_DOMAIN_VCPU_PRIVATE(vcpu);
            vcpupriv->tid = vcpuInfo->tid;
            ncpus++;
        } else {
            VIR_WARN("vcpu '%d' reported by hypervisor but not found in definition",
                     vcpuInfo->cpuid);
        }
    }

    /* TODO: Remove the warning when hotplug is implemented.*/
    if (ncpus != maxvcpus)
        VIR_WARN("Mismatch in the number of cpus, expected: %u, actual: %zu",
                 maxvcpus, ncpus);
}

virDomainDefParserConfig virCHDriverDomainDefParserConfig = {
    .domainPostParseBasicCallback = virCHDomainDefPostParseBasic,
    .domainPostParseCallback = virCHDomainDefPostParse,
    .deviceValidateCallback = chValidateDomainDeviceDef,
    .features = VIR_DOMAIN_DEF_FEATURE_NO_STUB_CONSOLE |
                VIR_DOMAIN_DEF_FEATURE_USER_ALIAS,
};

virCHMonitor *
virCHDomainGetMonitor(virDomainObj *vm)
{
    return CH_DOMAIN_PRIVATE(vm)->monitor;
}

pid_t
virCHDomainGetVcpuPid(virDomainObj *vm,
                      unsigned int vcpuid)
{
    virDomainVcpuDef *vcpu = virDomainDefGetVcpu(vm->def, vcpuid);

    return CH_DOMAIN_VCPU_PRIVATE(vcpu)->tid;
}

bool
virCHDomainHasVcpuPids(virDomainObj *vm)
{
    size_t i;
    size_t maxvcpus = virDomainDefGetVcpusMax(vm->def);
    virDomainVcpuDef *vcpu;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);

        if (CH_DOMAIN_VCPU_PRIVATE(vcpu)->tid > 0)
            return true;
    }

    return false;
}

char *
virCHDomainGetMachineName(virDomainObj *vm)
{
    virCHDomainObjPrivate *priv = CH_DOMAIN_PRIVATE(vm);
    virCHDriver *driver = priv->driver;
    char *ret = NULL;

    if (vm->pid != 0) {
        ret = virSystemdGetMachineNameByPID(vm->pid);
        if (!ret)
            virResetLastError();
    }

    if (!ret)
        ret = virDomainDriverGenerateMachineName("ch",
                                                 NULL,
                                                 vm->def->id, vm->def->name,
                                                 driver->privileged);

    return ret;
}

/**
 * virCHDomainObjFromDomain:
 * @domain: Domain pointer that has to be looked up
 *
 * This function looks up @domain and returns the appropriate virDomainObjPtr
 * that has to be released by calling virDomainObjEndAPI().
 *
 * Returns the domain object with incremented reference counter which is locked
 * on success, NULL otherwise.
 */
virDomainObj *
virCHDomainObjFromDomain(virDomainPtr domain)
{
    virDomainObj *vm;
    virCHDriver *driver = domain->conn->privateData;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    vm = virDomainObjListFindByUUID(driver->domains, domain->uuid);
    if (!vm) {
        virUUIDFormat(domain->uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%1$s' (%2$s)"),
                       uuidstr, domain->name);
        return NULL;
    }

    return vm;
}

int
virCHDomainValidateActualNetDef(virDomainNetDef *net)
{
    virDomainNetType actualType = virDomainNetGetActualType(net);

    /* hypervisor-agnostic validation */
    if (virDomainActualNetDefValidate(net) < 0)
        return -1;

    /* CH specific validation */
    switch (actualType) {
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        if (net->guestIP.nips > 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("ethernet type supports a single guest ip"));
            return -1;
        }
        break;
    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_VDPA:
    case VIR_DOMAIN_NET_TYPE_NULL:
    case VIR_DOMAIN_NET_TYPE_VDS:
    case VIR_DOMAIN_NET_TYPE_LAST:
    default:
        break;
    }

    return 0;
}

#define MAX_PCI_SLOTS 32

static bool
chDomainPCISlotIsAvailable(virDomainDef *def, size_t slot) {
    size_t i;

    /* virtio-blk disks */
    for (i = 0; i < def->ndisks; i++) {
        const virDomainDiskDef *disk = def->disks[i];

        if (disk->info.addr.pci.slot == slot) {
            return false;
        }
    }

    /* virtio-net devices */
    for (i = 0; i < def->nnets; i++) {
        const virDomainNetDef *net = def->nets[i];

        if (net->info.addr.pci.slot == slot) {
            return false;
        }
    }

    return true;
}

// TODO make this more robust; there are a few PCI slots occupied by
//  pre-existing devices
#define CHV_MIN_PCI_SLOT 4

/*
 * Finds the next PCI slot that is available.
 *
 * Only `MAX_PCI_SLOTS` slots/devices in total are supported for now.
 */
static int
chDomainFindNextPCISlot(virDomainDef *def) {
    size_t i;
    int slot_candidate = -1;

    for (i = CHV_MIN_PCI_SLOT; i < MAX_PCI_SLOTS; i++) {
        if (chDomainPCISlotIsAvailable(def, i)) {
            slot_candidate = (int) i;
            break;
        }
    }

    if (slot_candidate == -1) {
        VIR_ERROR("didn't find a free PCI slot for device");
    }

    return slot_candidate;
}

/*
 * Assigns a PCI slot to a device. For simplicity, only `MAX_PCI_SLOTS` PCI slots
 * in total are supported at the moment. This function can be used for static
 * devices and hotplugged devices.
 */
int
chDomainAssignDevicePCISlot(virDomainDef *def, virDomainDeviceInfo* info, int slot) {
    if (slot == -1) {
        slot = chDomainFindNextPCISlot(def);
        if (slot == -1) {
            VIR_ERROR("Couldn't find a free slot for PCI device %s", info->alias);
            return -1;
        }
    }

    // Populate the <interface type="pci"> XML tag, relevant for OpenStack
    // Currently not needed to have sane values here.
    info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
    info->addr.pci.bus = 0;
    info->addr.pci.slot = slot;
    info->addr.pci.function = 0;

    VIR_WARN("assigned PCI address 0:%d:0 to device %s", slot, info->alias);

    return 0;
}

/*
 * This assigns static PCI slots to all configured devices.
 *
 * Respects PCI addresses that are already in the XML.
 */
// TODO I'm not sure if the handling here is already graceful and mature enough.
// Especially when it comes to statically assigning values or keeping them if
// they are already configured.
static int
chDomainAssignDevicePCISlots(virDomainDef *def)
{
    size_t i, slot = 0;

    /* virtio-blk disks */
    for (i = 0; i < def->ndisks; i++) {
        virDomainDiskDef *disk = def->disks[i];

        // Skip in case an address comes already from the XML.
        if (disk->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI && disk->info.addr.pci.slot != 0) {
            VIR_WARN("keeping already assigned PCI address 0:%ld:0 for device %s", slot, disk->info.alias);
            continue;
        }

        if (chDomainAssignDevicePCISlot(def, &disk->info, -1) < 0) {
            VIR_ERROR("failed to assign PCI slot to disk device %s", disk->info.alias);
            return -1;
        }
        slot += 1;
    }

    /* virtio-net devices */
    for (i = 0; i < def->nnets; i++) {
        virDomainNetDef *net = def->nets[i];

        // Skip in case an address comes already from the XML.
        if (net->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI && net->info.addr.pci.slot != 0) {
            VIR_WARN("keeping already assigned PCI address 0:%ld:0 for device %s", slot, net->info.alias);
            continue;
        }

        if (chDomainAssignDevicePCISlot(def, &net->info, -1) < 0) {
            VIR_ERROR("failed to assign PCI slot to net device %s", net->info.alias);
            return -1;
        }
        slot += 1;
    }

    return 0;
}

static int
chDomainAssignPCIAddresses(virDomainDef *def)
{
    if (chDomainAssignDevicePCISlots(def) < 0)
        return -1;

    return 0;
}

int
chDomainAssignAddresses(virDomainDef *def)
{
    if (chDomainAssignPCIAddresses(def) < 0)
        return -1;

    return 0;
}
