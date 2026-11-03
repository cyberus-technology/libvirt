#pragma once

#include "domain_conf.h"
#include "virbuffer.h"


typedef struct _chMigrationCookieNetData chMigrationCookieNetData;
struct _chMigrationCookieNetData {
    /* Device alias */
    char *alias;
};

typedef struct _chMigrationCookieDiskData chMigrationCookieDiskData;
struct _chMigrationCookieDiskData {
    char *target;
    char *alias;
};

typedef struct _chMigrationCookie chMigrationCookie;
struct _chMigrationCookie {

    /* Network properties*/
    /* How many virtual NICs are we saving data for? */
    int nnets;

    chMigrationCookieNetData *net;

    /* Distinguish older cookies from a source with no disks. */
    bool hasDiskAliases;
    size_t ndisks;
    chMigrationCookieDiskData *disks;
};

chMigrationCookie *
chMigrationCookieNew(const virDomainDef *def);

void
chMigrationCookieFree(chMigrationCookie *migration_cookie);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(chMigrationCookie, chMigrationCookieFree);

int
chMigrationCookieXMLFormat(virBuffer *buf,
                           chMigrationCookie *migration_cookie);

chMigrationCookie *
chMigrationCookieParse(const char *cookiein, int cookieinlen);

int
chMigrationCookieApplyDiskAliases(const chMigrationCookie *migration_cookie,
                                  virDomainDef *def);
