#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"

static int ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static void trim(char *text)
{
    char *end;
    while (*text && ascii_space((unsigned char)*text)) memmove(text, text + 1, strlen(text));
    end = text + strlen(text);
    while (end > text && ascii_space((unsigned char)end[-1])) *--end = '\0';
}

static void copy_text(char *destination, int capacity, const char *source)
{
    int i = 0;
    while (i + 1 < capacity && source[i]) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

int utubbu_catalog_load(UtubbuCatalog *catalog, const char *path)
{
    SceUID file;
    char *data;
    int bytes;
    char *cursor;
    char line[512];
    catalog->count = 0;
    file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0) return -1;
    data = malloc(16385);
    if (!data) { sceIoClose(file); return -1; }
    bytes = sceIoRead(file, data, 16384);
    sceIoClose(file);
    if (bytes < 0) { free(data); return -1; }
    data[bytes] = '\0';
    cursor = data;

    while (catalog->count < UTUBBU_MAX_ITEMS && *cursor) {
        char *first;
        char *second;
        char *newline;
        int length;
        UtubbuCatalogItem *item;
        newline = strchr(cursor, '\n');
        length = newline ? (int)(newline - cursor) : (int)strlen(cursor);
        if (length >= (int)sizeof(line)) length = sizeof(line) - 1;
        memcpy(line, cursor, length); line[length] = '\0';
        cursor = newline ? newline + 1 : cursor + strlen(cursor);
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        first = strchr(line, '|');
        if (!first) continue;
        *first++ = '\0';
        second = strchr(first, '|');
        if (!second) continue;
        *second++ = '\0';
        trim(line); trim(first); trim(second);
        if (!line[0] || !first[0] || !second[0]) continue;
        item = &catalog->items[catalog->count++];
        copy_text(item->title, sizeof(item->title), line);
        item->duration[0] = '\0';
        copy_text(item->source, sizeof(item->source), first);
        copy_text(item->local_name, sizeof(item->local_name), second);
    }
    free(data);
    return catalog->count;
}

static int contains_casefold(const char *text, const char *query)
{
    size_t query_length = strlen(query);
    const char *at;
    if (query_length == 0) return 1;
    for (at = text; *at; ++at) {
        size_t i;
        for (i = 0; i < query_length; ++i) {
            if (!at[i] || ascii_lower((unsigned char)at[i]) !=
                ascii_lower((unsigned char)query[i])) break;
        }
        if (i == query_length) return 1;
    }
    return 0;
}

int utubbu_catalog_matches(const UtubbuCatalogItem *item, const char *query)
{
    return contains_casefold(item->title, query);
}

static void history_safe(char *destination, int capacity, const char *source)
{
    int i=0;
    while(source[i]&&i+1<capacity){
        char c=source[i];
        destination[i]=(c=='|'||c=='\r'||c=='\n')?' ':c;
        ++i;
    }
    destination[i]='\0';
}

int utubbu_history_add(const UtubbuCatalogItem *item, const char *path)
{
    UtubbuCatalog old;
    UtubbuCatalogItem entries[5];
    SceUID file;
    int count=0,i;
    char line[512],title[64];
    entries[count++]=*item;
    if(utubbu_catalog_load(&old,path)>=0){
        for(i=0;i<old.count&&count<5;i++){
            if(strcmp(old.items[i].source,item->source))entries[count++]=old.items[i];
        }
    }
    file=sceIoOpen(path,PSP_O_CREAT|PSP_O_TRUNC|PSP_O_WRONLY,0777);
    if(file<0)return file;
    for(i=0;i<count;i++){
        int length;
        history_safe(title,sizeof(title),entries[i].title);
        length=snprintf(line,sizeof(line),"%s|%s|%s\n",title,entries[i].source,entries[i].local_name);
        if(length<0||length>=(int)sizeof(line)||sceIoWrite(file,line,length)!=length){sceIoClose(file);return -2;}
    }
    sceIoClose(file);
    return count;
}
