#ifndef OSU5_WEB_H
#define OSU5_WEB_H

#include <stdbool.h>

/* osu.ppy.sh rankings and player profiles, through online.c's API keys.
 * Each fetch runs on its own thread; poll the getters every frame. */

enum { WEB_IDLE, WEB_LOADING, WEB_READY, WEB_FAILED };

#define WEB_RANKS 50

typedef struct {
    int id;
    char user[32];
    char country[4];
    int rank, plays;
    float pp, acc;
} web_rank;

typedef struct {
    int state, mode, page;
    int count;
    web_rank r[WEB_RANKS];
} web_rankings;

typedef struct {
    int state, mode;
    int id;
    char user[32], country[4], joined[12];
    int rank, country_rank, plays, max_combo, level, level_pct;
    int hours;
    float pp, acc;
    long long ranked_score, total_score;
    int ss, s, a;
    bool supporter;
} web_user;

void web_rankings_get(int mode, int page);
void web_rankings_read(web_rankings *out);

/* who: a user id, or a name */
void web_user_get(const char *who, int mode);
void web_user_read(web_user *out);

/* a beatmap set's page: its details, rating and the newest comments */
#define WEB_COMMENTS 30
typedef struct {
    int state, id;
    char title[96], artist[96], creator[32], source[96], genre[32], language[32], tags[200], status[16];
    int plays, favourites;
    float bpm, rating; /* rating 0-10, -1 none */
    int ncomments;
    struct { char user[32]; char text[240]; int votes; } comments[WEB_COMMENTS];
} web_set;

void web_set_get(int set_id);
void web_set_read(web_set *out);

/* osu! news posts and the wiki, which need no keys */
#define WEB_NEWS 20
typedef struct {
    int state;
    int count;
    struct { char title[128], author[48], date[12], preview[300], slug[96]; } post[WEB_NEWS];
} web_news;
void web_news_get(void);
void web_news_read(web_news *out);

/* a wiki page as plain lines; path like "Main_page" or "Game_mode/osu!" */
#define WEB_LINES 400
typedef struct {
    int state;
    char path[128];
    int count;
    char line[WEB_LINES][160];
    char links[40][96]; /* the pages it links to, in order */
    int nlinks;
} web_page;
void web_wiki_get(const char *path);
/* our own release notes, newest first, from GitHub */
void web_changelog_get(void);
void web_page_read(web_page *out);

#endif
