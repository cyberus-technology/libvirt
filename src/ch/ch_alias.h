#pragma once

#include "domain_conf.h"

int
chAssignDeviceDiskAlias(virDomainDef *def,
                        virDomainDiskDef *disk);

int
chAssignDeviceAliases(virDomainDef *def);
