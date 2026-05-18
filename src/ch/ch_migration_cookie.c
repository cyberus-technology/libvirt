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

    return g_steal_pointer(&migration_cookie);
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
