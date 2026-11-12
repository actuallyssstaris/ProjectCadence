#include "collections.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define COLL_FILE "/data/cadence/collections.txt"

typedef struct {
    char name[48];
    char **maps;
    int n, cap;
} coll;

static coll list[COLL_MAX];
static int count = -1;

static void add_map(coll *c, const char *file)
{
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        char **g = realloc(c->maps, sizeof(char *) * (size_t)cap);
        if (!g)
            return;
        c->maps = g;
        c->cap = cap;
    }
    c->maps[c->n] = strdup(file);
    if (c->maps[c->n])
        c->n++;
}

static int find(const char *name)
{
    for (int i = 0; i < count; i++)
        if (!strcasecmp(list[i].name, name))
            return i;
    return -1;
}

static void load(void)
{
    if (count >= 0)
        return;
    count = 0;
    FILE *f = fopen(COLL_FILE, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (tab)
            *tab = 0;
        int i = find(line);
        if (i < 0 && count < COLL_MAX && line[0]) {
            i = count++;
            snprintf(list[i].name, sizeof(list[i].name), "%s", line);
        }
        /* a name on its own line keeps an empty collection */
        if (i >= 0 && tab && tab[1])
            add_map(&list[i], tab + 1);
    }
    fclose(f);
}

static void save(void)
{
    FILE *f = fopen(COLL_FILE ".tmp", "w");
    if (!f)
        return;
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s\n", list[i].name);
        for (int k = 0; k < list[i].n; k++)
            fprintf(f, "%s\t%s\n", list[i].name, list[i].maps[k]);
    }
    fclose(f);
    rename(COLL_FILE ".tmp", COLL_FILE);
}

int coll_count(void)
{
    load();
    return count;
}

const char *coll_name(int i)
{
    load();
    return i >= 0 && i < count ? list[i].name : "";
}

int coll_size(int i)
{
    load();
    return i >= 0 && i < count ? list[i].n : 0;
}

bool coll_has(int i, const char *file)
{
    load();
    if (i < 0 || i >= count)
        return false;
    for (int k = 0; k < list[i].n; k++)
        if (!strcmp(list[i].maps[k], file))
            return true;
    return false;
}

bool coll_toggle(int i, const char *file)
{
    load();
    if (i < 0 || i >= count)
        return false;
    coll *c = &list[i];
    for (int k = 0; k < c->n; k++)
        if (!strcmp(c->maps[k], file)) {
            free(c->maps[k]);
            c->maps[k] = c->maps[--c->n];
            save();
            return false;
        }
    add_map(c, file);
    save();
    return true;
}

int coll_create(const char *name)
{
    load();
    if (!name[0] || count >= COLL_MAX || find(name) >= 0)
        return -1;
    memset(&list[count], 0, sizeof(list[0]));
    snprintf(list[count].name, sizeof(list[0].name), "%s", name);
    count++;
    save();
    return count - 1;
}

void coll_delete(int i)
{
    load();
    if (i < 0 || i >= count)
        return;
    for (int k = 0; k < list[i].n; k++)
        free(list[i].maps[k]);
    free(list[i].maps);
    memmove(&list[i], &list[i + 1], sizeof(list[0]) * (size_t)(count - i - 1));
    count--;
    save();
}
