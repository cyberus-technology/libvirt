#include <config.h>

#include "ch_alias.h"
#include "ch_domain.h"

#include "domain_conf.h"
#include "storage_source.h"
#include <unistd.h>

#include "virstring.h"
#include "virutil.h"
#include "virlog.h"
#include "virerror.h"

#define VIR_FROM_THIS VIR_FROM_CH
#define CH_NET_ID_PREFIX "net_"

VIR_LOG_INIT("ch.ch_alias");

int
chAssignDeviceDiskAlias(virDomainDef *def,
                        virDomainDiskDef *disk)
{
    const char *prefix = virDomainDiskBusTypeToString(disk->bus);
    int controllerModel = -1;

    if (!disk->info.alias) {
        if (disk->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI) {
                virDomainControllerDef *cont;

                if (!(cont = virDomainDeviceFindSCSIController(def, &disk->info.addr.drive))) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unable to find a SCSI controller for idx=%1$d"),
                                   disk->info.addr.drive.controller);
                    return -1;
                }

                controllerModel = cont->model;

                if (controllerModel < 0)
                    return -1;
            }

            if (disk->bus != VIR_DOMAIN_DISK_BUS_SCSI ||
                controllerModel == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
                disk->info.alias = g_strdup_printf("%s%d-%d-%d", prefix,
                                                   disk->info.addr.drive.controller,
                                                   disk->info.addr.drive.bus,
                                                   disk->info.addr.drive.unit);
            } else {
                disk->info.alias = g_strdup_printf("%s%d-%d-%d-%d", prefix,
                                                   disk->info.addr.drive.controller,
                                                   disk->info.addr.drive.bus,
                                                   disk->info.addr.drive.target,
                                                   disk->info.addr.drive.unit);
            }
        } else {
            int idx = virDiskNameToIndex(disk->dst);
            disk->info.alias = g_strdup_printf("%s-disk%d", prefix, idx);
        }
    }

    return 0;
}

/**
 * Extract the index number of some device alias
 */
static int
chDomainDeviceAliasIndex(const virDomainDeviceInfo *info,
                         const char *prefix)
{
    int idx;

    if (!info->alias)
        return -1;
    if (!STRPREFIX(info->alias, prefix))
        return -1;

    if (virStrToLong_i(info->alias + strlen(prefix), NULL, 10, &idx) < 0)
        return -1;

    return idx;
}

void
chAssignDeviceNetAlias(virDomainDef *def, virDomainNetDef *net)
{
    size_t idx = 0;
    size_t i;

    if (net->info.alias) {
        return;
    }

    for (i = 0; i < def->nnets; i++) {
        int thisidx;

        if ((thisidx = chDomainDeviceAliasIndex(&def->nets[i]->info, CH_NET_ID_PREFIX)) < 0)
            continue;
        if (thisidx >= idx)
            idx = thisidx + 1;
    }

    net->info.alias = g_strdup_printf("%s%lu", CH_NET_ID_PREFIX, idx);
}

int
chAssignDeviceAliases(virDomainDef *def)
{
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (chAssignDeviceDiskAlias(def, def->disks[i]) < 0)
            return -1;
    }

    for (i = 0; i < def->nnets; i++) {
        chAssignDeviceNetAlias(def, def->nets[i]);
    }

    /* TODO: handle other devices */

    return 0;
}


/* Require matching PCI identity from the destination VMM. */
static bool
chDiskAliasMatchesAddress(virDomainDiskDef *disk, virJSONValue *chDisk)
{
    unsigned int slot;
    unsigned int segment;

    if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI ||
        virJSONValueObjectGetNumberUint(chDisk, "bdf_device", &slot) < 0 ||
        virJSONValueObjectGetNumberUint(chDisk, "pci_segment", &segment) < 0)
        return false;

    return disk->info.addr.pci.domain == segment &&
        disk->info.addr.pci.bus == 0 && disk->info.addr.pci.slot == slot &&
        disk->info.addr.pci.function == 0;
}


static const char *
chDiskAliasFromInfo(virDomainDiskDef *disk, virJSONValue *disks)
{
    virJSONValue *match = NULL;
    size_t matches = 0;
    size_t i;

    /* A matching alias is sufficient only if its identity also agrees. */
    for (i = 0; i < virJSONValueArraySize(disks); i++) {
        virJSONValue *chDisk = virJSONValueArrayGet(disks, i);
        const char *id = virJSONValueObjectGetString(chDisk, "id");
        const char *serial = virJSONValueObjectGetString(chDisk, "serial");

        if (id && *id && STREQ_NULLABLE(disk->info.alias, id) &&
            chDiskAliasMatchesAddress(disk, chDisk) &&
            (!disk->serial || !*disk->serial ||
             (serial && STREQLEN(disk->serial, serial, VIRTIO_BLK_ID_BYTES))))
            return id;
    }

    if (!disk->serial || !*disk->serial) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot reconcile alias for disk '%1$s' without a serial"),
                       disk->dst);
        return NULL;
    }

    for (i = 0; i < virJSONValueArraySize(disks); i++) {
        virJSONValue *chDisk = virJSONValueArrayGet(disks, i);
        const char *serial = virJSONValueObjectGetString(chDisk, "serial");

        if (serial && STREQLEN(disk->serial, serial, VIRTIO_BLK_ID_BYTES)) {
            match = chDisk;
            matches++;
        }
    }

    if (matches != 1) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot reconcile alias for disk '%1$s': expected one Cloud Hypervisor disk matching serial '%2$s' after truncation, found %3$zu"),
                       disk->dst, disk->serial, matches);
        return NULL;
    }

    if (!chDiskAliasMatchesAddress(disk, match)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot reconcile alias for disk '%1$s': PCI address does not match Cloud Hypervisor"),
                       disk->dst);
        return NULL;
    }

    {
        const char *id = virJSONValueObjectGetString(match, "id");

        if (!id || !*id) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Missing Cloud Hypervisor ID for disk '%1$s'"),
                           disk->dst);
            return NULL;
        }
        return id;
    }
}


struct chDiskAliasValidation {
    GHashTable *replacements;
    GHashTable *seen;
};

static int
chDiskAliasValidate(virDomainDef *def G_GNUC_UNUSED,
                    virDomainDeviceDef *dev G_GNUC_UNUSED,
                    virDomainDeviceInfo *info,
                    void *opaque)
{
    struct chDiskAliasValidation *data = opaque;
    const char *alias = g_hash_table_lookup(data->replacements, info);

    if (!alias)
        alias = info->alias;
    if (!alias)
        return 0;

    if (g_hash_table_contains(data->seen, alias)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot reconcile disk aliases: alias '%1$s' conflicts with another device"),
                       alias);
        return -1;
    }

    g_hash_table_add(data->seen, (void *) alias);
    return 0;
}


/* Caller holds the domain lock and a migration job. The VMM is the
 * authority for live disk IDs. persistentDef must be NULL or a definition newly
 * copied from the incoming live definition. Preserve pre-existing configuration.
 * Validate all replacements before applying any;
 * never repair an ambiguous match by guessing which disk the caller meant. */
int
chDomainReconcileDiskAliases(virDomainObj *vm, virDomainDef *persistentDef)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(priv->driver);
    g_autoptr(virJSONValue) info = NULL;
    g_autoptr(GHashTable) replacements = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    struct chDiskAliasValidation validation = { replacements, seen };
    virJSONValue *config;
    virJSONValue *disks;
    bool persistentChanged = false;
    size_t i;

    if (!vm->def->ndisks)
        return 0;

    if (virCHMonitorGetInfo(priv->monitor, &info) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Cannot query Cloud Hypervisor disk IDs"));
        return -1;
    }

    if (!info ||
        !(config = virJSONValueObjectGetObject(info, "config")) ||
        !(disks = virJSONValueObjectGetArray(config, "disks"))) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Missing disks in Cloud Hypervisor vm.info response"));
        return -1;
    }

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDef *disk = vm->def->disks[i];
        virDomainDiskDef *persistDisk = NULL;
        const char *id = chDiskAliasFromInfo(disk, disks);

        if (!id)
            return -1;

        if (STRNEQ_NULLABLE(disk->info.alias, id))
            g_hash_table_insert(replacements, &disk->info, (void *) id);

        if (persistentDef &&
            (persistDisk = virDomainDiskByTarget(persistentDef, disk->dst)) &&
            STREQ_NULLABLE(persistDisk->serial, disk->serial) &&
            virStorageSourceIsSameLocation(persistDisk->src, disk->src) &&
            STRNEQ_NULLABLE(persistDisk->info.alias, id)) {
            g_hash_table_insert(replacements, &persistDisk->info, (void *) id);
            persistentChanged = true;
        }
    }

    if (g_hash_table_size(replacements) == 0)
        return 0;

    if (virDomainDeviceInfoIterate(vm->def, chDiskAliasValidate, &validation) < 0)
        return -1;
    g_hash_table_remove_all(seen);
    if (persistentDef &&
        virDomainDeviceInfoIterate(persistentDef, chDiskAliasValidate, &validation) < 0)
        return -1;

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDef *disk = vm->def->disks[i];
        const char *id = g_hash_table_lookup(replacements, &disk->info);

        if (id) {
            VIR_WARN("DISK ALIAS REPAIRED: domain '%s', disk '%s': replacing "
                     "incorrect libvirt alias '%s' with Cloud Hypervisor disk ID '%s' "
                     "matched by serial '%s'. The VM remains running!",
                     vm->def->name, disk->dst, NULLSTR(disk->info.alias), id,
                     NULLSTR(disk->serial));
            g_free(disk->info.alias);
            disk->info.alias = g_strdup(id);
        }
    }

    if (persistentChanged) {
        for (i = 0; i < persistentDef->ndisks; i++) {
            virDomainDiskDef *disk = persistentDef->disks[i];
            const char *id = g_hash_table_lookup(replacements, &disk->info);

            if (id) {
                g_free(disk->info.alias);
                disk->info.alias = g_strdup(id);
            }
        }
    }

    /* Keep the corrected in-memory IDs even on save failure: they describe
     * the running VMM. The caller must report that persistence failed. */
    if (virDomainObjSave(vm, priv->driver->xmlopt, cfg->stateDir) < 0)
        return -1;
    if (persistentChanged &&
        virDomainDefSave(persistentDef, priv->driver->xmlopt, cfg->configDir) < 0)
        return -1;

    return 0;
}
