#include <config.h>

#include "ch_driver.h"
#include "ch_migration_cookie.h"

#include "virbuffer.h"
#include "virlog.h"
#include "virutil.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_LOG_INIT("ch.ch_alias");

chMigrationCookie *
chMigrationCookieNew(const virDomainDef *def)
{
    g_autoptr(chMigrationCookie) migration_cookie = g_new0(chMigrationCookie, 1);
    size_t i;

    migration_cookie->nnets = def->nnets;
    migration_cookie->net = g_new0(chMigrationCookieNetData, def->nnets);

    for (i = 0; i < def->nnets; i++) {
        if (def->nets[i]->info.alias) {
            migration_cookie->net[i].alias = g_strdup(def->nets[i]->info.alias);
        }
    }

    migration_cookie->hasDiskAliases = true;
    migration_cookie->ndisks = def->ndisks;
    migration_cookie->disks = g_new0(chMigrationCookieDiskData, def->ndisks);

    for (i = 0; i < def->ndisks; i++) {
        const virDomainDiskDef *disk = def->disks[i];

        if (!disk->dst || !disk->info.alias || !*disk->info.alias) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Cannot migrate a disk without a target and alias"));
            return NULL;
        }

        migration_cookie->disks[i].target = g_strdup(disk->dst);
        migration_cookie->disks[i].alias = g_strdup(disk->info.alias);
    }

    return g_steal_pointer(&migration_cookie);
}

void
chMigrationCookieFree(chMigrationCookie *migration_cookie)
{
    size_t i;

    if (!migration_cookie) {
        return;
    }

    for (i = 0; i < migration_cookie->nnets; i++) {
        if (migration_cookie->net[i].alias) {
            g_free(migration_cookie->net[i].alias);
        }
    }
    g_free(migration_cookie->net);
    for (i = 0; i < migration_cookie->ndisks; i++) {
        g_free(migration_cookie->disks[i].target);
        g_free(migration_cookie->disks[i].alias);
    }
    g_free(migration_cookie->disks);
    g_free(migration_cookie);
}

int
chMigrationCookieXMLFormat(virBuffer *buf,
                           chMigrationCookie *migration_cookie)
{
    virBufferAddLit(buf, "<ch-migration>\n");
    virBufferAdjustIndent(buf, 2);
    virBufferAddLit(buf, "<network-aliases>\n");
    virBufferAdjustIndent(buf, 2);

    for (size_t i = 0; i < migration_cookie->nnets; i++) {
        if (migration_cookie->net[i].alias) {
            virBufferAsprintf(buf, "<interface index='%zu' alias='%s'/>\n",
                              i, migration_cookie->net[i].alias);
        } else {
            virBufferAsprintf(buf, "<interface index='%zu'/>\n", i);
        }
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</network-aliases>\n");

    if (migration_cookie->hasDiskAliases) {
        virBufferAddLit(buf, "<disk-aliases>\n");
        virBufferAdjustIndent(buf, 2);

        for (size_t i = 0; i < migration_cookie->ndisks; i++) {
            virBufferEscapeString(buf, "<disk target='%s'",
                                  migration_cookie->disks[i].target);
            virBufferEscapeString(buf, " alias='%s'/>\n",
                                  migration_cookie->disks[i].alias);
        }

        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "</disk-aliases>\n");
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</ch-migration>\n");
    return 0;
}

static chMigrationCookie *
chMigrationCookieXMLParse(xmlXPathContextPtr ctxt)
{
    g_autoptr(chMigrationCookie) migration_cookie = g_new0(chMigrationCookie, 1);
    size_t i;
    unsigned int net_dev_index;
    int n;
    g_autofree xmlNodePtr *interfaces = NULL;
    g_autofree xmlNodePtr *disks = NULL;
    xmlNodePtr root = ctxt->node;
    VIR_XPATH_NODE_AUTORESTORE(ctxt)

    if ((n = virXPathNodeSet("./network-aliases/interface", ctxt, &interfaces)) < 0)
        return NULL;

    migration_cookie->nnets = n;
    migration_cookie->net = g_new0(chMigrationCookieNetData, migration_cookie->nnets);

    for (i = 0; i < n; i++) {
        ctxt->node = interfaces[i];
        if (virXPathUInt("string(@index)", ctxt, &net_dev_index) < 0) {
            return NULL;
        }
        if (net_dev_index >= migration_cookie->nnets) {
            return NULL;
        }

        migration_cookie->net[net_dev_index].alias = virXMLPropString(ctxt->node, "alias");
    }

    ctxt->node = root;
    migration_cookie->hasDiskAliases = virXPathNode("./disk-aliases", ctxt) != NULL;

    if ((n = virXPathNodeSet("./disk-aliases/disk", ctxt, &disks)) < 0)
        return NULL;

    migration_cookie->ndisks = n;
    migration_cookie->disks = g_new0(chMigrationCookieDiskData, n);

    for (i = 0; i < n; i++) {
        chMigrationCookieDiskData *disk = &migration_cookie->disks[i];

        disk->target = virXMLPropString(disks[i], "target");
        disk->alias = virXMLPropString(disks[i], "alias");

        if (!disk->target || !*disk->target || !disk->alias || !*disk->alias)
            return NULL;
    }

    return g_steal_pointer(&migration_cookie);
}

static int
chMigrationCookieCheckDiskAlias(virDomainDef *def G_GNUC_UNUSED,
                                virDomainDeviceDef *dev,
                                virDomainDeviceInfo *info,
                                void *opaque)
{
    GHashTable *aliases = opaque;

    /* All disk aliases are replaced together, so check only other devices. */
    if (dev->type == VIR_DOMAIN_DEVICE_DISK || !info->alias ||
        !g_hash_table_contains(aliases, info->alias))
        return 0;

    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                   _("Migration disk alias '%1$s' conflicts with another device"),
                   info->alias);
    return -1;
}

/* Preserve source aliases before starting the receiver, ignoring caller
 * replacements because CH restores disk IDs from its migration stream. */
int
chMigrationCookieApplyDiskAliases(const chMigrationCookie *migration_cookie,
                                  virDomainDef *def)
{
    g_autoptr(GHashTable) targets = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) aliases = g_hash_table_new(g_str_hash, g_str_equal);
    size_t i;

    /* Old cookies do not carry disk aliases. */
    if (!migration_cookie || !migration_cookie->hasDiskAliases)
        return 0;

    if (migration_cookie->ndisks != def->ndisks) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Number of disks in migration cookie and migration XML does not match"));
        return -1;
    }

    for (i = 0; i < migration_cookie->ndisks; i++) {
        const chMigrationCookieDiskData *src = &migration_cookie->disks[i];
        virDomainDiskDef *disk = virDomainDiskByTarget(def, src->target);

        /* Domain XML formatting assumes aliases contain only safe characters.
         * Accept both generated aliases and user aliases without a prefix check. */
        if (!src->alias || !*src->alias ||
            strspn(src->alias,
                   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(src->alias)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Invalid disk alias in migration cookie for target '%1$s'"),
                           src->target);
            return -1;
        }

        if (!disk) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Migration disk target '%1$s' is missing from destination XML"),
                           src->target);
            return -1;
        }

        if (g_hash_table_contains(targets, src->target) ||
            g_hash_table_contains(aliases, src->alias)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Duplicate disk target or alias in migration cookie"));
            return -1;
        }

        g_hash_table_insert(targets, src->target, disk);
        g_hash_table_insert(aliases, src->alias, disk);
    }

    if (virDomainDeviceInfoIterate(def, chMigrationCookieCheckDiskAlias, aliases) < 0)
        return -1;

    for (i = 0; i < migration_cookie->ndisks; i++) {
        const chMigrationCookieDiskData *src = &migration_cookie->disks[i];
        virDomainDiskDef *disk = g_hash_table_lookup(targets, src->target);

        if (STREQ_NULLABLE(disk->info.alias, src->alias))
            continue;

        if (disk->info.alias) {
            VIR_WARN("DISK ALIAS OVERRIDE: domain '%s', disk '%s': ignoring "
                     "requested migration alias '%s'; preserving source alias '%s' "
                     "because Cloud Hypervisor restores the original disk ID!",
                     def->name, disk->dst, disk->info.alias, src->alias);
        }

        g_free(disk->info.alias);
        disk->info.alias = g_strdup(src->alias);
    }

    return 0;
}

chMigrationCookie *
chMigrationCookieParse(const char *cookiein, int cookieinlen)
{
    g_autoptr(xmlDoc) doc = NULL;
    g_autoptr(xmlXPathContext) ctxt = NULL;
    g_autoptr(chMigrationCookie) migration_cookie = NULL;

    if (!cookiein && !cookieinlen)
        return NULL;
    if (!cookiein && cookieinlen) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration cookie length provided with NULL pointer"));
        return NULL;
    }

    if (cookiein && !cookieinlen) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration cookie pointer provided with length of zero"));
        return NULL;
    }

    if (cookiein[cookieinlen - 1] != '\0') {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration cookie was not NULL terminated"));
        return NULL;
    }

    VIR_DEBUG("xml=%s", NULLSTR(cookiein));

    if (!(doc = virXMLParseStringCtxt(cookiein, _("(ch_migration_cookie)"), &ctxt)))
        return NULL;

    migration_cookie = chMigrationCookieXMLParse(ctxt);

    if (!migration_cookie) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration cookie parsing failed"));
        return NULL;
    }

    return g_steal_pointer(&migration_cookie);
}
