/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_process.h: header file for Cloud-Hypervisor's process controller
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

#pragma once

#include "ch_conf.h"
#include "internal.h"

int virCHProcessInit(virCHDriver *driver,
                     virDomainObj *vm);

int virCHProcessStart(virCHDriver *driver,
                      virDomainObj *vm,
                      virDomainRunningReason reason);
int virCHProcessStop(virCHDriver *driver,
                     virDomainObj *vm,
                     virDomainShutoffReason reason);
int virCHProcessKill(virCHDriver *driver,
                     virDomainObj *vm,
                     virDomainShutoffReason reason);

int virCHProcessStartRestore(virCHDriver *driver,
                         virDomainObj *vm,
                         const char *from);

int virCHProcessUpdateInfo(virDomainObj *vm);

int virCHProcessInitNetwork(virCHDriver *driver,
                            virDomainObj *vm);

int
chProcessAddNetworkDevice(virCHDriver *driver,
                          virCHMonitor *mon,
                          virDomainDef *vmdef,
                          virDomainNetDef *net);

int
chMonitorSocketConnect(virCHMonitor *mon);

/**
 * virCHProcessSetup:
 * @vm: domain object
 *
 * Applies CPU affinity, scheduler, and cgroup tuning to a running
 * Cloud Hypervisor domain.
 *
 * Must be called after the hypervisor process is started and the
 * monitor is connected, when all emulator, IO, and vCPU threads
 * exist and their TIDs can be queried.
 *
 * Performs:
 *   - Refresh of runtime thread IDs from the monitor
 *   - Affinity and scheduler setup for emulator threads
 *   - Affinity and scheduler setup for IO threads
 *   - Global CPU bandwidth (cgroup) configuration
 *   - Per-vCPU affinity and bandwidth setup
 *   - Runtime info synchronization
 *
 * Ensures that the CPU tuning defined in the domain XML is enforced
 * on the actual Linux threads of the VM process.
 *
 * Returns 0 on success, -1 on error.
 */
int
virCHProcessSetup(virDomainObj *vm);
