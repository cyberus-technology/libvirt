#include <config.h>

#include "ch_alias.h"

#include "domain_conf.h"
#include <unistd.h>

#include "virutil.h"
#include "virlog.h"



#define VIR_FROM_THIS VIR_FROM_CH

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

int
chAssignDeviceAliases(virDomainDef *def)
{
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (chAssignDeviceDiskAlias(def, def->disks[i]) < 0)
            return -1;
    }

    /*for (i = 0; i < def->nnets; i++) {
        chAssignDeviceNetAlias(def, def->nets[i], -1);
    }*/

	// TODO other devices*/

    return 0;
}
