/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_monitor.h: header file for managing Cloud-Hypervisor interactions
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

#include <curl/curl.h>

#include "virobject.h"
#include "virjson.h"
#include "ch_conf.h"

#define URL_ROOT "http://localhost/api/v1"
#define URL_VMM_SHUTDOWN "vmm.shutdown"
#define URL_VM_CREATE "vm.create"
#define URL_VM_DELETE "vm.delete"
#define URL_VM_BOOT "vm.boot"
#define URL_VM_SHUTDOWN "vm.shutdown"
#define URL_VM_REBOOT "vm.reboot"
#define URL_VM_Suspend "vm.pause"
#define URL_VM_RESUME "vm.resume"
#define URL_VM_INFO "vm.info"
#define URL_VM_SAVE "vm.snapshot"
#define URL_VM_RESTORE "vm.restore"
#define URL_VM_RECEIVE_MIGRATION "vm.receive-migration"
#define URL_VM_SEND_MIGRATION "vm.send-migration"
#define URL_VM_REMOVE_DEVICE "vm.remove-device"
#define URL_VM_ADD_DISK "vm.add-disk"
#define URL_VM_RESIZE_DISK "vm.resize-disk"
#define URL_VM_MIGRATION_PROGRESS "vm.migration-progress"

#define VIRCH_THREAD_NAME_LEN   16

#define CH_NET_ID_PREFIX "net"

typedef enum {
    virCHThreadTypeEmulator,
    virCHThreadTypeVcpu,
    virCHThreadTypeIO,
    virCHThreadTypeMax
} virCHThreadType;

typedef struct _virCHMonitorCPUInfo virCHMonitorCPUInfo;

struct _virCHMonitorCPUInfo {
    int cpuid;
    pid_t tid;

    bool online;
};

typedef struct _virCHMonitorEmuThreadInfo virCHMonitorEmuThreadInfo;

struct _virCHMonitorEmuThreadInfo {
    char    thrName[VIRCH_THREAD_NAME_LEN];
    pid_t   tid;
};

typedef struct _virCHMonitorIOThreadInfo virCHMonitorIOThreadInfo;

struct _virCHMonitorIOThreadInfo {
    char    thrName[VIRCH_THREAD_NAME_LEN];
    pid_t   tid;
};

typedef struct _virCHMonitorThreadInfo virCHMonitorThreadInfo;

struct _virCHMonitorThreadInfo {
    virCHThreadType type;

    union {
        virCHMonitorCPUInfo vcpuInfo;
        virCHMonitorEmuThreadInfo emuInfo;
        virCHMonitorIOThreadInfo ioInfo;
    };
};

typedef struct _virCHMonitor virCHMonitor;

struct _virCHMonitor {
    virObjectLockable parent;

    char *socketpath;

    char *eventmonitorpath;
    int eventmonitorfd;

    virThread event_handler_thread;
    int event_handler_stop;
    struct {
        /* Buffer to hold the data read from pipe */
        char *buffer;
        /* Size of the data read from pipe into buffer */
        size_t buf_fill_sz;
    } event_buffer;

    virDomainObj *vm;

    size_t nthreads;
    virCHMonitorThreadInfo *threads;
};

typedef enum {
    VIR_CH_MIGRATION_PROGRESS_STATE_INVALID = 0,
    VIR_CH_MIGRATION_PROGRESS_STATE_CANCELLED,
    VIR_CH_MIGRATION_PROGRESS_STATE_FAILED,
    VIR_CH_MIGRATION_PROGRESS_STATE_FINISHED,
    VIR_CH_MIGRATION_PROGRESS_STATE_ONGOING,

    VIR_CH_MIGRATION_PROGRESS_STATE_LAST
} virCHMigrationProgressState;

VIR_ENUM_DECL(virCHMigrationProgressState);

typedef enum {
    VIR_CH_MIGRATION_PROGRESS_ONGOING_PHASE_INVALID = 0,
    VIR_CH_MIGRATION_PROGRESS_ONGOING_PHASE_STARTING,
    VIR_CH_MIGRATION_PROGRESS_ONGOING_PHASE_MEMORY_FDS,
    VIR_CH_MIGRATION_PROGRESS_ONGOING_PHASE_MEMORY_PRECOPY,
    VIR_CH_MIGRATION_PROGRESS_ONGOING_PHASE_COMPLETING,

    VIR_CH_MIGRATION_PROGRESS_ONGOING_PHASE_LAST
} virCHMigrationProgressOngoingPhase;

VIR_ENUM_DECL(virCHMigrationProgressOngoingPhase);

typedef enum {
    VIR_CH_MIGRATION_TRANSPORT_MODE_LOCAL = 0,
    VIR_CH_MIGRATION_TRANSPORT_MODE_TCP,

    VIR_CH_MIGRATION_TRANSPORT_MODE_LAST
} virCHMigrationTransportMode;

VIR_ENUM_DECL(virCHMigrationTransportMode);

/**
 * C-friendly representation of the migration progress.
 */
typedef struct _chMigrationProgress chMigrationProgress;
struct _chMigrationProgress {
    uint8_t vcpu_throttle_percent;
    uint64_t timestamp_begin_ms;
    uint64_t timestamp_snapshot_ms;
    uint64_t timestamp_snapshot_relative_ms;
    uint64_t downtime_configured_ms;
    uint64_t downtime_estimated_ms;
    // "Tcp" or "Local"
    // String with static lifetime.
    virCHMigrationTransportMode transportation_mode;
    uint16_t tcp_connections;
    bool tcp_tls;
    virCHMigrationProgressState state;
    virCHMigrationProgressOngoingPhase ongoing_phase;
    struct {
        uint64_t memory_iteration;
        uint64_t memory_transmission_bps;
        uint64_t memory_bytes_total;
        uint64_t memory_bytes_transmitted;
        uint64_t memory_bytes_remaining_iteration;
        uint64_t memory_pages_4k_transmitted;
        uint64_t memory_pages_4k_remaining_iteration;
        uint64_t memory_pages_constant_count;
        uint64_t memory_dirty_rate_pps;
    } memory_transmission_info;
};


virCHMonitor *virCHMonitorNew(virDomainObj *vm, virCHDriverConfig *cfg,
                              int logfile);
virCHMonitor *
virCHMonitorReattach(virDomainObj *vm, virCHDriverConfig *cfg, virCHDriver *driver);

void virCHMonitorClose(virCHMonitor *mon);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(virCHMonitor, virCHMonitorClose);


int virCHMonitorCreateVM(virCHDriver *driver, virCHMonitor *mon);
int virCHMonitorBootVM(virCHMonitor *mon);
int virCHMonitorShutdownVM(virCHMonitor *mon);
int virCHMonitorShutdownVMM(virCHMonitor *mon);
int virCHMonitorRebootVM(virCHMonitor *mon);
int virCHMonitorSuspendVM(virCHMonitor *mon);
int virCHMonitorResumeVM(virCHMonitor *mon);
int virCHMonitorSaveVM(virCHMonitor *mon,
                       const char *to);
int virCHMonitorMigrationSend(virCHMonitor *mon,
                              const char *dst_uri,
                              unsigned parallel_connections,
                              bool use_tls,
                              char *tls_dir);
int virCHMonitorMigrationReceive(virCHMonitor *mon,
                                 const char *rcv_uri,
                                 virDomainDef *vmdef,
                                 virCHDriver *driver,
                                 virCond *cond,
                                 char* tcp_serial_url,
                                 bool use_tls,
                                 virJSONValue *mem_zones);
int virCHMonitorRemoveDevice(virCHMonitor *mon, const char* device_id);
int virCHMonitorGetInfo(virCHMonitor *mon, virJSONValue **info);

size_t virCHMonitorGetThreadInfo(virCHMonitor *mon, bool refresh,
                                 virCHMonitorThreadInfo **threads);
int virCHMonitorGetIOThreads(virCHMonitor *mon,
                             virDomainIOThreadInfo ***iothreads);
int
virCHMonitorBuildDiskJson(virJSONValue *disks, virDomainDiskDef *diskdef);

int
virCHMonitorBuildNetJson(virDomainNetDef *netdef,
                         int netindex,
                         char **jsonstr);
int virCHMonitorBuildRestoreJson(virDomainDef *vmdef,
                                 const char *from,
                                 char **jsonstr);
int
virCHMonitorPutNoResponse(virCHMonitor *mon, const char *endpoint,
                const char *payload);
virJSONValue*
virCHMonitorPut(virCHMonitor *mon, const char *endpoint,
                const char *payload);

int
virCHMonitorBuildMemoryZonesJson(virJSONValue *content, virDomainDef *def);

int
chMonitorJSONGetMigrationStatsReply(virCHMonitor *mon,
                                    chMigrationProgress *progress);
