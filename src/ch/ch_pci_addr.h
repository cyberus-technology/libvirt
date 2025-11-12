/*
 * Copyright Cyberus Technology GmbH. 2025
 *
 * ch_pci_addr.h: header file for Cloud-Hypervisor's PCI device address
 * management
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "domain_addr.h"
int chDomainPCIAddressSetCreate(virDomainObj *obj);
int chInitPciDevices(virDomainObj *dom);

int chAssignStaticPciDeviceId(virDomainDeviceInfo *devInfo,
                              virDomainPCIAddressSet *addrs);

int chEnsurePciAddress(virDomainObj *obj, virDomainDeviceDef *dev);
void chDomainReleaseDeviceAddress(virDomainObj *vm, virDomainDeviceInfo *info);
