#pragma once

#include "domain_addr.h"
int chAssignStaticPciDeviceId(virDomainDeviceInfo *devInfo,
                              virDomainPCIAddressSet *addrs);

int chEnsurePciAddress(virDomainObj *obj, virDomainDeviceDef *dev);
void chDomainReleaseDeviceAddress(virDomainObj *vm, virDomainDeviceInfo *info);
int chAssignPciAddresses(virDomainDef *def, virDomainObj *obj);
