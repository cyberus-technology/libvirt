#pragma once

#include "domain_conf.h"

int
chAssignDeviceDiskAlias(virDomainDef *def,
                        virDomainDiskDef *disk);

void
chAssignDeviceNetAlias(virDomainDef *def, virDomainNetDef *net);

int
chAssignDeviceAliases(virDomainDef *def);

int
chDomainReconcileDiskAliases(virDomainObj *vm, virDomainDef *persistentDef);
