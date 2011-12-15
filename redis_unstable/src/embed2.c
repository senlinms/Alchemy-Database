
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <strings.h>

#include "redis.h"

#include "common.h"
#include "embed.h"

//#define DEBUG_EMBED2

//GLOBALS
extern eresp_t *CurrEresp; // USED in callbacks to get "CurrEresp->cnames[]"
extern bool GlobalZipSwitch; // can GLOBALLY turn off [lzf] compression of rows

extern int Num_tbls; // USED in thin_select
extern int Num_indx; // USED in thin_select

// PROTOTYPES
struct aobj;
void dumpAobj(printer *prn, struct aobj *a);
static void init_kv_table();
static void populate_kv_table(ulong prows);

// HELPERS
static long long mstime(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return ((long)tv.tv_sec)*1000 + tv.tv_usec/1000;
}

// CALLBACKS
static bool print_cb_w_cnames(erow_t* er) {
    printf("\tROW:\n");
    for (int i = 0; i < er->ncols; i++) {
        printf("\t\t%s: ", CurrEresp->cnames[i]); dumpAobj(printf, er->cols[i]);
    }
    return 1;
}

// DEBUG
static void hit_return_to_continue() {
#ifdef DEBUG_EMBED2
    char buff[80];
    printf("Hit Return To Continue:\n");
    if (!fgets(buff, sizeof(buff), stdin)) assert(!"fgets FAILED");
#endif
}
static debug_rows(ulong beg, ulong end) {
    ereq_t ereq; init_ereq(&ereq);
    ereq.op                 = SELECT;
    ereq.tablelist          = sdsnew("kv");
    ereq.scb                = print_cb_w_cnames;
    ereq.select_column_list = sdsnew("*");
    for (ulong i = beg; i < end; i++) {
        char lbuf[32];
        sprintf(lbuf, "pk = %u", i);
        ereq.where_clause = sdsnew(lbuf);
        e_alchemy_fast(&ereq);
        sdsfree(ereq.where_clause); ereq.where_clause = NULL;
    }
    hit_return_to_continue();
    release_ereq(&ereq);
}

// POPULATE
static void desc_table() {
    printf("\n");
    eresp_t *eresp = e_alchemy_raw("DESC kv", NULL);
    printEmbedResp(eresp); printf("\n");
}
static void init_kv_table() {
    e_alchemy_raw("DROP TABLE kv", NULL);
    e_alchemy_raw("CREATE TABLE kv (pk LONG, val TEXT)", NULL);
}
static void populate_kv_table(ulong prows) {
    char buf[1024]; char nbuf[16];
    char  *insert_string = "(00000000000001, '00000000000001 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX')";

    ereq_t ereq; init_ereq(&ereq);
    ereq.op        = INSERT;
    ereq.tablelist = sdsnew("kv");

    memcpy(buf, insert_string, strlen(insert_string) + 1);
    char *pkspot  = buf + 1;
    char *valspot = buf + 18;
    printf("Populating table KV: prows: %d zip: %d\n", prows, GlobalZipSwitch);
    long long beg = mstime(), fin, tps;
    for (ulong i = 1; i < prows; i++) {
        sprintf(nbuf, "%014lu", i); // Next 2 lines avoid a 420Byte sprintf()
        memcpy(pkspot, nbuf, 14); memcpy(valspot, nbuf, 14);
        ereq.insert_value_string = sdsnew(buf);
        e_alchemy_fast(&ereq);
        sdsfree(ereq.insert_value_string); ereq.insert_value_string = NULL;
    }
    fin = mstime(); tps = (fin == beg) ? 0 : prows / (fin - beg);
    printf("INSERT: %lu rows, duration: %lld ms, %lldK TPS zip: %d\n",
           prows, (fin - beg), tps, GlobalZipSwitch);

    desc_table();
    hit_return_to_continue();
    release_ereq(&ereq);
}


static void test_zip_select(ulong prows, ulong qrows) {
    ereq_t ereq; init_ereq(&ereq);
    init_kv_table(); populate_kv_table(prows);

    ereq.op                 = SELECT;
    ereq.tablelist          = sdsnew("kv");
#ifdef DEBUG_EMBED2
    ereq.scb                = print_cb_w_cnames;
#endif
    ereq.select_column_list = sdsnew("val");

    long long beg = mstime(), fin, tps;
    for (ulong i = 1; i < qrows; i++) {
        char lbuf[32];
        uint32 index      = rand() % prows + 1;
        sprintf(lbuf, "pk = %u", index);
        ereq.where_clause = sdsnew(lbuf);
        e_alchemy_fast(&ereq);
        sdsfree(ereq.where_clause); ereq.where_clause = NULL;
    }
    fin = mstime(); tps = (fin == beg) ? 0 : qrows / (fin - beg);
    printf("SELECT: %lu rows, duration: %lld ms, %lldK TPS\n\n",
           qrows, (fin - beg), tps);
    hit_return_to_continue();
    release_ereq(&ereq);
}

#define MANUAL_SELECT_QUERY                                         \
    bool    cstar = 0;  /* NOT SELECT COUNT(*) */                   \
    int     qcols = 1;  /* SELECT val -> 1 query column */          \
    int     cmatchs[qcols];                                         \
    cmatchs[0]    = 1;  /* SELECT val -> column number 1 */         \
    uchar   qtype = SQL_SINGLE_LKP; /* WHERE pk = 1 -> PK lookup */ \
    enum OP op    = EQ;                                             \
    int tmatch    = Num_tbls - 1; /* last table created */          \
    int cmatch    = 0;                                              \
    int imatch    = Num_indx - 1; /* last index created */

static void test_zip_thinselect(ulong prows, ulong qrows) {
    ereq_t ereq; init_ereq(&ereq);
    MANUAL_SELECT_QUERY
    init_kv_table(); populate_kv_table(prows);

    select_callback *scb = NULL;
#ifdef DEBUG_EMBED2
    scb                  = print_cb_w_cnames;
#endif
    long long beg = mstime(), fin, tps;
    for (ulong i = 1; i < qrows; i++) {
        ulong    index = (ulong)rand() % prows + 1;
        e_alchemy_thin_select(qtype, tmatch, cmatch, imatch, op, qcols,
                              0, index, 0, cmatchs, cstar, scb);
    }
    fin = mstime(); tps = (fin == beg) ? 0 : qrows / (fin - beg);
    printf("THIN SELECT: %lu rows, duration: %lld ms, %lldK TPS\n\n",
           qrows, (fin - beg), tps);
    hit_return_to_continue();
    release_ereq(&ereq);
}
static void test_nocompression_thinselect(ulong prows, ulong qrows) {
    ereq_t ereq; init_ereq(&ereq);
    MANUAL_SELECT_QUERY
    GlobalZipSwitch = 0; // turn compression -> OFF
    init_kv_table(); populate_kv_table(prows);
    GlobalZipSwitch = 1; // turn compression -> BACK ON

    select_callback *scb = NULL;
#ifdef DEBUG_EMBED2
    scb                  = print_cb_w_cnames;
#endif

    long long beg = mstime(), fin, tps;
    for (ulong i = 1; i < qrows; i++) {
        ulong    index = (ulong)rand() % prows + 1;
        e_alchemy_thin_select(qtype, tmatch, cmatch, imatch, op, qcols,
                              0, index, 0, cmatchs, cstar, scb);
    }
    fin = mstime();
    tps = (fin == beg) ? 0 : qrows / (fin - beg);
    printf("THIN SELECT [NO ZIP]: %lu rows, duration: %lld ms, %lldK TPS\n\n",
           qrows, (fin - beg), tps);
    hit_return_to_continue();
    release_ereq(&ereq);
}
static void test_delete(ulong prows, ulong qrows) {
    ereq_t ereq; init_ereq(&ereq);
    init_kv_table(); populate_kv_table(prows);

    ereq.op                 = DELETE;
    ereq.tablelist          = sdsnew("kv");
#ifdef DEBUG_EMBED2
    ereq.scb                = print_cb_w_cnames;
#endif

    ulong     iters = (prows / 2); // DELETE first HALF of table
    long long beg = mstime(), fin, tps;
    for (ulong i = 1; i < iters; i++) {
        char lbuf[32];
        sprintf(lbuf, "pk = %u", i);
        ereq.where_clause = sdsnew(lbuf);
        e_alchemy_fast(&ereq);
        sdsfree(ereq.where_clause); ereq.where_clause = NULL;
    }
    fin = mstime(); tps = (fin == beg) ? 0 : qrows / (fin - beg);
    printf("DELETE: %lu rows, duration: %lld ms, %lldK TPS\n\n",
           qrows, (fin - beg), tps);
    hit_return_to_continue();
    printf("AFTER DELETION\n"); desc_table();
    release_ereq(&ereq);
}
static void test_update(ulong prows, ulong qrows) {
    ereq_t ereq; init_ereq(&ereq);
    init_kv_table(); populate_kv_table(prows);

    ereq.op              = UPDATE;
    ereq.tablelist       = sdsnew("kv");
#ifdef DEBUG_EMBED2
    ereq.scb             = print_cb_w_cnames;
#endif
    ereq.update_set_list = sdsnew("val='not much text'");

    ulong     iters = (prows / 2); // UPDATE first HALF of table
    long long beg = mstime(), fin, tps;
    for (ulong i = 1; i < iters; i++) {
        char lbuf[32];
        sprintf(lbuf, "pk = %u", i);
        ereq.where_clause = sdsnew(lbuf);
        e_alchemy_fast(&ereq);
        sdsfree(ereq.where_clause); ereq.where_clause = NULL;
    }
    fin = mstime(); tps = (fin == beg) ? 0 : qrows / (fin - beg);
    printf("UPDATE: %lu rows, duration: %lld ms, %lldK TPS\n\n",
           qrows, (fin - beg), tps);
    hit_return_to_continue();
    printf("AFTER UPDATE\n"); desc_table();
    //debug_rows((prows / 2 - 2), (prows / 2 + 2));
    release_ereq(&ereq);
}

int main(int argc, char **argv) {
    argc = 0; argv = NULL; /* compiler warning */
    ulong  prows = 1000000;
    ulong  qrows = 2000000;
#ifdef DEBUG_EMBED2
    prows = 5; qrows = 5;
#endif
    test_zip_select              (prows, qrows);
    test_zip_thinselect          (prows, qrows);
    test_nocompression_thinselect(prows, qrows);
    test_delete                  (prows, qrows);
    test_update                  (prows, qrows);
    printf("Exiting...\n");
    return 0;
}
