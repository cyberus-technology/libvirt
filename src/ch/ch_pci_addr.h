#pragma once

#include "domain_addr.h"
int chDomainPCIAddressSetCreate(virDomainObj *obj);
int chInitPciDevices(virDomainObj *dom);

int chAssignStaticPciDeviceId(virDomainDeviceInfo *devInfo,
                              virDomainPCIAddressSet *addrs);

int chEnsurePciAddress(virDomainObj *obj, virDomainDeviceDef *dev);
void chDomainReleaseDeviceAddress(virDomainObj *vm, virDomainDeviceInfo *info);
