#pragma once

#include "domain_conf.h"

#define CH_NET_ID_PREFIX "net"

int
chAssignDeviceDiskAlias(virDomainDef *def,
                        virDomainDiskDef *disk);

void
chAssignDeviceNetAlias(virDomainDef *def,
                       virDomainNetDef *net,
                       int idx);

int
chAssignDeviceAliases(virDomainDef *def);
