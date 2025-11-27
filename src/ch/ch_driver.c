/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_driver.c: Core Cloud-Hypervisor driver functions
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

#include <config.h>
#include <fcntl.h>

#include "ch_alias.h"
#include "ch_capabilities.h"
#include "ch_conf.h"
#include "ch_domain.h"
#include "ch_driver.h"
#include "ch_monitor.h"
#include "ch_process.h"
#include "domain_capabilities.h"
#include "domain_cgroup.h"
#include "domain_event.h"
#include "domain_interface.h"
#include "domain_validate.h"
#include "domain_postparse.h"
#include "datatypes.h"
#include "driver.h"
#include "viralloc.h"
#include "viraccessapicheck.h"
#include "virchrdev.h"
#include "virerror.h"
#include "virjson.h"
#include "virinhibitor.h"
#include "virlog.h"
#include "virobject.h"
#include "virfile.h"
#include "virstring.h"
#include "virtime.h"
#include "virtypedparam.h"
#include "virutil.h"
#include "viruuid.h"
#include "virnuma.h"
#include "virhostmem.h"

#include "util/virportallocator.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_LOG_INIT("ch.ch_driver");

virCHDriver *ch_driver = NULL;

/* Functions */
static int
chConnectURIProbe(char **uri)
{
    if (ch_driver == NULL)
        return 0;

    *uri = g_strdup("ch:///system");
    return 1;
}

static virDrvOpenStatus chConnectOpen(virConnectPtr conn,
                                      virConnectAuthPtr auth G_GNUC_UNUSED,
                                      virConf *conf G_GNUC_UNUSED,
                                      unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    /* URI was good, but driver isn't active */
    if (ch_driver == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Cloud-Hypervisor state driver is not active"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (virConnectOpenEnsureACL(conn) < 0)
        return VIR_DRV_OPEN_ERROR;

    conn->privateData = ch_driver;

    return VIR_DRV_OPEN_SUCCESS;
}

static int chConnectClose(virConnectPtr conn)
{
    conn->privateData = NULL;
    return 0;
}

static const char *chConnectGetType(virConnectPtr conn)
{
    if (virConnectGetTypeEnsureACL(conn) < 0)
        return NULL;

    return "CH";
}

static int chConnectGetVersion(virConnectPtr conn,
                               unsigned long *version)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectGetVersionEnsureACL(conn) < 0)
        return -1;

    *version = driver->version;
    return 0;
}

static char *chConnectGetHostname(virConnectPtr conn)
{
    if (virConnectGetHostnameEnsureACL(conn) < 0)
        return NULL;

    return virGetHostname();
}

static int chConnectNumOfDomains(virConnectPtr conn)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectNumOfDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListNumOfDomains(driver->domains, true,
                                        virConnectNumOfDomainsCheckACL, conn);
}

static int chConnectListDomains(virConnectPtr conn, int *ids, int nids)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectListDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListGetActiveIDs(driver->domains, ids, nids,
                                     virConnectListDomainsCheckACL, conn);
}

static int
chConnectListAllDomains(virConnectPtr conn,
                        virDomainPtr **domains,
                        unsigned int flags)
{
    virCHDriver *driver = conn->privateData;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);

    if (virConnectListAllDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListExport(driver->domains, conn, domains,
                                 virConnectListAllDomainsCheckACL, flags);
}

static int chNodeGetInfo(virConnectPtr conn,
                         virNodeInfoPtr nodeinfo)
{
    if (virNodeGetInfoEnsureACL(conn) < 0)
        return -1;

    return virCapabilitiesGetNodeInfo(nodeinfo);
}

static char *chConnectGetCapabilities(virConnectPtr conn)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virCaps) caps = NULL;
    char *xml;

    if (virConnectGetCapabilitiesEnsureACL(conn) < 0)
        return NULL;

    if (!(caps = virCHDriverGetCapabilities(driver, true)))
        return NULL;

    xml = virCapabilitiesFormatXML(caps);

    return xml;
}

static char *
chDomainManagedSavePath(virCHDriver *driver, virDomainObj *vm)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    return g_strdup_printf("%s/%s.save", cfg->saveDir, vm->def->name);
}


/**
 * chDomainCreateXML:
 * @conn: pointer to connection
 * @xml: XML definition of domain
 * @flags: bitwise-OR of supported virDomainCreateFlags
 *
 * Creates a domain based on xml and starts it
 *
 * Returns a new domain object or NULL in case of failure.
 */
static virDomainPtr
chDomainCreateXML(virConnectPtr conn,
                  const char *xml,
                  unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virDomainDef) vmdef = NULL;
    virDomainObj *vm = NULL;
    virDomainPtr dom = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;
    g_autofree char *managed_save_path = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;

    cfg = virCHDriverGetConfig(driver);

    virCheckFlags(VIR_DOMAIN_START_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_START_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;

    /* Avoid parsing the whole domain definition for ACL checks */
    if (!(vmdef = virDomainDefIDsParseString(xml, driver->xmlopt, parse_flags)))
        return NULL;

    if (virDomainCreateXMLEnsureACL(conn, vmdef) < 0)
        return NULL;

    g_clear_pointer(&vmdef, virDomainDefFree);

    if ((vmdef = virDomainDefParseString(xml, driver->xmlopt,
                                         NULL, parse_flags)) == NULL)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains,
                                   &vmdef,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                       VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
        goto cleanup;

    /* cleanup if there's any stale managedsave dir */
    managed_save_path = chDomainManagedSavePath(driver, vm);
    if (virFileDeleteTree(managed_save_path) < 0) {
        virReportSystemError(errno,
                             _("Failed to cleanup stale managed save dir '%1$s'"),
                             managed_save_path);
        goto cleanup;
    }

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virCHProcessStart(driver, vm, VIR_DOMAIN_RUNNING_BOOTED) < 0)
        goto endjob;

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
        DBG("Failed to save status on vm %s", vm->def->name);
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    if (vm && !dom) {
        virCHDomainRemoveInactive(driver, vm);
    }
    virDomainObjEndAPI(&vm);
    return dom;
}

static int
chDomainCreateWithFlags(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virCHDomainObjPrivate *priv;
    g_autofree char *managed_save_path = NULL;
    int ret = -1;
    g_autoptr(virCHDriverConfig) cfg = NULL;

    cfg = virCHDriverGetConfig(driver);

    virCheckFlags(0, -1);

#ifdef COMMIT_HASH
    DBG("Commit hash: %s", COMMIT_HASH);
#endif

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainCreateWithFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (vm->hasManagedSave) {
        priv = vm->privateData;
        managed_save_path = chDomainManagedSavePath(driver, vm);
        if (virCHProcessStartRestore(driver, vm, managed_save_path) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to restore domain from managed save"));
            goto endjob;
        }
        if (virCHMonitorResumeVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to resume domain after restore from managed save"));
            goto endjob;
        }
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_RESTORED);
        /* cleanup the save dir after restore */
        if (virFileDeleteTree(managed_save_path) < 0) {
            virReportSystemError(errno,
                                 _("Failed to remove managed save path '%1$s'"),
                                 managed_save_path);
            goto endjob;
        }
        vm->hasManagedSave = false;
        ret = 0;
    } else {
        ret = virCHProcessStart(driver, vm, VIR_DOMAIN_RUNNING_BOOTED);
    }

    if (ret == 0) {
        virObjectEvent *event;

        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_STARTED,
                                                  VIR_DOMAIN_EVENT_STARTED_BOOTED);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
        DBG("Failed to save status on vm %s", vm->def->name);
    }

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainGetJobInfo(virDomainPtr domain, virDomainJobInfoPtr info)
{
    virDomainObj *vm;
    int ret = -1;
    unsigned long long timeElapsed = 0;

    memset(info, 0, sizeof(*info));

    if (!(vm = virCHDomainObjFromDomain(domain)))
        goto cleanup;

    if (!vm->job->active) {
        info->type = VIR_DOMAIN_JOB_NONE;
        ret = 0;
        goto cleanup;
    }

    if (virCHDomainJobGetTimeElapsed(vm->job, &timeElapsed) < 0)
        goto cleanup;

    info->type = VIR_DOMAIN_JOB_UNBOUNDED;
    info->timeElapsed = timeElapsed;
    ret = 0;

cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainGetJobStats(virDomainPtr dom,
                    int *type,
                    virTypedParameterPtr *params,
                    int *nparams,
                    unsigned int flags)
{
    virDomainObj *vm;
    int ret = -1;
    int maxparams = 0;
    unsigned long long timeElapsed = 0;

    /* VIR_DOMAIN_JOB_STATS_COMPLETED not supported yet */
    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (!vm->job->active) {
        *type = VIR_DOMAIN_JOB_NONE;
        *params = NULL;
        *nparams = 0;
        ret = 0;
        goto cleanup;
    }

    /* In libxl we don't have an estimated completion time
     * thus we always set to unbounded and update time
     * for the active job. */
    if (virCHDomainJobGetTimeElapsed(vm->job, &timeElapsed) < 0)
        goto cleanup;

    if (virTypedParamsAddULLong(params, nparams, &maxparams,
                                VIR_DOMAIN_JOB_TIME_ELAPSED,
                                timeElapsed) < 0)
        goto cleanup;

    *type = VIR_DOMAIN_JOB_UNBOUNDED;
    ret = 0;

    cleanup:
       virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainCreate(virDomainPtr dom)
{
    return chDomainCreateWithFlags(dom, 0);
}

static virDomainPtr
chDomainDefineXMLFlags(virConnectPtr conn, const char *xml, unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virDomainDef) vmdef = NULL;
    g_autoptr(virDomainDef) oldDef = NULL;
    virDomainObj *vm = NULL;
    virDomainPtr dom = NULL;
    virObjectEvent *event = NULL;
    g_autofree char *managed_save_path = NULL;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;

    virCheckFlags(VIR_DOMAIN_DEFINE_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_START_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;

    /* Avoid parsing the whole domain definition for ACL checks */
    if (!(vmdef = virDomainDefIDsParseString(xml, driver->xmlopt, parse_flags)))
        return NULL;

    if (virDomainDefineXMLFlagsEnsureACL(conn, vmdef) < 0)
        return NULL;

    g_clear_pointer(&vmdef, virDomainDefFree);

    if ((vmdef = virDomainDefParseString(xml, driver->xmlopt,
                                         NULL, parse_flags)) == NULL)
        goto cleanup;

    if (virXMLCheckIllegalChars("name", vmdef->name, "\n") < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains, &vmdef,
                                   driver->xmlopt,
                                   0, &oldDef)))
        goto cleanup;

    if (virDomainDefSave(vm->newDef ? vm->newDef : vm->def,
                         driver->xmlopt, cfg->configDir) < 0)
        goto cleanup;

    /* cleanup if there's any stale managedsave dir */
    managed_save_path = chDomainManagedSavePath(driver, vm);

    /* Do not delete the saved states if there was a previous definition of that
     * domain. In that case, the saved state is probably to be reused. */
    if (!oldDef && virFileDeleteTree(managed_save_path) < 0) {
        virReportSystemError(errno,
                             _("Failed to cleanup stale managed save dir '%1$s'"),
                             managed_save_path);
        goto cleanup;
    }

    vm->persistent = 1;
    event = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_DEFINED,
                                              !oldDef ?
                                              VIR_DOMAIN_EVENT_DEFINED_ADDED :
                                              VIR_DOMAIN_EVENT_DEFINED_UPDATED);
    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);

    return dom;
}

static virDomainPtr
chDomainDefineXML(virConnectPtr conn, const char *xml)
{
    return chDomainDefineXMLFlags(conn, xml, 0);
}

static int
chDomainUndefineFlags(virDomainPtr dom,
                      unsigned int)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virObjectEvent *event = NULL;
    int ret = -1;
    g_autoptr(virCHDriverConfig) cfg = NULL;

    cfg = virCHDriverGetConfig(driver);

    // causes errors with openstack
    // virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainUndefineFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("Cannot undefine transient domain"));
        goto cleanup;
    }

    if (virDomainDeleteConfig(cfg->configDir, cfg->autostartDir, vm) < 0)
        goto endjob;

    event = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_UNDEFINED,
                                              VIR_DOMAIN_EVENT_UNDEFINED_REMOVED);

    DBG("Undefining domain '%s'", vm->def->name);

    vm->persistent = 0;
    if (!virDomainObjIsActive(vm)) {
        virCHDomainRemoveInactive(driver, vm);
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);

    return ret;
}

static int
chDomainUndefine(virDomainPtr dom)
{
    return chDomainUndefineFlags(dom, 0);
}

static int chDomainIsActive(virDomainPtr dom)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainIsActiveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = virDomainObjIsActive(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int chDomainIsPersistent(virDomainPtr dom)
{
    virDomainObj *obj;
    int ret = -1;

    if (!(obj = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainIsPersistentEnsureACL(dom->conn, obj->def) < 0)
        goto cleanup;

    ret = obj->persistent;

 cleanup:
    virDomainObjEndAPI(&obj);
    return ret;
}


static int
chDomainShutdownFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virDomainObj *vm;
    virDomainState state;
    int ret = -1;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    virCHDriver *driver = dom->conn->privateData;

    cfg = virCHDriverGetConfig(driver);

    DBG("chDomainShutdown");
    virCheckFlags(VIR_DOMAIN_SHUTDOWN_ACPI_POWER_BTN, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainShutdownFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    state = virDomainObjGetState(vm, NULL);
    if (state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("only can shutdown running/paused domain"));
        goto endjob;
    }

    if (virCHProcessKill(driver, vm, VIR_DOMAIN_SHUTOFF_SHUTDOWN) < 0)
        goto endjob;

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainShutdown(virDomainPtr dom)
{
    return chDomainShutdownFlags(dom, 0);
}


static int
chDomainReboot(virDomainPtr dom, unsigned int flags)
{
    virCHDomainObjPrivate *priv;
    virDomainObj *vm;
    virDomainState state;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_REBOOT_ACPI_POWER_BTN, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainRebootEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    state = virDomainObjGetState(vm, NULL);
    if (state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("only can reboot running/paused domain"));
        goto endjob;
    } else {
        if (virCHMonitorRebootVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to reboot domain"));
            goto endjob;
        }
    }

    if (state == VIR_DOMAIN_RUNNING)
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_BOOTED);
    else
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_UNPAUSED);
    if (virDomainObjSave(vm, priv->driver->xmlopt, virCHDriverGetConfig(priv->driver)->stateDir) < 0) {
        DBG("Failed to save status on vm %s", vm->def->name);
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainSuspend(virDomainPtr dom)
{
    virCHDomainObjPrivate *priv;
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainSuspendEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("only can suspend running domain"));
        goto endjob;
    } else {
        if (virCHMonitorSuspendVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to suspend domain"));
            goto endjob;
        }
    }

    virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_USER);
    if (virDomainObjSave(vm, priv->driver->xmlopt, virCHDriverGetConfig(priv->driver)->stateDir) < 0) {
        DBG("Failed to save status on vm %s", vm->def->name);
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainResume(virDomainPtr dom)
{
    virCHDomainObjPrivate *priv;
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainResumeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("only can resume paused domain"));
        goto endjob;
    } else {
        if (virCHMonitorResumeVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to resume domain"));
            goto endjob;
        }
    }

    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_UNPAUSED);
    if (virDomainObjSave(vm, priv->driver->xmlopt, virCHDriverGetConfig(priv->driver)->stateDir) < 0) {
        DBG("Failed to save status on vm %s", vm->def->name);
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

/**
 * chDomainDestroyFlags:
 * @dom: pointer to domain to destroy
 * @flags: extra flags; not used yet.
 *
 * Sends SIGKILL to Cloud-Hypervisor process to terminate it
 *
 * Returns 0 on success or -1 in case of error
 */
static int
chDomainDestroyFlags(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virDomainState state;
    virObjectEvent *event = NULL;
    virCHDomainObjPrivate *priv = NULL;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainDestroyFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_DESTROY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    // FIXME: we currently have to shutdown the VMM here
    // because CHV does not release the network file descriptors
    state = virDomainObjGetState(vm, NULL);
    if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
        if (virCHMonitorShutdownVMM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("failed to shutdown VMM"));
        }
    }

    if (virCHProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_DESTROYED) < 0)
        goto endjob;

    event = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_STOPPED,
                                              VIR_DOMAIN_EVENT_STOPPED_DESTROYED);
    virCHDomainRemoveInactive(driver, vm);
    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);

    return ret;
}

static int
chDomainDestroy(virDomainPtr dom)
{
    return chDomainDestroyFlags(dom, 0);
}

static int
chDomainSaveRestoreAdditionalValidation(virCHDriver *driver,
                                        virDomainDef *vmdef)
{
    /* SAVE and RESTORE are functional only without any host device
     * passthrough configuration */
    if  (vmdef->nhostdevs > 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot save/restore domain with host devices"));
        return -1;
    }

    if (vmdef->nnets > 0) {
        if (!virBitmapIsBitSet(driver->chCaps, CH_RESTORE_WITH_NEW_TAPFDS)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cannot save/restore domain with network devices"));
            return -1;
        }
    }

    return 0;
}

/**
 * chDoDomainSave:
 * @driver: pointer to driver structure
 * @vm: pointer to virtual machine structure. Must be locked before invocation.
 * @to_dir: directory path (CH needs directory input) to save the domain
 * @managed: whether the VM is managed or not
 *
 * Checks if the domain is running or paused, then suspends it and saves it
 * using CH's vmm.snapshot API. CH creates multiple files for config, memory,
 * device state into @to_dir.
 *
 * Returns 0 on success or -1 in case of error
 */
static int
chDoDomainSave(virCHDriver *driver,
               virDomainObj *vm,
               const char *to_dir,
               bool managed)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    virCHDomainObjPrivate *priv = vm->privateData;
    CHSaveXMLHeader hdr;
    virDomainState domainState;
    g_autofree char *to = NULL;
    g_autofree char *xml = NULL;
    uint32_t xml_len;
    VIR_AUTOCLOSE fd = -1;
    int ret = -1;

    if (chDomainSaveRestoreAdditionalValidation(driver, vm->def) < 0)
        goto end;

    domainState = virDomainObjGetState(vm, NULL);
    if (domainState == VIR_DOMAIN_RUNNING) {
        if (virCHMonitorSuspendVM(priv->monitor) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to suspend domain before saving"));
            goto end;
        }
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_SAVE);
    } else if (domainState != VIR_DOMAIN_PAUSED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("only can save running/paused domain"));
        goto end;
    }

    if (virDirCreate(to_dir, 0770, cfg->user, cfg->group,
                     VIR_DIR_CREATE_ALLOW_EXIST) < 0) {
        virReportSystemError(errno, _("Failed to create SAVE dir %1$s"), to_dir);
        goto end;
    }

    to = g_strdup_printf("%s/%s", to_dir, CH_SAVE_XML);
    if ((fd = virFileOpenAs(to, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR,
                            cfg->user, cfg->group, 0)) < 0) {
        virReportSystemError(-fd,
                             _("Failed to create/open domain save xml file '%1$s'"),
                             to);
        goto end;
    }

    if ((xml = virDomainDefFormat(vm->def, driver->xmlopt, 0)) == NULL)
        goto end;
    xml_len = strlen(xml) + 1;

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, CH_SAVE_MAGIC, sizeof(hdr.magic));
    hdr.xmlLen = xml_len;

    if (safewrite(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        virReportSystemError(errno, "%s", _("Failed to write file header"));
        goto end;
    }

    if (safewrite(fd, xml, xml_len) != xml_len) {
        virReportSystemError(errno, "%s", _("Failed to write xml definition"));
        goto end;
    }

    if (virCHMonitorSaveVM(priv->monitor, to_dir) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Failed to save domain"));
        goto end;
    }

    if (virCHProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_SAVED) < 0 ) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("failed to stop CH process"));
    }

    vm->hasManagedSave = managed;
    ret = 0;

 end:
    return ret;
}

static int
chDomainSaveFlags(virDomainPtr dom, const char *to, const char *dxml, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);
    if (dxml) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("xml modification unsupported"));
        return -1;
    }

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainSaveFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (chDoDomainSave(driver, vm, to, false) < 0)
        goto endjob;

    /* Remove if VM is not persistent */
    virCHDomainRemoveInactive(driver, vm);
    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainSave(virDomainPtr dom, const char *to)
{
    return chDomainSaveFlags(dom, to, NULL, 0);
}

static char *
chDomainSaveXMLRead(int fd)
{
    g_autofree char *xml = NULL;
    CHSaveXMLHeader hdr;

    if (saferead(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("failed to read CHSaveXMLHeader header"));
        return NULL;
    }

    if (memcmp(hdr.magic, CH_SAVE_MAGIC, sizeof(hdr.magic))) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("save image magic is incorrect"));
        return NULL;
    }

    if (hdr.xmlLen <= 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("invalid XML length: %1$d"), hdr.xmlLen);
        return NULL;
    }

    xml = g_new0(char, hdr.xmlLen);

    if (saferead(fd, xml, hdr.xmlLen) != hdr.xmlLen) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("failed to read XML"));
        return NULL;
    }

    return g_steal_pointer(&xml);
}

static int chDomainSaveImageRead(virConnectPtr conn,
                                 const char *path,
                                 virDomainDef **ret_def,
                                 unsigned int flags,
                                 int (*ensureACL)(virConnectPtr, virDomainDef *),
                                 int (*ensureACLWithFlags)(virConnectPtr,
                                                           virDomainDef *,
                                                           unsigned int))
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    g_autoptr(virDomainDef) def = NULL;
    g_autofree char *from = NULL;
    g_autofree char *xml = NULL;
    VIR_AUTOCLOSE fd = -1;
    int ret = -1;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE |
                               VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE;

    from = g_strdup_printf("%s/%s", path, CH_SAVE_XML);
    if ((fd = virFileOpenAs(from, O_RDONLY, 0, cfg->user, cfg->group, 0)) < 0) {
        virReportSystemError(errno,
                             _("Failed to open domain save file '%1$s'"),
                             from);
        goto end;
    }

    if (!(xml = chDomainSaveXMLRead(fd)))
        goto end;

    if (ensureACL || ensureACLWithFlags) {
        /* Parse only the IDs for ACL checks */
        g_autoptr(virDomainDef) aclDef = virDomainDefIDsParseString(xml,
                                                                    driver->xmlopt,
                                                                    parse_flags);

        if (!aclDef)
            goto end;

        if (ensureACL && ensureACL(conn, aclDef) < 0)
            goto end;

        if (ensureACLWithFlags && ensureACLWithFlags(conn, aclDef, flags) < 0)
            goto end;
    }

    if (!(def = virDomainDefParseString(xml, driver->xmlopt, NULL, parse_flags)))
        goto end;

    *ret_def = g_steal_pointer(&def);
    ret = 0;

 end:
    return ret;
}

static char *
chDomainSaveImageGetXMLDesc(virConnectPtr conn,
                            const char *path,
                            unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    g_autoptr(virDomainDef) def = NULL;
    char *ret = NULL;

    virCheckFlags(VIR_DOMAIN_SAVE_IMAGE_XML_SECURE, NULL);

    if (chDomainSaveImageRead(conn, path, &def, flags,
                              virDomainSaveImageGetXMLDescEnsureACL,
                              NULL) < 0)
        goto cleanup;

    ret = virDomainDefFormat(def, driver->xmlopt,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    return ret;
}

static int
chDomainManagedSave(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    g_autofree char *to = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainManagedSaveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot do managed save for transient domain"));
        goto endjob;
    }

    to = chDomainManagedSavePath(driver, vm);
    if (chDoDomainSave(driver, vm, to, true) < 0)
        goto endjob;

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainManagedSaveRemove(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    int ret = -1;
    g_autofree char *path = NULL;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainManagedSaveRemoveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    path = chDomainManagedSavePath(driver, vm);

    if (virFileDeleteTree(path) < 0) {
        virReportSystemError(errno,
                             _("Failed to remove managed save path '%1$s'"),
                             path);
        goto cleanup;
    }

    vm->hasManagedSave = false;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static char *
chDomainManagedSaveGetXMLDesc(virDomainPtr dom, unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    g_autoptr(virDomainDef) def = NULL;
    char *ret = NULL;
    g_autofree char *path = NULL;

    virCheckFlags(VIR_DOMAIN_SAVE_IMAGE_XML_SECURE, NULL);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    path = chDomainManagedSavePath(driver, vm);
    if (chDomainSaveImageRead(dom->conn, path, &def, flags,
                              NULL,
                              virDomainManagedSaveGetXMLDescEnsureACL) < 0)
        goto cleanup;

    ret = virDomainDefFormat(def, driver->xmlopt,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainHasManagedSaveImage(virDomainPtr dom, unsigned int flags)
{
    virDomainObj *vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainHasManagedSaveImageEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = vm->hasManagedSave;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainRestoreFlags(virConnectPtr conn,
                     const char *from,
                     const char *dxml,
                     unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm = NULL;
    virCHDomainObjPrivate *priv;
    g_autoptr(virDomainDef) def = NULL;
    int ret = -1;
    g_autoptr(virCHDriverConfig) cfg = NULL;

    cfg = virCHDriverGetConfig(driver);

    virCheckFlags(0, -1);

    if (dxml) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("xml modification unsupported"));
        return -1;
    }

    if (chDomainSaveImageRead(conn, from, &def, flags,
                              virDomainRestoreFlagsEnsureACL,
                              NULL) < 0)
        goto cleanup;

    if (chDomainSaveRestoreAdditionalValidation(driver, def) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains, &def,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                   VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virCHProcessStartRestore(driver, vm, from) < 0)
        goto endjob;

    priv = vm->privateData;
    if (virCHMonitorResumeVM(priv->monitor) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to resume domain after restore"));
        goto endjob;
    }
    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_RESTORED);

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
        DBG("Failed to save status on vm %s", vm->def->name);
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    if (vm && ret < 0)
        virCHDomainRemoveInactive(driver, vm);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainRestore(virConnectPtr conn, const char *from)
{
    return chDomainRestoreFlags(conn, from, NULL, 0);
}

static virDomainPtr chDomainLookupByID(virConnectPtr conn,
                                       int id)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByID(driver->domains, id);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching id '%1$d'"), id);
        goto cleanup;
    }

    if (virDomainLookupByIDEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr chDomainLookupByName(virConnectPtr conn,
                                         const char *name)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByName(driver->domains, name);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%1$s'"), name);
        goto cleanup;
    }

    if (virDomainLookupByNameEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr chDomainLookupByUUID(virConnectPtr conn,
                                         const unsigned char *uuid)
{
    virCHDriver *driver = conn->privateData;
    virDomainObj *vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByUUID(driver->domains, uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%1$s'"), uuidstr);
        goto cleanup;
    }

    if (virDomainLookupByUUIDEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static int
chDomainGetState(virDomainPtr dom,
                 int *state,
                 int *reason,
                 unsigned int flags)
{
    virDomainObj *vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetStateEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    *state = virDomainObjGetState(vm, reason);
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static char *chDomainGetXMLDesc(virDomainPtr dom,
                                unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virDomainDef *def;
    char *ret = NULL;

    virCheckFlags(VIR_DOMAIN_XML_COMMON_FLAGS, NULL);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetXMLDescEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if ((flags & VIR_DOMAIN_XML_INACTIVE) && vm->newDef) {
        def = vm->newDef;
    } else {
        def = vm->def;
    }

    ret = virDomainDefFormat(def, driver->xmlopt,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int chDomainGetInfo(virDomainPtr dom,
                           virDomainInfoPtr info)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    info->state = virDomainObjGetState(vm, NULL);

    info->cpuTime = 0;

    info->maxMem = virDomainDefGetMemoryTotal(vm->def);
    info->memory = vm->def->mem.cur_balloon;
    info->nrVirtCpu = virDomainDefGetVcpus(vm->def);

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainOpenConsole(virDomainPtr dom,
                    const char *dev_name,
                    virStreamPtr st,
                    unsigned int flags)
{
     virDomainObj *vm = NULL;
     int ret = -1;
     size_t i;
     virDomainChrDef *chr = NULL;
     virCHDomainObjPrivate *priv;

     virCheckFlags(VIR_DOMAIN_CONSOLE_SAFE | VIR_DOMAIN_CONSOLE_FORCE, -1);

     if (!(vm = virCHDomainObjFromDomain(dom)))
          goto cleanup;

     if (virDomainOpenConsoleEnsureACL(dom->conn, vm->def) < 0)
          goto cleanup;

     if (virDomainObjCheckActive(vm) < 0)
          goto cleanup;

     priv = vm->privateData;

     if (dev_name) {
          for (i = 0; !chr && i < vm->def->nconsoles; i++) {
               if (vm->def->consoles[i]->info.alias &&
                   STREQ(dev_name, vm->def->consoles[i]->info.alias))
                    chr = vm->def->consoles[i];
          }
          for (i = 0; !chr && i < vm->def->nserials; i++) {
               if (STREQ(dev_name, vm->def->serials[i]->info.alias))
                    chr = vm->def->serials[i];
          }
     } else {
          if (vm->def->nconsoles &&
              vm->def->consoles[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)
               chr = vm->def->consoles[0];
          else if (vm->def->nserials &&
                   vm->def->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)
               chr = vm->def->serials[0];
     }

     if (!chr) {
          virReportError(VIR_ERR_INTERNAL_ERROR, _("cannot find character device %1$s"),
                         NULLSTR(dev_name));
          goto cleanup;
     }

     if (chr->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
          virReportError(VIR_ERR_INTERNAL_ERROR,
                         _("character device %1$s is not using a PTY"),
                         dev_name ? dev_name : NULLSTR(chr->info.alias));
          goto cleanup;
     }

     /* handle mutually exclusive access to console devices */
     ret = virChrdevOpen(priv->chrdevs, chr->source, st,
                         (flags & VIR_DOMAIN_CONSOLE_FORCE) != 0);

     if (ret == 1) {
          virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                         _("Active console session exists for this domain"));
          ret = -1;
     }

 cleanup:
     virDomainObjEndAPI(&vm);
     return ret;
}

static int chStateCleanup(void)
{
    if (ch_driver == NULL)
        return -1;

    virBitmapFree(ch_driver->chCaps);
    virObjectUnref(ch_driver->config);
    virObjectUnref(ch_driver->xmlopt);
    virObjectUnref(ch_driver->caps);
    virObjectUnref(ch_driver->domains);
    virObjectUnref(ch_driver->hostdevMgr);
    virObjectUnref(ch_driver->domainEventState);
    virMutexDestroy(&ch_driver->lock);
    g_clear_pointer(&ch_driver, g_free);

    return 0;
}

static int
chDomainReattach(virDomainObj *vm, void*data) {
    virCHDriver *driver = data;
    virCHDomainObjPrivate *priv = vm->privateData;
    // virCHMonitor *mon = priv->monitor;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(ch_driver);
    virDomainState state = virDomainObjGetState(vm, NULL);

    DBG("Reattach to domain: %s", vm->def->name);

    if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
        priv->monitor = virCHMonitorReattach(vm, cfg, driver);
    }

    return 0;
}

static void
processMonitorEOFEvent(virCHDriver *driver,
                       virDomainObj *vm,
                       int domid)
{
    int eventReason = VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN;
    int stopReason = VIR_DOMAIN_SHUTOFF_SHUTDOWN;
    virObjectEvent *event = NULL;

    DBG("Received Monitor EOF. The domain might crashed.");

    if (vm->def->id != domid) {
        VIR_DEBUG("Domain %s was restarted, ignoring EOF",
                  vm->def->name);
        return;
    }

    if (virDomainObjBeginJob(vm, VIR_JOB_DESTROY) < 0)
        return;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain %p '%s' is not active, ignoring EOF",
                  vm, vm->def->name);
        goto endjob;
    }

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_SHUTDOWN) {
        VIR_DEBUG("Monitor connection to '%s' closed without SHUTDOWN event; "
                  "assuming the domain crashed", vm->def->name);
        eventReason = VIR_DOMAIN_EVENT_STOPPED_FAILED;
        stopReason = VIR_DOMAIN_SHUTOFF_CRASHED;
    }

    /* TODO: Handle pending migrations */

    event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                              eventReason);
    virCHProcessStop(driver, vm, stopReason);
    virObjectEventStateQueue(driver->domainEventState, event);

 endjob:
    virDomainObjEndJob(vm);
}

static void chProcessEventHandler(void *data, void *opaque)
{
    struct chProcessEvent *processEvent = data;
    virDomainObj *vm = processEvent->vm;
    virCHDriver *driver = opaque;

    virObjectLock(vm);
    switch (processEvent->eventType) {
    case CH_PROCESS_EVENT_MONITOR_EOF:
        processMonitorEOFEvent(driver, vm, GPOINTER_TO_INT(processEvent->data));
        break;
    case CH_PROCESS_EVENT_LAST:
        break;
    }
    virDomainObjEndAPI(&vm);
    g_free(processEvent);
}

static virDrvStateInitResult
chStateInitialize(bool privileged,
                  const char *root,
                  bool monolithic G_GNUC_UNUSED,
                  virStateInhibitCallback callback G_GNUC_UNUSED,
                  void *opaque G_GNUC_UNUSED)
{
    int ret = VIR_DRV_STATE_INIT_ERROR;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    g_autoptr(virIdentity) identity = virIdentityGetCurrent();
    int rv;

    if (root != NULL) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Driver does not support embedded mode"));
        return -1;
    }

    ch_driver = g_new0(virCHDriver, 1);

    if (virMutexInit(&ch_driver->lock) < 0) {
        g_free(ch_driver);
        return VIR_DRV_STATE_INIT_ERROR;
    }

    if (!(ch_driver->domains = virDomainObjListNew()))
        goto cleanup;

    if (!(ch_driver->caps = virCHDriverCapsInit()))
        goto cleanup;

    if (!virCapabilitiesDomainSupported(ch_driver->caps, -1,
                                        VIR_ARCH_NONE, VIR_DOMAIN_VIRT_KVM, false) &&
        !virCapabilitiesDomainSupported(ch_driver->caps, -1,
                                        VIR_ARCH_NONE, VIR_DOMAIN_VIRT_HYPERV, false)) {
        VIR_INFO("/dev/kvm and /dev/mshv are missing. CH driver failed to initialize.");
        return VIR_DRV_STATE_INIT_SKIPPED;
    }

    if (!(ch_driver->xmlopt = chDomainXMLConfInit(ch_driver)))
        goto cleanup;

    if (!(ch_driver->config = virCHDriverConfigNew(privileged)))
        goto cleanup;

    if (!(ch_driver->hostdevMgr = virHostdevManagerGetDefault()))
        goto cleanup;

    if (!(ch_driver->domainEventState = virObjectEventStateNew()))
        goto cleanup;

    ch_driver->inhibitor = virInhibitorNew(
        VIR_INHIBITOR_WHAT_SHUTDOWN,
        _("Libvirt CHV"),
        _("CHV virtual machines are running"),
        VIR_INHIBITOR_MODE_DELAY,
        callback,
        opaque);

        /* Allocate bitmap for migration port reservation */
    if (!(ch_driver->migrationPorts =
          virPortAllocatorRangeNew(_("migration"),
                                   49152,
                                   49216)))
        goto cleanup;

    if ((rv = chExtractVersion(ch_driver)) < 0) {
        if (rv == -2)
            ret = VIR_DRV_STATE_INIT_SKIPPED;
        goto cleanup;
    }

    ch_driver->chCaps = virCHCapsInitCHVersionCaps(ch_driver->version);

    /* Get all the running persistent or transient configs first */
    cfg = virCHDriverGetConfig(ch_driver);
    DBG("Loading old configs. state dir %s config dir: %s", cfg->stateDir, cfg->configDir);
    if (virDomainObjListLoadAllConfigs(ch_driver->domains,
                                       cfg->stateDir,
                                       NULL, true,
                                       ch_driver->xmlopt,
                                       NULL, NULL) < 0)
        goto cleanup;

    /* Then inactive persistent configs */
    if (virDomainObjListLoadAllConfigs(ch_driver->domains,
                                       cfg->configDir,
                                       cfg->autostartDir, false,
                                       ch_driver->xmlopt,
                                       NULL, NULL) < 0)
        goto cleanup;

    ch_driver->privileged = privileged;

    ch_driver->workerPool = virThreadPoolNewFull(0, 1, 0, chProcessEventHandler,
                                                          "ch-event",
                                                          identity,
                                                          ch_driver);

    virDomainObjListForEach(ch_driver->domains,
                            true,
                            chDomainReattach,
                            ch_driver);

    ret = VIR_DRV_STATE_INIT_COMPLETE;

 cleanup:
    if (ret != VIR_DRV_STATE_INIT_COMPLETE)
        chStateCleanup();
    return ret;
}

/* Which features are supported by this driver? */
static int
chConnectSupportsFeature(virConnectPtr conn,
                         int feature)
{
    int supported;

    if (virConnectSupportsFeatureEnsureACL(conn) < 0)
        return -1;

    if (virDriverFeatureIsGlobal(feature, &supported))
        return supported;

    switch ((virDrvFeature) feature) {
        case VIR_DRV_FEATURE_REMOTE:
        case VIR_DRV_FEATURE_PROGRAM_KEEPALIVE:
        case VIR_DRV_FEATURE_REMOTE_CLOSE_CALLBACK:
        case VIR_DRV_FEATURE_REMOTE_EVENT_CALLBACK:
        case VIR_DRV_FEATURE_TYPED_PARAM_STRING:
        case VIR_DRV_FEATURE_NETWORK_UPDATE_HAS_CORRECT_ORDER:
        case VIR_DRV_FEATURE_FD_PASSING:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Global feature %1$d should have already been handled"),
                           feature);
            return -1;
        case VIR_DRV_FEATURE_MIGRATION_V3:
        case VIR_DRV_FEATURE_MIGRATION_P2P:
        case VIR_DRV_FEATURE_MIGRATION_PARAMS:
            return 1;
        case VIR_DRV_FEATURE_MIGRATION_V2:
        case VIR_DRV_FEATURE_MIGRATE_CHANGE_PROTECTION:
        case VIR_DRV_FEATURE_XML_MIGRATABLE:
        case VIR_DRV_FEATURE_MIGRATION_OFFLINE:
        case VIR_DRV_FEATURE_MIGRATION_DIRECT:
        case VIR_DRV_FEATURE_MIGRATION_V1:
        default:
            return 0;
    }
}

static int
chDomainGetVcpusFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virDomainObj *vm;
    virDomainDef *def;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM | VIR_DOMAIN_VCPU_GUEST, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainGetVcpusFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDef(vm, flags)))
        goto cleanup;

    if (flags & VIR_DOMAIN_VCPU_MAXIMUM)
        ret = virDomainDefGetVcpusMax(def);
    else
        ret = virDomainDefGetVcpus(def);


 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainGetMaxVcpus(virDomainPtr dom)
{
    return chDomainGetVcpusFlags(dom,
                                 (VIR_DOMAIN_AFFECT_LIVE |
                                  VIR_DOMAIN_VCPU_MAXIMUM));
}

static int
chDomainGetVcpuPinInfo(virDomain *dom,
                       int ncpumaps,
                       unsigned char *cpumaps,
                       int maplen,
                       unsigned int flags)
{
    virDomainObj *vm = NULL;
    virDomainDef *def;
    bool live;
    int ret = -1;

    g_autoptr(virBitmap) hostcpus = NULL;
    virBitmap *autoCpuset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetVcpuPinInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (!(hostcpus = virHostCPUGetAvailableCPUsBitmap()))
        goto cleanup;

    if (live)
        autoCpuset = CH_DOMAIN_PRIVATE(vm)->autoCpuset;

    ret = virDomainDefGetVcpuPinInfoHelper(def, maplen, ncpumaps, cpumaps,
                                           hostcpus, autoCpuset);
 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chNodeGetCPUMap(virConnectPtr conn,
                unsigned char **cpumap,
                unsigned int *online, unsigned int flags)
{
    if (virNodeGetCPUMapEnsureACL(conn) < 0)
        return -1;

    return virHostCPUGetMap(cpumap, online, flags);
}


static int
chDomainHelperGetVcpus(virDomainObj *vm,
                       virVcpuInfoPtr info,
                       unsigned long long *cpuwait,
                       int maxinfo,
                       unsigned char *cpumaps,
                       int maplen)
{
    size_t ncpuinfo = 0;
    size_t i;

    if (maxinfo == 0)
        return 0;

    if (!virCHDomainHasVcpuPids(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cpu affinity is not supported"));
        return -1;
    }

    if (info)
        memset(info, 0, sizeof(*info) * maxinfo);

    if (cpumaps)
        memset(cpumaps, 0, sizeof(*cpumaps) * maxinfo);

    for (i = 0; i < virDomainDefGetVcpusMax(vm->def) && ncpuinfo < maxinfo; i++) {
        virDomainVcpuDef *vcpu = virDomainDefGetVcpu(vm->def, i);
        pid_t vcpupid = virCHDomainGetVcpuPid(vm, i);
        virVcpuInfoPtr vcpuinfo = info + ncpuinfo;

        if (!vcpu->online)
            continue;

        if (info) {
            vcpuinfo->number = i;
            vcpuinfo->state = VIR_VCPU_RUNNING;
            if (virProcessGetStatInfo(&vcpuinfo->cpuTime,
                                      NULL, NULL,
                                      &vcpuinfo->cpu, NULL,
                                      vm->pid, vcpupid) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("cannot get vCPU placement & pCPU time"));
                return -1;
            }
        }

        if (cpumaps) {
            unsigned char *cpumap = VIR_GET_CPUMAP(cpumaps, maplen, ncpuinfo);
            g_autoptr(virBitmap) map = NULL;

            if (!(map = virProcessGetAffinity(vcpupid)))
                return -1;

            virBitmapToDataBuf(map, cpumap, maplen);
        }

        if (cpuwait) {
            if (virProcessGetSchedInfo(&(cpuwait[ncpuinfo]), vm->pid, vcpupid) < 0)
                return -1;
        }

        ncpuinfo++;
    }

    return ncpuinfo;
}

static int
chDomainGetVcpus(virDomainPtr dom,
                 virVcpuInfoPtr info,
                 int maxinfo,
                 unsigned char *cpumaps,
                 int maplen)
{
    virDomainObj *vm;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetVcpusEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot retrieve vcpu information for inactive domain"));
        goto cleanup;
    }

    ret = chDomainHelperGetVcpus(vm, info, NULL, maxinfo, cpumaps, maplen);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainPinVcpuLive(virDomainObj *vm,
                    virDomainDef *def,
                    int vcpu,
                    virCHDriver *driver,
                    virCHDriverConfig *cfg,
                    virBitmap *cpumap)
{
    g_autoptr(virBitmap) tmpmap = NULL;
    g_autoptr(virCgroup) cgroup_vcpu = NULL;
    virDomainVcpuDef *vcpuinfo;
    virCHDomainObjPrivate *priv = vm->privateData;

    if (!virCHDomainHasVcpuPids(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cpu affinity is not supported"));
        return -1;
    }

    if (!(vcpuinfo = virDomainDefGetVcpu(def, vcpu))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("vcpu %1$d is out of range of live cpu count %2$d"),
                       vcpu, virDomainDefGetVcpusMax(def));
        return -1;
    }

    if (!(tmpmap = virBitmapNewCopy(cpumap)))
        return -1;

    if (vcpuinfo->online) {
        /* Configure the corresponding cpuset cgroup before set affinity. */
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, vcpu,
                                   false, &cgroup_vcpu) < 0)
                return -1;
            if (virDomainCgroupSetupCpusetCpus(cgroup_vcpu, cpumap) < 0)
                return -1;
        }

        if (virProcessSetAffinity(virCHDomainGetVcpuPid(vm, vcpu), cpumap, false) < 0)
            return -1;
    }

    virBitmapFree(vcpuinfo->cpumask);
    vcpuinfo->cpumask = g_steal_pointer(&tmpmap);

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        return -1;

    return 0;
}

static int
chDomainPinVcpuFlags(virDomainPtr dom,
                     unsigned int vcpu,
                     unsigned char *cpumap,
                     int maplen,
                     unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virDomainDef *def;
    virDomainDef *persistentDef;
    int ret = -1;
    g_autoptr(virBitmap) pcpumap = NULL;
    virDomainVcpuDef *vcpuinfo = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virCHDriverGetConfig(driver);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPinVcpuFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (persistentDef &&
        !(vcpuinfo = virDomainDefGetVcpu(persistentDef, vcpu))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("vcpu %1$d is out of range of persistent cpu count %2$d"),
                       vcpu, virDomainDefGetVcpus(persistentDef));
        goto endjob;
    }

    if (!(pcpumap = virBitmapNewData(cpumap, maplen)))
        goto endjob;

    if (virBitmapIsAllClear(pcpumap)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Empty cpu list for pinning"));
        goto endjob;
    }

    if (def &&
        chDomainPinVcpuLive(vm, def, vcpu, driver, cfg, pcpumap) < 0)
        goto endjob;

    if (persistentDef) {
        virBitmapFree(vcpuinfo->cpumask);
        vcpuinfo->cpumask = g_steal_pointer(&pcpumap);
        ret = virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir);
        goto endjob;
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainPinVcpu(virDomainPtr dom,
                unsigned int vcpu,
                unsigned char *cpumap,
                int maplen)
{
    return chDomainPinVcpuFlags(dom, vcpu, cpumap, maplen,
                                VIR_DOMAIN_AFFECT_LIVE);
}



static int
chDomainGetEmulatorPinInfo(virDomainPtr dom,
                           unsigned char *cpumaps,
                           int maplen,
                           unsigned int flags)
{
    virDomainObj *vm = NULL;
    virDomainDef *def;
    virCHDomainObjPrivate *priv;
    bool live;
    int ret = -1;
    virBitmap *cpumask = NULL;
    g_autoptr(virBitmap) bitmap = NULL;
    virBitmap *autoCpuset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetEmulatorPinInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (live) {
        priv = vm->privateData;
        autoCpuset = priv->autoCpuset;
    }
    if (def->cputune.emulatorpin) {
        cpumask = def->cputune.emulatorpin;
    } else if (def->cpumask) {
        cpumask = def->cpumask;
    } else if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO &&
               autoCpuset) {
        cpumask = autoCpuset;
    } else {
        if (!(bitmap = virHostCPUGetAvailableCPUsBitmap()))
            goto cleanup;
        cpumask = bitmap;
    }

    virBitmapToDataBuf(cpumask, cpumaps, maplen);

    ret = 1;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainPinEmulator(virDomainPtr dom,
                    unsigned char *cpumap,
                    int maplen,
                    unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm;
    virDomainDef *def;
    virDomainDef *persistentDef;
    int ret = -1;
    virCHDomainObjPrivate *priv;
    g_autoptr(virBitmap) pcpumap = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxparams = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virCHDriverGetConfig(driver);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPinEmulatorEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    priv = vm->privateData;

    if (!(pcpumap = virBitmapNewData(cpumap, maplen)))
        goto endjob;

    if (virBitmapIsAllClear(pcpumap)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Empty cpu list for pinning"));
        goto endjob;
    }

    if (def) {
        g_autoptr(virCgroup) cgroup_emulator = NULL;
        g_autofree char *str = NULL;

        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_EMULATOR,
                                   0, false, &cgroup_emulator) < 0)
                goto endjob;

            if (virDomainCgroupSetupCpusetCpus(cgroup_emulator, pcpumap) < 0) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                               _("failed to set cpuset.cpus in cgroup for emulator threads"));
                goto endjob;
            }
        }

        if (virProcessSetAffinity(vm->pid, pcpumap, false) < 0)
            goto endjob;

        g_clear_pointer(&def->cputune.emulatorpin, virBitmapFree);

        if (!(def->cputune.emulatorpin = virBitmapNewCopy(pcpumap)))
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;

        str = virBitmapFormat(pcpumap);
        if (virTypedParamsAddString(&eventParams, &eventNparams,
                                    &eventMaxparams,
                                    VIR_DOMAIN_TUNABLE_CPU_EMULATORPIN,
                                    str) < 0)
            goto endjob;
    }

    if (persistentDef) {
        virBitmapFree(persistentDef->cputune.emulatorpin);
        persistentDef->cputune.emulatorpin = virBitmapNewCopy(pcpumap);

        ret = virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir);
        goto endjob;
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

#define CH_NB_NUMA_PARAM 2

static int
chDomainGetNumaParameters(virDomainPtr dom,
                          virTypedParameterPtr params,
                          int *nparams,
                          unsigned int flags)
{
    size_t i;
    virDomainObj *vm = NULL;
    virDomainNumatuneMemMode tmpmode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;
    virCHDomainObjPrivate *priv;
    g_autofree char *nodeset = NULL;
    int ret = -1;
    virDomainDef *def = NULL;
    bool live = false;
    virBitmap *autoNodeset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;
    priv = vm->privateData;

    if (virDomainGetNumaParametersEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (live)
        autoNodeset = priv->autoNodeset;

    if ((*nparams) == 0) {
        *nparams = CH_NB_NUMA_PARAM;
        ret = 0;
        goto cleanup;
    }

    for (i = 0; i < CH_NB_NUMA_PARAM && i < *nparams; i++) {
        virMemoryParameterPtr param = &params[i];

        switch (i) {
        case 0: /* fill numa mode here */
            ignore_value(virDomainNumatuneGetMode(def->numa, -1, &tmpmode));

            if (virTypedParameterAssign(param, VIR_DOMAIN_NUMA_MODE,
                                        VIR_TYPED_PARAM_INT, tmpmode) < 0)
                goto cleanup;

            break;

        case 1: /* fill numa nodeset here */
            nodeset = virDomainNumatuneFormatNodeset(def->numa, autoNodeset, -1);

            if (!nodeset ||
                virTypedParameterAssign(param, VIR_DOMAIN_NUMA_NODESET,
                                        VIR_TYPED_PARAM_STRING, nodeset) < 0)
                goto cleanup;

            nodeset = NULL;
            break;

        default:
            break;
            /* should not hit here */
        }
    }

    if (*nparams > CH_NB_NUMA_PARAM)
        *nparams = CH_NB_NUMA_PARAM;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chDomainSetNumaParamsLive(virDomainObj *vm,
                          virBitmap *nodeset)
{
    g_autoptr(virCgroup) cgroup_temp = NULL;
    virCHDomainObjPrivate *priv = vm->privateData;
    g_autofree char *nodeset_str = NULL;
    virDomainNumatuneMemMode mode;
    size_t i = 0;

    if (virDomainNumatuneGetMode(vm->def->numa, -1, &mode) == 0 &&
        mode != VIR_DOMAIN_NUMATUNE_MEM_RESTRICTIVE) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("change of nodeset for running domain requires strict numa mode"));
        return -1;
    }

    if (!virNumaNodesetIsAvailable(nodeset))
        return -1;

    /* Ensure the cpuset string is formatted before passing to cgroup */
    nodeset_str = virBitmapFormat(nodeset);

    if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_EMULATOR, 0,
                           false, &cgroup_temp) < 0 ||
        virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
        return -1;


    for (i = 0; i < virDomainDefGetVcpusMax(vm->def); i++) {
        virDomainVcpuDef *vcpu = virDomainDefGetVcpu(vm->def, i);

        if (!vcpu->online)
            continue;

        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, i,
                               false, &cgroup_temp) < 0 ||
            virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
            return -1;
    }

    for (i = 0; i < vm->def->niothreadids; i++) {
        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                               vm->def->iothreadids[i]->iothread_id,
                               false, &cgroup_temp) < 0 ||
            virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
            return -1;
    }

    /* set nodeset for root cgroup */
    if (virCgroupSetCpusetMems(priv->cgroup, nodeset_str) < 0)
        return -1;

    return 0;
}

static int
chDomainSetNumaParameters(virDomainPtr dom,
                          virTypedParameterPtr params,
                          int nparams,
                          unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    size_t i;
    virDomainDef *def;
    virDomainDef *persistentDef;
    virDomainObj *vm = NULL;
    int ret = -1;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    virCHDomainObjPrivate *priv;
    g_autoptr(virBitmap) nodeset = NULL;
    virDomainNumatuneMemMode config_mode;
    int mode = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_NUMA_MODE,
                               VIR_TYPED_PARAM_INT,
                               VIR_DOMAIN_NUMA_NODESET,
                               VIR_TYPED_PARAM_STRING,
                               NULL) < 0)
        return -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainSetNumaParametersEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    priv = vm->privateData;
    cfg = virCHDriverGetConfig(driver);

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STREQ(param->field, VIR_DOMAIN_NUMA_MODE)) {
            mode = param->value.i;

            if (mode < 0 || mode >= VIR_DOMAIN_NUMATUNE_MEM_LAST) {
                virReportError(VIR_ERR_INVALID_ARG,
                               _("unsupported numatune mode: '%1$d'"), mode);
                goto cleanup;
            }

        } else if (STREQ(param->field, VIR_DOMAIN_NUMA_NODESET)) {
            if (virBitmapParse(param->value.s, &nodeset,
                               VIR_DOMAIN_CPUMASK_LEN) < 0)
                goto cleanup;

            if (virBitmapIsAllClear(nodeset)) {
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("Invalid nodeset of 'numatune': %1$s"),
                               param->value.s);
                goto cleanup;
            }
        }
    }

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def) {
        if (!driver->privileged) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("NUMA tuning is not available in session mode"));
            goto endjob;
        }

        if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cgroup cpuset controller is not mounted"));
            goto endjob;
        }

        if (mode != -1 &&
            virDomainNumatuneGetMode(def->numa, -1, &config_mode) == 0 &&
            config_mode != mode) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("can't change numatune mode for running domain"));
            goto endjob;
        }

        if (nodeset &&
            chDomainSetNumaParamsLive(vm, nodeset) < 0)
            goto endjob;

        if (virDomainNumatuneSet(def->numa,
                                 def->placement_mode ==
                                 VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC,
                                 -1, mode, nodeset) < 0)
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentDef) {
        if (virDomainNumatuneSet(persistentDef->numa,
                                 persistentDef->placement_mode ==
                                 VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC,
                                 -1, mode, nodeset) < 0)
            goto endjob;

        if (virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir) < 0)
            goto endjob;
    }

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chNodeGetMemoryStats(virConnectPtr conn,
                     int cellNum,
                     virNodeMemoryStatsPtr params,
                     int *nparams,
                     unsigned int flags)
{
    if (virNodeGetMemoryStatsEnsureACL(conn) < 0)
        return -1;

    return virHostMemGetStats(cellNum, params, nparams, flags);
}

static int
chConnectDomainEventRegisterAny(virConnectPtr conn,
                                virDomainPtr dom,
                                int eventID,
                                virConnectDomainEventGenericCallback callback,
                                void *opaque,
                                virFreeCallback freecb)
{
    virCHDriver *driver = conn->privateData;
    int ret = -1;

    if (virConnectDomainEventRegisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virDomainEventStateRegisterID(conn,
                                      driver->domainEventState,
                                      dom, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        ret = -1;

    return ret;
}


static int
chConnectDomainEventDeregisterAny(virConnectPtr conn,
                                  int callbackID)
{
    virCHDriver *driver = conn->privateData;

    if (virConnectDomainEventDeregisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virObjectEventStateDeregisterID(conn,
                                        driver->domainEventState,
                                        callbackID, true) < 0)
        return -1;

    return 0;
}

static int
chDomainInterfaceAddresses(virDomain *dom,
                           virDomainInterfacePtr **ifaces,
                           unsigned int source,
                           unsigned int flags)
{
    virDomainObj *vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainInterfaceAddressesEnsureACL(dom->conn, vm->def, source) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    switch (source) {
    case VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE:
        ret = virDomainNetDHCPInterfaces(vm->def, ifaces);
        break;

    case VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP:
        ret = virDomainNetARPInterfaces(vm->def, ifaces);
        break;

    default:
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                       _("Unsupported IP address data source %1$d"),
                       source);
        break;
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

/*******************************************************************
 * Migration Protocol Version 3
 *******************************************************************/

static char *
chDomainMigrateBegin3(virDomainPtr domain,
                      const char *xmlin,
                      char **cookieout,
                      int *cookieoutlen,
                      unsigned long flags,
                      const char *dname,
                      unsigned long resource G_GNUC_UNUSED)
{
    virDomainObj *vm;
    char *xml = NULL;
    virCHDriver *driver = domain->conn->privateData;

    VIR_INFO("chDomainMigrateBegin3 %p %s %p %p %lu %s",
              domain, xmlin, cookieout, cookieoutlen, flags, dname);
    if (!(vm = virCHDomainObjFromDomain(domain)))
        return NULL;

    if (virDomainMigrateBegin3EnsureACL(domain->conn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return NULL;
    }

    // Copied from libxl_migration.c:386
    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    xml = virDomainDefFormat(vm->def, driver->xmlopt, VIR_DOMAIN_DEF_FORMAT_SECURE);

    if (xml) {
        VIR_INFO("chDomainMigrateBegin3 success. xml: %s", xml);
        goto cleanup;
    }

    return NULL;
 cleanup:
    virDomainObjEndAPI(&vm);
    return xml;
}

static char *
chDomainMigrateBegin3Params(virDomainPtr domain,
                              virTypedParameterPtr params,
                              int nparams,
                              char **cookieout,
                              int *cookieoutlen,
                              unsigned int flags)
{
    const char *xmlin = NULL;
    const char *dname = NULL;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_XML,
                                &xmlin) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0)
        return NULL;

    return chDomainMigrateBegin3(domain, xmlin, cookieout, cookieoutlen, flags, dname, 0);
}

static
virDomainDef *
chMigrationAnyPrepareDef(virCHDriver *driver,
                           const char *dom_xml,
                           const char *dname)
{
    virDomainDef *def;
    char *name = NULL;

    if (!dom_xml) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("no domain XML passed"));
        return NULL;
    }

    if (!(def = virDomainDefParseString(dom_xml, driver->xmlopt,
                                        NULL,
                                        VIR_DOMAIN_DEF_PARSE_INACTIVE)))
        goto cleanup;

    if (dname) {
        VIR_FREE(name);
        def->name = g_strdup(dname);
    }

 cleanup:
    return def;
}

static void
chDoMigrateDstReceive(void *opaque)
{
    chMigrationDstArgs *args = opaque;
    virCHDomainObjPrivate *priv = args->priv;
    g_autofree char* rcv_uri = NULL;

    DBG("Migration thread executing");
    if (!priv->monitor) {
        VIR_ERROR(_("VMs monitor not initialized"));
    }

    rcv_uri = g_strdup_printf("tcp:0.0.0.0:%d", args->port);

    if (virCHMonitorMigrationReceive(priv->monitor,
                                     rcv_uri,
                                     args->def,
                                     args->driver,
                                     &args->cond,
                                     args->tcp_serial_url) < 0) {
        DBG("Migration receive failed.");
        args->success = false;
        return;
    }

    DBG("Migration thread finished its duty");
    args->success = true;
}

static virURI *
chMigrationAnyParseURI(const char *uri, bool *wellFormed)
{
    char *tmp = NULL;
    virURI *parsed;

    /* For compatibility reasons tcp://... URIs are sent as tcp:...
     * We need to transform them to a well-formed URI before parsing. */
    if (STRPREFIX(uri, "tcp:") && !STRPREFIX(uri + 4, "//")) {
        tmp = g_strdup_printf("tcp://%s", uri + 4);
        uri = tmp;
    }

    parsed = virURIParse(uri);
    if (parsed && wellFormed)
        *wellFormed = !tmp;
    VIR_FREE(tmp);

    return parsed;
}

static int chMigrationJobStart(virDomainObj *vm,
                               virDomainAsyncJob job)
{
    virDomainJobOperation op;
    unsigned long long mask;
    if (vm->job->asyncJob == VIR_ASYNC_JOB_MIGRATION_IN ||
        vm->job->asyncJob == VIR_ASYNC_JOB_MIGRATION_OUT) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("another migration job is already running for domain '%1$s'"),
                       vm->def->name);
        return -1;
    }

    if (vm->job) {
        // I don't know where this member is set for QEMU, but with CH we ran
        // into a nullptr exception when trying to access some member of
        // `jobDataPrivateCb`.
        // As far as I can see, we do not require setting these callbacks and
        // we can avoid the exception by setting the whole member to NULL.
        // vm->job->jobDataPrivateCb = NULL;
        g_free(vm->job->jobDataPrivateCb);
        vm->job->jobDataPrivateCb = NULL;
    }

    if (job == VIR_ASYNC_JOB_MIGRATION_IN) {
        op = VIR_DOMAIN_JOB_OPERATION_MIGRATION_IN;
        mask = VIR_JOB_NONE;
    } else {
        op = VIR_DOMAIN_JOB_OPERATION_MIGRATION_OUT;
        mask = VIR_JOB_DEFAULT_MASK |
               JOB_MASK(VIR_JOB_MIGRATION_OP);
    }
    mask |= JOB_MASK(VIR_JOB_MODIFY_MIGRATION_SAFE);

    if (virDomainObjBeginAsyncJob(vm, job, op, 0) < 0)
        return -1;

    return 0;
}

/**
 * Runs on the destination and prepares the empty cloud hypervisor process to
 * receive the migration.
 * Allocates some tcp port number to use as a migration channel.
 */
static int
chDomainMigratePrepare3(virConnectPtr dconn,
                        const char *cookiein,
                        int cookieinlen,
                        char **cookieout,
                        int *cookieoutlen,
                        const char *uri_in,
                        char **uri_out,
                        unsigned long flags,
                        const char *dname,
                        unsigned long resource G_GNUC_UNUSED,
                        const char *dom_xml)
{
    virCHDriver *driver = dconn->privateData;
    virDomainObj *vm = NULL;
    virCHDomainObjPrivate *priv = NULL;
    chMigrationDstArgs *args = g_new0(chMigrationDstArgs, 1);
    unsigned short port = 0;
    g_autofree char *server_addr = NULL;
    const char *threadname = "mig-ch";
    virDomainDef *def = NULL;
    g_autoptr(virDomainDef) vmdef = NULL;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    int rc = -1;
    const char *incFormat = "%s:%s:%d"; // seems to differ for AF_INET6

    DBG("%p %s %u %p %p %s %p %lu %s %s",
        dconn, cookiein, cookieinlen, cookieout, cookieoutlen, uri_in, uri_out, flags, dname, dom_xml);

    if (virDomainMigratePrepare3EnsureACL(dconn, def) < 0)
        return -1;

    if (!(def = chMigrationAnyPrepareDef(driver, dom_xml, dname)))
        return -1;

    VIR_INFO("Got DomainDef prepared successfully");

    if (virPortAllocatorAcquire(driver->migrationPorts, &port) < 0) {
        goto cleanup;
    }
    VIR_DEBUG("Got port %i", port);

    // The dname contains the server address (e.g. IP address) that got passed
    // to the libvirt API. This is the correct target address for the
    // migration. If we haven't got one here, we use the hostname instead.
    if (uri_in) {
        server_addr = g_strdup_printf("%s", uri_in);
    } else if ((server_addr = virGetHostname()) == NULL) {
        goto cleanup;
    }

    *uri_out = g_strdup_printf(incFormat, "tcp", server_addr, port);
    VIR_DEBUG("uri out %s", *uri_out);

    if (!(vm = virDomainObjListAdd(driver->domains, &def,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                   VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
    {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add Domain Obj to List"));
        goto cleanup;
    }

    if (chMigrationJobStart(vm, VIR_ASYNC_JOB_MIGRATION_IN) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not begin async migration job"));
        goto cleanup;
    }

    if (virCHProcessInit(driver, vm) < 0) {
        DBG("Could not init process");
        goto cleanup;
    }

    DBG("Try creating migration thread for domain: %s", vm->def->name);

    if (virMutexInit(&args->mutex) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to initialize mutex"));
    }

    if (virCondInit(&args->cond) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to initialize condition variable"));
    }

    priv = vm->privateData;
    priv->args = args;
    args->port = port;
    args->priv = priv;
    args->def = vm->def;
    args->driver = driver;
    args->success = false;
    args->tcp_serial_url = NULL;

    if (vm->def->nserials > 0 &&
        vm->def->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_TCP) {
        args->tcp_serial_url = g_strdup_printf("%s:%s",
                                               vm->def->serials[0]->source->data.tcp.host,
                                               vm->def->serials[0]->source->data.tcp.service);
    }

    // VM receiving is blocking which we cannot do here, because it would block
    // the Libvirt migration protocol.
    // Prepare a thread to receive the migration data
    // VIR_FREE(priv->migrationDstReceiveThr);
    priv->migrationDstReceiveThr = g_new0(virThread, 1);
    if (virThreadCreateFull(priv->migrationDstReceiveThr, true,
                            chDoMigrateDstReceive,
                            threadname,
                            false,
                            args) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Failed to create thread for receiving migration data"));
        goto cleanup;
    }

    DBG("Finished creating migration thread");

    if (virCondWait(&args->cond, &args->mutex) < 0) {
        DBG("CondWait returned failure. Keep going.");
    }

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
        DBG("Failed to save status on vm %s", vm->def->name);
    }

    if (flags & VIR_MIGRATE_PERSIST_DEST) {
        DBG("persist domain on receiving side");
        if (virDomainDefSave(vm->newDef ? vm->newDef : vm->def,
                            driver->xmlopt, cfg->configDir) < 0) {
            DBG("Failed to persist domain on receiving side");
        }
    }

    rc = 0;
    DBG("Fin migrationPrepare");


 cleanup:
    virDomainObjEndAPI(&vm);
    return rc;
}

static int
chDomainMigratePrepare3Params(virConnectPtr dconn,
                                virTypedParameterPtr params,
                                int nparams,
                                const char *cookiein,
                                int cookieinlen,
                                char **cookieout,
                                int *cookieoutlen,
                                char **uri_out,
                                unsigned int flags)
{

    const char *dom_xml = NULL;
    const char *dname = NULL;
    const char *uri_in = NULL;
    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_XML,
                                &dom_xml) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_URI,
                                &uri_in) < 0)
        return -1;

    return chDomainMigratePrepare3(dconn, cookiein, cookieinlen, cookieout, cookieoutlen, uri_in, uri_out, flags, dname, 0, dom_xml);
}

static int
chDomainMigrateConfirm3(virDomainPtr domain,
                        const char *cookiein,
                        int cookieinlen,
                        unsigned long flags,
                        int cancelled)
{
    virCHDriver *driver = domain->conn->privateData;
    virObjectEvent *event = NULL;
    // virObjectEvent *event = NULL;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(domain->conn->privateData);
    // virCHDomainObjPrivate *priv = NULL;
    // size_t i;
    virDomainObj *vm;

    VIR_INFO("chDomainMigrateConfirm3 %p %s %d %lu %d",
              domain, cookiein, cookieinlen, flags, cancelled);

    if (!(vm = virCHDomainObjFromDomain(domain)))
        return -1;

    // priv = vm->privateData;

    if (virDomainMigrateConfirm3EnsureACL(domain->conn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return -1;
    }

    // Following call deletes network devices, cgroups and stops the cloud
    // hypervisor process
    virCHProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_MIGRATED);

    if (flags & VIR_MIGRATE_UNDEFINE_SOURCE) {

        virDomainDeleteConfig(cfg->configDir, cfg->autostartDir, vm);
        vm->persistent = 0;
    }

    virCHDomainRemoveInactive(driver, vm);

    event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                              VIR_DOMAIN_EVENT_STOPPED_MIGRATED);

    virDomainObjEndJob(vm);
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);
    return 0;
}

static int
chDomainMigrateConfirm3Params(virDomainPtr domain,
                                virTypedParameterPtr /*params*/,
                                int /*nparams*/,
                                const char *cookiein,
                                int cookieinlen,
                                unsigned int flags,
                                int cancelled)
{
    return chDomainMigrateConfirm3(domain, cookiein, cookieinlen, flags, cancelled);
}

static int virConnectCredType[] = {
    VIR_CRED_AUTHNAME,
    VIR_CRED_PASSPHRASE,
};


static virConnectAuth virConnectAuthConfig = {
    .credtype = virConnectCredType,
    .ncredtype = G_N_ELEMENTS(virConnectCredType),
};

static int
chDomainMigratePerform3Impl(virDomainObj *vm,
                            virCHDriver *driver,
                            const char *xmlin,
                            const char *dconnuri,
                            const char *uri,
                            const char *cookiein,
                            int cookieinlen,
                            char **cookieout,
                            int *cookieoutlen,
                            unsigned long flags,
                            const char *dname,
                            unsigned parallel_connections)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    g_autofree char *id = NULL;
    g_autoptr(virConnect) dconn = NULL;
    g_autoptr(virURI) uri_parsed = NULL;
    g_autofree char *uri_out = NULL;
    g_autofree char *dom_xml = NULL;
    virDomainPtr ddomain = NULL;
    int rc = -1;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);

    DBG("chDomainMigratePerform3Impl %s %s %s %lu %s %u",
        xmlin, dconnuri, uri, flags, dname, parallel_connections);

    if (!priv->monitor) {
        VIR_ERROR(_("VMs monitor not initialized"));
        goto cleanup;
    }

    if (chMigrationJobStart(vm, VIR_ASYNC_JOB_MIGRATION_OUT) < 0) {
        DBG("Could not begin async migration job");
        return -1;
    }

    /**
     * If the dconnuri is set we are (most likely) in the P2P/direct case. In this
     * case, the libvirt migration protocol (begin, prepare, perform, finish,
     * confirm) is not handled by libvirt, but must be driven by us.
     * Libvirt only calls the perform API call and the rest must be done in this function.
     * We can obtain a connection to the remote libvirt via the
     * `virConnectOpenAuth` call and call all the migration functions in the
     * right order from here.
     */
    if (dconnuri) {
        DBG("Got dconnuri. Probably p2p/direct migration. Do special extra handling");

        /* The caller of the migration is able to specify a domain XML
         * description used for the domain on the destination side. This is
         * required e.g. to adapt some ip or port binding in the TCP serial
         * configuration. */
        if (xmlin) {
            /* dom_xml will be cleaned up by us but the caller expects to cleanup xmlin. */
            dom_xml = g_strdup(xmlin);
        } else {
            dom_xml = virDomainDefFormat(vm->def, driver->xmlopt, VIR_DOMAIN_DEF_FORMAT_SECURE);
        }

        DBG("Got domain xml: %s", dom_xml);

        dconn = virConnectOpenAuth(dconnuri, &virConnectAuthConfig, 0);
        if (dconn == NULL) {
            DBG("Could not open connection to remote libvirt daemon");
            goto cleanup;
        }

        if (!(uri_parsed = chMigrationAnyParseURI(dconnuri, NULL))) {
            DBG("Parse dconnuri failed.");
        }

        dconn->driver->domainMigratePrepare3(dconn, cookiein, cookieinlen, cookieout, cookieoutlen, uri_parsed ? uri_parsed->server : NULL, &uri_out, flags, dname, 0 /*bandwidth*/, dom_xml);

        DBG("Got uri_out that will be used for CHV migration: %s", uri_out);
        uri = uri_out;
    }

    if (virCHMonitorMigrationSend(priv->monitor, uri, parallel_connections) < 0) {
        DBG("Migration send failed.");
        dconn->driver->domainMigrateFinish3(dconn, vm->def->name, NULL, 0, NULL, NULL, NULL, uri, flags, 1);
        rc = -1;
        goto cleanup;
    }

    rc = 0;

    if (dconnuri) {
        DBG("P2P: Call finish on remote context");
        ddomain = dconn->driver->domainMigrateFinish3(dconn, vm->def->name, NULL, 0, NULL, NULL, NULL, uri, flags, 0);
        virObjectUnref(ddomain);

        DBG("P2P: Call confirm on our context");

        // Instead of calling DomainMigrateConfirm3 here, we reimplement the
        // interesting part. We do so, because we have observed some strange
        // locking issues and removing the domain infinitely hang.
        // We probably want to call some function that is also called from
        // chDomainMigrateConfirm3 here which implements the right logic.
        // chDomainMigrateConfirm3(dom, cookiein, cookieinlen, flags, 0);

        virCHProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_MIGRATED);

        if (virDomainDeleteConfig(cfg->stateDir, cfg->autostartDir, vm) < 0) {
            DBG("Failed to delete transient config");
        }
        if (flags & VIR_MIGRATE_UNDEFINE_SOURCE) {
            virDomainDeleteConfig(cfg->configDir, cfg->autostartDir, vm);
            vm->persistent = 0;
        }

        if (!virDomainObjIsActive(vm)) {
            virCHDomainRemoveInactive(driver, vm);
        }

        virDomainObjEndAsyncJob(vm);
        DBG("P2P: Migration finished");
        return 0;
    }

cleanup:
    virDomainObjEndAsyncJob(vm);
    if (dconn) {
        virConnectClose(dconn);
    }

    return rc;
}

static int
chDomainMigratePerform3(virDomainPtr dom,
                        const char *xmlin,
                        const char *cookiein,
                        int cookieinlen,
                        char **cookieout,
                        int *cookieoutlen,
                        const char *dconnuri,
                        const char *uri,
                        unsigned long flags,
                        const char *dname,
                        unsigned long resource)
{
    virDomainObj *vm;
    virCHDriver *driver = dom->conn->privateData;
    int rc = -1;

    VIR_INFO("chDomainMigratePerform3 %p %s %s %u %p %p %s %s %lu %s %lu",
              dom, xmlin, cookiein, cookieinlen, cookieout, cookieoutlen, dconnuri, uri, flags, dname, resource);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainMigratePerform3EnsureACL(dom->conn, vm->def) < 0) {
        goto cleanup;
    }

    rc = chDomainMigratePerform3Impl(vm,
                                     driver,
                                     xmlin,
                                     dconnuri,
                                     uri,
                                     cookiein,
                                     cookieinlen,
                                     cookieout,
                                     cookieoutlen,
                                     flags,
                                     dname,
                                     1);

cleanup:
    virDomainObjEndAPI(&vm);
    return rc;
}

static int
chDomainMigratePerform3Params(virDomainPtr dom,
                              const char *dconnuri,
                              virTypedParameterPtr params,
                              int nparams,
                              const char *cookiein,
                              int cookieinlen,
                              char **cookieout,
                              int *cookieoutlen,
                              unsigned int flags)
{
    const char *dname = NULL;
    const char *xmlin = NULL;
    const char *uri = NULL;
    int parallel_connections = 1;
    virDomainObj *vm;
    virCHDriver *driver = dom->conn->privateData;
    int rc = -1;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_URI,
                                &uri) < 0)
        goto error;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0)
        goto error;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_XML,
                                &xmlin) < 0) {
        goto error;
    }

    if (flags & VIR_MIGRATE_PARALLEL) {
        if (virTypedParamsGetInt(params, nparams,
                                VIR_MIGRATE_PARAM_PARALLEL_CONNECTIONS,
                                &parallel_connections) < 0) {
            DBG("Could not get param: VIR_MIGRATE_PARAM_PARALLEL_CONNECTIONS");
        } else {
            DBG("VIR_MIGRATE_PARAM_PARALLEL_CONNECTIONS: %d", parallel_connections);
        }
    }

    if (parallel_connections < 1) {
        DBG("Unexpected value of parallel_connections: %d. Setting value to 1.", parallel_connections);
        parallel_connections = 1;
    }

    DBG("chDomainMigratePerform3Params dconnuri: %s dname: %s parallel connection: %d", dconnuri, dname, parallel_connections);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        return -1;

    if (virDomainMigratePerform3EnsureACL(dom->conn, vm->def) < 0)
        goto error;

    rc = chDomainMigratePerform3Impl(vm,
                                     driver,
                                     xmlin,
                                     dconnuri,
                                     uri,
                                     cookiein,
                                     cookieinlen,
                                     cookieout,
                                     cookieoutlen,
                                     flags,
                                     dname,
                                     parallel_connections);
error:
    virDomainObjEndAPI(&vm);
    return rc;
}

static virDomainPtr
chDomainMigrateFinish3(virConnectPtr dconn,
                       const char *dname,
                       const char *cookiein,
                       int cookieinlen,
                       char **cookieout,
                       int *cookieoutlen,
                       const char *dconnuri G_GNUC_UNUSED,
                       const char *uri G_GNUC_UNUSED,
                       unsigned long flags,
                       int cancelled)
{
    virCHDriver *driver = dconn->privateData;
    virDomainObj *vm = NULL;
    virDomainPtr dom = NULL;
    virDomainDef *vmdef = NULL;
    virCHDomainObjPrivate *priv = NULL;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);

    DBG("chDomainMigrateFinish3 %p %s %s %d %p %p %lu %d",
        dconn, dname, cookiein, cookieinlen, cookieout, cookieoutlen, flags, cancelled);

    vm = virDomainObjListFindByName(driver->domains, dname);
    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%1$s'"), dname);
        return NULL;
    }

    if (virDomainMigrateFinish3EnsureACL(dconn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return NULL;
    }
    if (!(dom = virGetDomain(dconn, vm->def->name, vm->def->uuid, vm->def->id))) {
        virDomainObjEndAPI(&vm);
        DBG("virGetDomain failed.");
        return NULL;

    }

    priv = vm->privateData;

    // If cancelled == 1, the sender told us that there was an error on their side and
    // we need to abort the migration. We cannot expect the monitor to still function
    // properly at this point because chv currently handles rpc and migration within the
    // same thread.
    //
    // We also need to take care about our migration thread, because
    // it might be stuck in a recv() system call waiting for the migration to complete.
    if (cancelled == 0) {
        if (virCHProcessUpdateInfo(vm) < 0) {
            DBG("Could not update console info. Consider that non-fatal.");
        }
    } else {
        DBG("Migration was unsuccessful, cancel thread");
        virThreadCancel(priv->migrationDstReceiveThr);
    }

    virThreadJoin(priv->migrationDstReceiveThr);

    VIR_FREE(priv->migrationDstReceiveThr);

    if (virPortAllocatorRelease(priv->args->port) < 0) {
        DBG("Could not release migration port");
    }

    virMutexDestroy(&priv->args->mutex);

    if (virCondDestroy(&priv->args->cond) < 0) {
        DBG("Failed to destroy migration condition variable");
    }


    // If priv->args->success is false, our migrationDstReceiveThr indicated an error.
    if (priv->args->success == true && cancelled == 0) {
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, VIR_DOMAIN_RUNNING_MIGRATED);

        if (virCHProcessInitCpuAffinity(vm) < 0) {
            goto error;
        }

        if (virCHProcessSetup(vm) < 0) {
            goto error;
        }

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
            DBG("Failed to save status on vm %s", vm->def->name);
        }

        if (flags & VIR_MIGRATE_PERSIST_DEST) {
            DBG("Persisting domain at destination");
            vm->persistent = 1;
            if (!(vmdef = virDomainObjGetPersistentDef(driver->xmlopt, vm, NULL)))
                goto error;

            if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0)
                goto error;
        }
    } else {
        // Kill the chv process. We have no idea what happened, so better be on the
        // safe side. Only stopping the process, i.e. sending SIGTERM might result
        // unsuccessful network cleanup if the chv process is not plating nice with us.
        if (virCHProcessKill(driver, vm, VIR_DOMAIN_SHUTOFF_DESTROYED) < 0) {
            goto error;
        }

        virDomainObjRemoveTransientDef(vm);

        if (virDomainDeleteConfig(cfg->stateDir, cfg->autostartDir, vm) < 0) {
            goto error;
        }
        if (virDomainDeleteConfig(cfg->configDir, cfg->autostartDir, vm) < 0) {
            goto error;
        }

        virCHDomainRemoveInactive(driver, vm);
    }
error:
    if (priv->args->tcp_serial_url) {
        VIR_FREE(priv->args->tcp_serial_url);
    }
    VIR_FREE(priv->args);
    virDomainObjEndAsyncJob(vm);
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr
chDomainMigrateFinish3Params(virConnectPtr dconn,
                               virTypedParameterPtr params,
                               int nparams,
                               const char *cookiein,
                               int cookieinlen,
                               char **cookieout,
                               int *cookieoutlen,
                               unsigned int flags,
                               int cancelled)
{
    const char *dname = NULL;
    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0)
        return NULL;
    return chDomainMigrateFinish3(dconn, dname, cookiein, cookieinlen, cookieout, cookieoutlen, NULL, NULL, flags, cancelled);
}

static int
chDomainAttachDeviceLive(virDomainObj *vm,
                         virDomainDeviceDef *dev,
                         virCHDriver *driver)
{
    int ret = -1;
    virCHDomainObjPrivate *priv = vm->privateData;
    virCHMonitor *mon = priv->monitor;
    virJSONValue *response = NULL;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK: {
        g_autoptr(virJSONValue) disks = NULL;
        g_autofree char *payload = NULL;
        g_autofree char *idTmp = NULL;

        if (chAssignDeviceDiskAlias(vm->def, dev->data.disk) < 0) {
            DBG("assigning disk alias failed");
            break;
        }

        disks = virJSONValueNewArray();
        if (virCHMonitorBuildDiskJson(disks, dev->data.disk) < 0) {
            DBG("Attach disk failed");
            break;
        }
        payload = virJSONValueToString(virJSONValueArrayGet(disks, 0), false);

        VIR_DEBUG("Attach disk %s", payload);

        response = virCHMonitorPut(priv->monitor, URL_VM_ADD_DISK, payload, NULL);

        if (!response) {
            DBG("Attach disk failed. Invalid CH response.");
            break;
        }

        virDomainDiskInsert(vm->def, dev->data.disk);
        dev->data.disk = NULL;

        ret = 0;
        break;
    }
    case VIR_DOMAIN_DEVICE_NET:
    {
        virDomainNetInsert(vm->def, dev->data.net);
        ret = chProcessAddNetworkDevice(driver, mon, vm->def, dev->data.net);
        dev->data.net = NULL;
        break;
    }
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("live attach of device '%1$s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    virJSONValueFree(response);
    return ret;
}

static int
chDomainAttachDeviceConfig(virDomainDef *vmdef,
                           virDomainDeviceDef *dev,
                           unsigned int parse_flags,
                           virDomainXMLOption *xmlopt)
{
    virDomainDiskDef *disk;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        disk = dev->data.disk;
        if (virDomainDiskIndexByName(vmdef, disk->dst, true) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("target %1$s already exists"), disk->dst);
            return -1;
        }
        if (virDomainDiskTranslateSourcePool(disk) < 0) {
            DBG("virDomainDiskTranslateSourcePool failed");
            return -1;
        }

        virDomainDiskInsert(vmdef, disk);
        /* vmdef has the pointer. Generic codes for vmdef will do all jobs */
        dev->data.disk = NULL;
        break;
    case VIR_DOMAIN_DEVICE_NET:
    {
        /*
         * Calling g_steal_pointer here has the same effect as setting the
         * pointer to null like in the disk case. Not doing so leads to errors
         * in some cases e.g. hotplugging a persistant device and shutting down
         * and starting the domain.
         */
        virDomainNetInsert(vmdef, g_steal_pointer(&dev->data.net));
        break;
    }
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
         virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                        _("persistent attach of device '%1$s' is not supported"),
                        virDomainDeviceTypeToString(dev->type));
         return -1;
    }
    if (virDomainDefPostParse(vmdef, parse_flags, xmlopt, NULL) < 0) {
        DBG("virDomainDefPostParse failed");
        return -1;
    }

    if (virDomainDefValidate(vmdef, parse_flags, xmlopt, NULL) < 0) {
        DBG("virDomainDefValidate failed");
        return -1;
    }

    return 0;
}

static void
chDomainAttachDeviceLiveAndConfigHomogenize(const virDomainDeviceDef *devConf,
                                              virDomainDeviceDef *devLive)
{
    /*
     * Fixup anything that needs to be identical in the live and
     * config versions of DeviceDef, but might not be. Do this by
     * changing the contents of devLive. This is done after all
     * post-parse tweaks and validation, so be very careful about what
     * changes are made. (For example, it would be a very bad idea to
     * change assigned PCI, scsi, or sata addresses, as it could lead
     * to a conflict and there would be nothing to catch it except
     * qemu itself!)
     */

    /* MAC address should be identical in both DeviceDefs, but if it
     * wasn't specified in the XML, and was instead autogenerated, it
     * will be different for the two since they are each the result of
     * a separate parser call. If it *was* specified, it will already
     * be the same, so copying does no harm.
     */

    if (devConf->type == VIR_DOMAIN_DEVICE_NET)
        virMacAddrSet(&devLive->data.net->mac, &devConf->data.net->mac);

}

static int
chDomainAttachDeviceLiveAndConfig(virDomainObj *vm,
                                  virCHDriver *driver,
                                  const char *xml,
                                  unsigned int flags)
{
    // virCHDomainObjPrivate *priv = vm->privateData;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE |
                               VIR_DOMAIN_DEF_PARSE_ABI_UPDATE;
    virObjectEvent *event = NULL;
    g_autoptr(virDomainDeviceDef) devLive = NULL;
    g_autoptr(virDomainDef) vmdef = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    g_autoptr(virDomainDeviceDef) devConf = NULL;
    virDomainDeviceDef devConfSave = { 0 };

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virCHDriverGetConfig(driver);

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt,
                                              NULL/*priv->qemuCaps*/);
        if (!vmdef)
            return -1;

        if (!(devConf = virDomainDeviceDefParse(xml, vmdef,
                                                driver->xmlopt, NULL /*priv->qemuCaps*/,
                                                parse_flags)))
            return -1;
        /*
         * devConf will be NULLed out by
         * qemuDomainAttachDeviceConfig(), so save it for later use by
         * qemuDomainAttachDeviceLiveAndConfigHomogenize()
         */
        devConfSave = *devConf;

        if (virDomainDeviceValidateAliasForHotplug(vm, devConf,
                                                   VIR_DOMAIN_AFFECT_CONFIG) < 0)
            return -1;

        if (virDomainDefCompatibleDevice(vmdef, devConf, NULL,
                                         VIR_DOMAIN_DEVICE_ACTION_ATTACH,
                                         false) < 0)
            return -1;

        if (chDomainAttachDeviceConfig(vmdef, devConf,
                                         parse_flags,
                                         driver->xmlopt) < 0)
            return -1;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        if (!(devLive = virDomainDeviceDefParse(xml, vm->def,
                                                driver->xmlopt, NULL/*priv->qemuCaps*/,
                                                parse_flags))) {
            return -1;
        }

        if (flags & VIR_DOMAIN_AFFECT_CONFIG)
            chDomainAttachDeviceLiveAndConfigHomogenize(&devConfSave, devLive);

        if (virDomainDeviceValidateAliasForHotplug(vm, devLive,
                                                VIR_DOMAIN_AFFECT_LIVE) < 0)
            return -1;

        if (virDomainDefCompatibleDevice(vm->def, devLive, NULL,
                                        VIR_DOMAIN_DEVICE_ACTION_ATTACH,
                                        true) < 0) {
            return -1;
        }
        if (chDomainAttachDeviceLive(vm, devLive, driver) < 0) {
            return -1;
        }

        if (virDomainObjIsActive(vm)) {
            if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
                DBG("Failed to save status on vm %s", vm->def->name);
            }
        }
    }

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0) {
            DBG("virDomainDefSave failed");
            return -1;
        }
        virDomainObjAssignDef(vm, &vmdef, false, NULL);
        /* Event sending if persistent config has changed */
        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_DEFINED,
                                                  VIR_DOMAIN_EVENT_DEFINED_UPDATED);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    return 0;
}

static int
chDomainAttachDeviceFlags(virDomainPtr dom,
                          const char *xml,
                          unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    int ret = -1;

    DBG("chDomainAttachDeviceFlags \n%s\n", xml);

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainAttachDeviceFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjUpdateModificationImpact(vm, &flags) < 0)
        goto endjob;

    if (chDomainAttachDeviceLiveAndConfig(vm, driver, xml, flags) < 0)
        goto endjob;

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
chFindNet(virDomainDef *def, const char *dst)
{
    size_t i;

    for (i = 0; i < def->nnets; i++) {
        if (STREQ(def->nets[i]->ifname, dst))
            return i;
    }

    return -1;
}

static int
chDomainDetachPrepNet(virDomainObj *vm,
                      virDomainNetDef *match,
                      virDomainNetDef **detach)
{
    int idx;

    if (!match->ifname) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("no interface name specified"));
        return -1;
    }
    if ((idx = chFindNet(vm->def, match->ifname)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("net %1$s not found"), match->ifname);
        return -1;
    }
    *detach = vm->def->nets[idx];

    return 0;
}

static int
chFindDisk(virDomainDef *def, const char *dst)
{
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (STREQ(def->disks[i]->dst, dst))
            return i;
    }

    return -1;
}

static int
chDomainDetachPrepDisk(virDomainObj *vm,
                       virDomainDiskDef *match,
                       virDomainDiskDef **detach)
{
    virDomainDiskDef *disk;
    int idx;

    if ((idx = chFindDisk(vm->def, match->dst)) < 0) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("disk %1$s not found"), match->dst);
        return -1;
    }
    *detach = disk = vm->def->disks[idx];

    return 0;
}

static int
chDomainDetachDeviceLive(virDomainObj *vm,
                         virDomainDeviceDef *match,
                         virCHDriver */*driver*/,
                         bool /*async*/)
{
    virDomainDeviceDef detach = { .type = match->type };
    virDomainDeviceInfo *info = NULL;
    virCHDomainObjPrivate *priv = vm->privateData;
    int idx = 0;
    // int ret = -1;
    // int rc;

    if (match->type != VIR_DOMAIN_DEVICE_DISK && match->type != VIR_DOMAIN_DEVICE_NET) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("live detach of device '%1$s' is not supported"),
                       virDomainDeviceTypeToString(match->type));
        return -1;
    }

    if (match->type == VIR_DOMAIN_DEVICE_DISK) {
        if (chDomainDetachPrepDisk(vm, match->data.disk,
                                &detach.data.disk) < 0) {
            DBG("chDomainDetachPrepDisk failed");
            return -1;
        }
    } else if (match->type == VIR_DOMAIN_DEVICE_NET) {
        if (chDomainDetachPrepNet(vm, match->data.net,
                                 &detach.data.net) < 0) {
            DBG("chDomainDetachPrepNet failed");
            return -1;
        }

    }


    /* "detach" now points to the actual device we want to detach */

    if (!(info = virDomainDeviceGetInfo(&detach))) {
        /*
         * This should never happen, since all of the device types in
         * the switch cases that end with a "break" instead of a
         * return have a virDeviceInfo in them.
         */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("device of type '%1$s' has no device info"),
                       virDomainDeviceTypeToString(detach.type));
        return -1;
    }


    /* Make generic validation checks common to all device types */

    if (!info->alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot detach %1$s device with no alias"),
                       virDomainDeviceTypeToString(detach.type));
        return -1;
    }

    /**
     * TODO:
     * There can be multiple disks attached to a virtio slot e.g. vda or vdb
     * All of those disks should be removed in the XML and in CHV when detach is called.
     */

    DBG("Try to remove device with id %s", info->alias);
    if (virCHMonitorRemoveDevice(priv->monitor, info->alias) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("CH API call for device removal failed."));
        return -1;
    }

    if (match->type == VIR_DOMAIN_DEVICE_DISK) {
        idx = chFindDisk(vm->def, match->data.disk->dst);
        if (idx >= 0) {
            DBG("Remove device from libvirt xml state %d", idx);
            virDomainDiskRemove(vm->def, idx);
            g_clear_pointer(&detach.data.disk, virDomainDiskDefFree);
        }
    } else if (match->type == VIR_DOMAIN_DEVICE_NET) {
        virDomainInterfaceStopDevice(detach.data.net);
        virDomainInterfaceDeleteDevice(vm->def, detach.data.net, false, NULL);
        if ((idx = virDomainNetFindIdx(vm->def, detach.data.net)) < 0)
            return -1;

        virDomainNetRemove(vm->def, idx);
        g_clear_pointer(&detach.data.net, virDomainNetDefFree);
    }

//  cleanup:

    return 0;
}

static int
chDomainDetachDeviceConfig(virDomainDef *vmdef,
                             virDomainDeviceDef *dev,
                             virBitmap * /*chCaps*/,
                             // virQEMUCaps *qemuCaps,
                             unsigned int parse_flags,
                             virDomainXMLOption *xmlopt)
{
    virDomainDiskDef *disk;
    virDomainDiskDef *det_disk;
    virDomainNetDef *net;
    /*virDomainSoundDef *sound;
    virDomainHostdevDef *hostdev;
    virDomainHostdevDef *det_hostdev;
    virDomainLeaseDef *lease;
    virDomainLeaseDef *det_lease;
    virDomainControllerDef *cont;
    virDomainControllerDef *det_cont;
    virDomainChrDef *chr;
    virDomainFSDef *fs;
    virDomainMemoryDef *mem;*/
    int idx;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        disk = dev->data.disk;
        if (!(det_disk = virDomainDiskRemoveByName(vmdef, disk->dst))) {
            virReportError(VIR_ERR_DEVICE_MISSING,
                           _("no target device %1$s"), disk->dst);
            return -1;
        }
        virDomainDiskDefFree(det_disk);
        break;

    case VIR_DOMAIN_DEVICE_NET:
        net = dev->data.net;
        if ((idx = virDomainNetFindIdx(vmdef, net)) < 0)
            return -1;

        /* this is guaranteed to succeed */
        virDomainNetDefFree(virDomainNetRemove(vmdef, idx));
        break;

    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_AUDIO:
    case VIR_DOMAIN_DEVICE_CRYPTO:
    case VIR_DOMAIN_DEVICE_PSTORE:
    case VIR_DOMAIN_DEVICE_LAST:
         virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                        _("persistent deattach of device '%1$s' is not supported"),
                        virDomainDeviceTypeToString(dev->type));
         return -1;
    }
    if (virDomainDefPostParse(vmdef, parse_flags, xmlopt, NULL) < 0) {
        DBG("virDomainDefPostParse failed");
        return -1;
    }

    if (virDomainDefValidate(vmdef, parse_flags, xmlopt, NULL) < 0) {
        DBG("virDomainDefValidate failed");
        return -1;
    }

    return 0;
}

static int
chDomainDetachDeviceLiveAndConfig(virCHDriver *driver,
                                  virDomainObj *vm,
                                  const char *xml,
                                  unsigned int flags)
{
    virObjectEvent *event = NULL;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    g_autoptr(virDomainDeviceDef) dev_config = NULL;
    g_autoptr(virDomainDeviceDef) dev_live = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE;
    g_autoptr(virDomainDef) vmdef = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    DBG("chDomainDetachDeviceLiveAndConfig\nxml: %s", xml);

    cfg = virCHDriverGetConfig(driver);

    if ((flags & VIR_DOMAIN_AFFECT_CONFIG) &&
        !(flags & VIR_DOMAIN_AFFECT_LIVE))
        parse_flags |= VIR_DOMAIN_DEF_PARSE_INACTIVE;

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (!(dev_config = virDomainDeviceDefParse(xml, vm->def, driver->xmlopt,
                                                   NULL, parse_flags)))
            return -1;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        if (!(dev_live = virDomainDeviceDefParse(xml, vm->def, driver->xmlopt,
                                                 NULL, parse_flags))) {
            DBG("chDomainDetachDeviceLiveAndConfig failed");
            return -1;
        }
    }

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
         /* Make a copy for updated domain. */
         vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt, NULL);
         if (!vmdef)
             return -1;

         if (chDomainDetachDeviceConfig(vmdef, dev_config, NULL,
                                          parse_flags,
                                          driver->xmlopt) < 0)
             return -1;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        int rc;

        if ((rc = chDomainDetachDeviceLive(vm, dev_live, driver, false)) < 0)
            return -1;

        if (virDomainObjIsActive(vm)) {
            if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
                DBG("Failed to save status on vm %s", vm->def->name);
            }
        }
    }

    /* Finally, if no error until here, we can save config. */
    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0)
            return -1;

        virDomainObjAssignDef(vm, &vmdef, false, NULL);

        /* Event sending if persistent config has changed */
        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_DEFINED,
                                                  VIR_DOMAIN_EVENT_DEFINED_UPDATED);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    return 0;
}

static int
chDomainAttachDevice(virDomainPtr dom,
                     const char *xml)
{
    return chDomainAttachDeviceFlags(dom, xml, VIR_DOMAIN_AFFECT_LIVE);
}

static int
chDomainDetachDeviceFlags(virDomainPtr dom,
                          const char *xml,
                          unsigned int flags)
{
    virCHDriver *driver = dom->conn->privateData;
    virDomainObj *vm = NULL;
    int ret = -1;

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainDetachDeviceFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjUpdateModificationImpact(vm, &flags) < 0)
        goto endjob;

    if (chDomainDetachDeviceLiveAndConfig(driver, vm, xml, flags) < 0)
        goto endjob;

    ret = 0;

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int chDomainDetachDevice(virDomainPtr dom, const char *xml)
{
    return chDomainDetachDeviceFlags(dom, xml,
                                     VIR_DOMAIN_AFFECT_LIVE);
}

/* static void chNotifyLoadDomain(virDomainObj *vm, int newVM, void *opaque) */
/* { */
    /* virCHDriver *driver = opaque; */

    /* if (newVM) { */
        /* virObjectEvent *event = */
            /* virDomainEventLifecycleNewFromObj(vm, */
                                     /* VIR_DOMAIN_EVENT_DEFINED, */
                                     /* VIR_DOMAIN_EVENT_DEFINED_ADDED); */
        /* virObjectEventStateQueue(driver->domainEventState, event); */
    /* } */
/* } */
static int
chStateReload(void)
{
    /* g_autoptr(virCHDriverConfig) cfg = NULL; */

    DBG("in chStateReload\n");

    /* if (!ch_driver) */
        /* return 0; */

    /* cfg = virCHDriverGetConfig(ch_driver); */
    /* virDomainObjListLoadAllConfigs(ch_driver->domains, */
                                   /* cfg->configDir, */
                                   /* cfg->autostartDir, false, */
                                   /* ch_driver->xmlopt, */
                                   /* chNotifyLoadDomain, ch_driver); */
    return 0;
}

static int
chStateStop(void)
{
    DBG("in chStateStop\n");
    /* g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(qemu_driver); */
    /* virDomainDriverAutoShutdownConfig ascfg = { */
        /* .uri = cfg->uri, */
        /* .trySave = cfg->autoShutdownTrySave, */
        /* .tryShutdown = cfg->autoShutdownTryShutdown, */
        /* .poweroff = cfg->autoShutdownPoweroff, */
        /* .waitShutdownSecs = cfg->autoShutdownWait, */
        /* .saveBypassCache = cfg->autoSaveBypassCache, */
        /* .autoRestore = cfg->autoShutdownRestore, */
    /* }; */

    /* virDomainDriverAutoShutdown(&ascfg); */

    return 0;
}

static int
chStateShutdownPrepare(void)
{
    DBG("Shutdown libvirt daemon\n");
    // We can reach this function before chStateInitialize is completed.
    if (!ch_driver) {
        return 0;
    }

    virThreadPoolStop(ch_driver->workerPool);
    return 0;
}

static int
chStateShutdownWait(void)
{
    DBG("chStateShutdownWait\n");
    // We can reach this function before chStateInitialize is completed.
    if (!ch_driver) {
        VIR_WARN("No ch_driver object yet. Early return.");
        return 0;
    }
    /* virDomainObjListForEach(ch_driver->domains, false, */
                            /* qemuDomainObjStopWorkerIter, NULL); */
    virThreadPoolDrain(ch_driver->workerPool);
    virThreadPoolFree(ch_driver->workerPool);

    virBitmapFree(ch_driver->chCaps);

    virPortAllocatorRangeFree(ch_driver->migrationPorts);

    virInhibitorRelease(ch_driver->inhibitor);
    virInhibitorFree(ch_driver->inhibitor);

    virObjectUnref(ch_driver->domainEventState);
    virObjectUnref(ch_driver->xmlopt);
    virObjectUnref(ch_driver->domains);
    virObjectUnref(ch_driver->hostdevMgr);
    virObjectUnref(ch_driver->caps);

    virObjectUnref(ch_driver->config);

    VIR_FREE(ch_driver);
    return 0;
}

static
virDomainCaps *
virCHDriverGetDomainCapabilities(virCHDriver *driver,
                                 const char *machine,
                                 virArch arch,
                                 virDomainVirtType virttype)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    g_autoptr(virDomainCaps) domCaps = NULL;

    if (!(domCaps = virDomainCapsNew(NULL, machine, arch, virttype)))
        return NULL;

    // We initialize the mininum to make Nova happy and announce that we only support CPU pass-through
    domCaps->cpu.hostPassthrough = true;
    domCaps->cpu.hostPassthroughMigratable.report = true;
    domCaps->os.supported = VIR_TRISTATE_BOOL_YES;
    domCaps->os.firmware.report = false;
    VIR_DOMAIN_CAPS_ENUM_SET(domCaps->os.firmware, VIR_DOMAIN_OS_DEF_FIRMWARE_EFI);
    domCaps->os.loader.supported = VIR_TRISTATE_BOOL_YES;
    domCaps->os.loader.type.report = false;
    domCaps->os.loader.readonly.report = false;
    domCaps->os.loader.secure.report = true;
    VIR_DOMAIN_CAPS_ENUM_SET(domCaps->os.loader.secure,
                             VIR_TRISTATE_BOOL_NO);
    domCaps->video.supported = VIR_TRISTATE_BOOL_YES;
    domCaps->video.modelType.report = true;
    VIR_DOMAIN_CAPS_ENUM_SET(domCaps->video.modelType, VIR_DOMAIN_VIDEO_TYPE_NONE);

    return g_steal_pointer(&domCaps);
}

static char *
chConnectGetDomainCapabilities(virConnectPtr conn,
                               const char *emulatorbin,
                               const char *arch_str,
                               const char *machine,
                               const char *virttype_str,
                               unsigned int flags)
{
    virCHDriver *driver = conn->privateData;
    virArch arch = virArchFromHost();
    virDomainVirtType virttype = VIR_DOMAIN_VIRT_CH;
    g_autoptr(virDomainCaps) domCaps = NULL;

    VIR_DEBUG("Get Domain Capabilities Input: %s %s %s %s", emulatorbin, arch_str, machine, virttype_str);

    virCheckFlags(VIR_CONNECT_GET_DOMAIN_CAPABILITIES_DISABLE_DEPRECATED_FEATURES,
                  NULL);

    if (virConnectGetDomainCapabilitiesEnsureACL(conn) < 0)
        return NULL;

    if (!(domCaps = virCHDriverGetDomainCapabilities(driver,
                                                     machine,
                                                     arch, virttype)))
        return NULL;

    return virDomainCapsFormat(domCaps);
}

static int
chDomainBlockResize(virDomainPtr dom,
                      const char *path,
                      unsigned long long size,
                      unsigned int flags)
{
    virDomainObj *vm;
    virCHDomainObjPrivate *priv;
    int ret = -1;
    g_autofree char *device = NULL;
    virDomainDiskDef *disk = NULL;
    g_autofree char *payload = NULL;
    bool success = NULL;
    g_autoptr(virJSONValue) resize = NULL;

    DBG("chDomainBlockResize: path:%s size:%lld, flags:%x", path, size, flags);

    virCheckFlags(VIR_DOMAIN_BLOCK_RESIZE_BYTES |
                  VIR_DOMAIN_BLOCK_RESIZE_CAPACITY, -1);

    if ((flags & VIR_DOMAIN_BLOCK_RESIZE_BYTES) == 0) {
        if (size > ULLONG_MAX / 1024) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("size must be less than %1$llu"),
                           ULLONG_MAX / 1024);
            return -1;
        }
        size *= 1024;
    }

    if (!(vm = virCHDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainBlockResizeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjBeginJob(vm, VIR_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(disk = virDomainDiskByName(vm->def, path, false))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("disk '%1$s' was not found in the domain config"), path);
        goto endjob;
    }

    if (virStorageSourceIsEmpty(disk->src) || disk->src->readonly) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("can't resize empty or readonly disk '%1$s'"),
                       disk->dst);
        goto endjob;
    }

    if (virStorageSourceGetActualType(disk->src) == VIR_STORAGE_TYPE_VHOST_USER) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("block resize is not supported for vhostuser disk"));
        goto endjob;
    }

    if (flags & VIR_DOMAIN_BLOCK_RESIZE_CAPACITY) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "capacity resize is currently not supported");
        goto endjob;
    }

    if (disk->src->format == VIR_STORAGE_FILE_QCOW2 ||
        disk->src->format == VIR_STORAGE_FILE_QED) {
        size = VIR_ROUND_UP(size, 512);
    }

    if (disk->src->sliceStorage) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "slice storage is not supported");
        goto endjob;
    }

    resize = virJSONValueNewObject();

    if (virJSONValueObjectAppendString(resize, "id", disk->info.alias) < 0) {
        goto endjob;
    }
    if (virJSONValueObjectAppendNumberUlong(resize, "desired_size", size) < 0) {
        goto endjob;
    }

    payload = virJSONValueToString(resize, false);

    success = virCHMonitorPutNoResponse(priv->monitor, URL_VM_RESIZE_DISK, payload, NULL);

    if (success) {
        ret = 0;
    } else {
        DBG("Disk rezise failed. Invalid CH response.");
    }

 endjob:
    virDomainObjEndJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

/* Function Tables */
static virHypervisorDriver chHypervisorDriver = {
    .name = "CH",
    .connectURIProbe = chConnectURIProbe,
    .connectOpen = chConnectOpen,                           /* 7.5.0 */
    .connectClose = chConnectClose,                         /* 7.5.0 */
    .connectGetType = chConnectGetType,                     /* 7.5.0 */
    .connectGetVersion = chConnectGetVersion,               /* 7.5.0 */
    .connectGetHostname = chConnectGetHostname,             /* 7.5.0 */
    .connectNumOfDomains = chConnectNumOfDomains,           /* 7.5.0 */
    .connectListAllDomains = chConnectListAllDomains,       /* 7.5.0 */
    .connectListDomains = chConnectListDomains,             /* 7.5.0 */
    .connectGetCapabilities = chConnectGetCapabilities,     /* 7.5.0 */
    .connectSupportsFeature = chConnectSupportsFeature,     /* 8.1.0 */
    .domainCreateXML = chDomainCreateXML,                   /* 7.5.0 */
    .domainCreate = chDomainCreate,                         /* 7.5.0 */
    .domainCreateWithFlags = chDomainCreateWithFlags,       /* 7.5.0 */
    .domainGetJobInfo = chDomainGetJobInfo,                 /* 11.4.0 */
    .domainGetJobStats = chDomainGetJobStats,               /* 11.4.0 */
    .domainShutdown = chDomainShutdown,                     /* 7.5.0 */
    .domainShutdownFlags = chDomainShutdownFlags,           /* 7.5.0 */
    .domainReboot = chDomainReboot,                         /* 7.5.0 */
    .domainSuspend = chDomainSuspend,                       /* 7.5.0 */
    .domainResume = chDomainResume,                         /* 7.5.0 */
    .domainDestroy = chDomainDestroy,                       /* 7.5.0 */
    .domainDestroyFlags = chDomainDestroyFlags,             /* 7.5.0 */
    .domainDefineXML = chDomainDefineXML,                   /* 7.5.0 */
    .domainDefineXMLFlags = chDomainDefineXMLFlags,         /* 7.5.0 */
    .domainUndefine = chDomainUndefine,                     /* 7.5.0 */
    .domainUndefineFlags = chDomainUndefineFlags,           /* 7.5.0 */
    .domainLookupByID = chDomainLookupByID,                 /* 7.5.0 */
    .domainLookupByUUID = chDomainLookupByUUID,             /* 7.5.0 */
    .domainLookupByName = chDomainLookupByName,             /* 7.5.0 */
    .domainGetState = chDomainGetState,                     /* 7.5.0 */
    .domainGetXMLDesc = chDomainGetXMLDesc,                 /* 7.5.0 */
    .domainGetInfo = chDomainGetInfo,                       /* 7.5.0 */
    .domainIsActive = chDomainIsActive,                     /* 7.5.0 */
    .domainIsPersistent = chDomainIsPersistent,             /* 7.5.0 */
    .domainOpenConsole = chDomainOpenConsole,               /* 7.8.0 */
    .nodeGetInfo = chNodeGetInfo,                           /* 7.5.0 */
    .domainGetVcpus = chDomainGetVcpus,                     /* 8.0.0 */
    .domainGetVcpusFlags = chDomainGetVcpusFlags,           /* 8.0.0 */
    .domainGetMaxVcpus = chDomainGetMaxVcpus,               /* 8.0.0 */
    .domainGetVcpuPinInfo = chDomainGetVcpuPinInfo,         /* 8.0.0 */
    .domainPinVcpu = chDomainPinVcpu,                       /* 8.1.0 */
    .domainPinVcpuFlags = chDomainPinVcpuFlags,             /* 8.1.0 */
    .domainPinEmulator = chDomainPinEmulator,               /* 8.1.0 */
    .domainGetEmulatorPinInfo = chDomainGetEmulatorPinInfo, /* 8.1.0 */
    .nodeGetCPUMap = chNodeGetCPUMap,                       /* 8.0.0 */
    .domainSetNumaParameters = chDomainSetNumaParameters,   /* 8.1.0 */
    .domainGetNumaParameters = chDomainGetNumaParameters,   /* 8.1.0 */
    .domainSave = chDomainSave,                             /* 10.2.0 */
    .domainSaveFlags = chDomainSaveFlags,                   /* 10.2.0 */
    .domainSaveImageGetXMLDesc = chDomainSaveImageGetXMLDesc,   /* 10.2.0 */
    .domainManagedSave = chDomainManagedSave,               /* 10.2.0 */
    .domainManagedSaveRemove = chDomainManagedSaveRemove,   /* 10.2.0 */
    .domainManagedSaveGetXMLDesc = chDomainManagedSaveGetXMLDesc,   /* 10.2.0 */
    .domainHasManagedSaveImage = chDomainHasManagedSaveImage,   /* 10.2.0 */
    .domainRestore = chDomainRestore,                       /* 10.2.0 */
    .domainRestoreFlags = chDomainRestoreFlags,             /* 10.2.0 */
    .nodeGetMemoryStats = chNodeGetMemoryStats,             /* 10.10.0 */
    .connectDomainEventRegisterAny = chConnectDomainEventRegisterAny,       /* 10.10.0 */
    .connectDomainEventDeregisterAny = chConnectDomainEventDeregisterAny,   /* 10.10.0 */
    .domainInterfaceAddresses = chDomainInterfaceAddresses, /* 11.0.0 */
    .domainMigrateBegin3 = chDomainMigrateBegin3, /* 11.4.0 */
    .domainMigrateBegin3Params = chDomainMigrateBegin3Params, /* 11.4.0 */
    .domainMigratePrepare3 = chDomainMigratePrepare3, /* 11.4.0 */
    .domainMigratePrepare3Params = chDomainMigratePrepare3Params, /* 11.4.0 */
    .domainMigratePerform3 = chDomainMigratePerform3, /* 11.4.0 */
    .domainMigratePerform3Params = chDomainMigratePerform3Params, /* 11.4.0 */
    .domainMigrateFinish3 = chDomainMigrateFinish3, /* 11.4.0 */
    .domainMigrateFinish3Params = chDomainMigrateFinish3Params, /* 11.4.0 */
    .domainMigrateConfirm3 = chDomainMigrateConfirm3, /* 11.4.0 */
    .domainMigrateConfirm3Params = chDomainMigrateConfirm3Params, /* 11.4.0 */
    .domainAttachDevice = chDomainAttachDevice, /* 11.4.0 */
    .domainAttachDeviceFlags = chDomainAttachDeviceFlags, /* 11.4.0 */
    .domainDetachDevice = chDomainDetachDevice, /* 11.4.0 */
    .domainDetachDeviceFlags = chDomainDetachDeviceFlags, /* 11.4.0 */
    .connectGetDomainCapabilities = chConnectGetDomainCapabilities, /* 11.4.0 */
    .domainBlockResize = chDomainBlockResize, /* 11.4.0 */
};

static virConnectDriver chConnectDriver = {
    .localOnly = true,
    .uriSchemes = (const char *[]){"ch", NULL},
    .hypervisorDriver = &chHypervisorDriver,
};

static virStateDriver chStateDriver = {
    .name = "cloud-hypervisor",
    .stateInitialize = chStateInitialize,
    .stateCleanup = chStateCleanup,
    .stateReload = chStateReload,
    .stateStop = chStateStop,
    .stateShutdownPrepare = chStateShutdownPrepare,
    .stateShutdownWait = chStateShutdownWait,
};

int chRegister(void)
{
    if (virRegisterConnectDriver(&chConnectDriver, true) < 0)
        return -1;
    if (virRegisterStateDriver(&chStateDriver) < 0)
        return -1;
    return 0;
}
