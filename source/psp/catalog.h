#ifndef UTUBBU_CATALOG_H
#define UTUBBU_CATALOG_H

#define UTUBBU_MAX_ITEMS 32

typedef struct UtubbuCatalogItem {
    char title[64];
    char duration[16];
    char source[256];
    char local_name[64];
} UtubbuCatalogItem;

typedef struct UtubbuCatalog {
    UtubbuCatalogItem items[UTUBBU_MAX_ITEMS];
    int count;
} UtubbuCatalog;

int utubbu_catalog_load(UtubbuCatalog *catalog, const char *path);
int utubbu_catalog_matches(const UtubbuCatalogItem *item, const char *query);
int utubbu_history_add(const UtubbuCatalogItem *item, const char *path);

#endif
