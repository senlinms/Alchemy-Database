/*
 * Implements ALCHEMY_DATABASE StarSchema table joins
 *

AGPL License

Copyright (c) 2011 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

   This file is part of ALCHEMY_DATABASE

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "xdb_hooks.h"

#include "redis.h"

#include "debug.h"
#include "lru.h"
#include "lfu.h"
#include "qo.h"
#include "row.h"
#include "index.h"
#include "orderby.h"
#include "filter.h"
#include "range.h"
#include "find.h"
#include "alsosql.h"
#include "aobj.h"
#include "common.h"
#include "join.h"

// GLOBALS
extern r_tbl_t *Tbl;
extern r_ind_t *Index;

extern bool  OB_asc  [MAX_ORDER_BY_COLS];
extern uchar OB_ctype[MAX_ORDER_BY_COLS];

// CONSTANT GLOBALS
extern char *EMPTY_STRING;
extern char  OUTPUT_DELIM;

// TODO push into JoinBlock & -> single struct
bool JoinErr   =  0; bool JoinMiss  =  0;
long JoinCard  =  0; long JoinLoops =  0;
long JoinLim   = -1; long JoinOfst  = -1; bool JoinQed   =  0;

//TODO take Jcols -> too complicated
static aobj Resp;
static sl_t Jcols[MAX_JOIN_COLS];

static char *memclone(char *s, int len) {
    char *x = malloc(len); memcpy(x, s, len); return x;
}

void init_ijp(ijp_t *ij) {
    bzero(ij, sizeof(ijp_t));
    initFilter(&ij->lhs); initFilter(&ij->rhs);
    ij->nrows   = UINT_MAX; ij->kimatch = -1; ij->op = NONE;
}
void switchIJ(ijp_t *ij) {
    f_t temp;
    memcpy(&temp,    &ij->rhs, sizeof(f_t));
    memcpy(&ij->rhs, &ij->lhs, sizeof(f_t));
    memcpy(&ij->lhs, &temp,    sizeof(f_t));
}

bool validateJoinOrderBy(cli *c, jb_t *jb) {
    if (jb->wb.nob) { /* ORDER BY -> Table must be in join */
        for (uint32 j = 0; j < jb->wb.nob; j++) {
            bool hit = 0;
            for (int i = 0; i < jb->qcols; i++) {
                if (jb->js[i].t == jb->wb.obt[j]) { hit = 1; break; }
            }
            if (!hit) { addReply(c, shared.join_table_not_in_query); return 0; }
        }
    }
    return 1;
}

#define JOP_DEBUG_0                                          \
   printf("  top join_op: lvl: %d\n", g->co.lvl);            \
   printf("  join_op W:\n"); dumpW(printf, w);               \
   dumpFL(printf, "\t", "JOIN FILTER", flist);               \
   if (rrow) {                                               \
       printf("  join_op ROW:\n");                           \
       dumpRow(printf, g->co.btr, rrow, apk, w->wf.tmatch);  \
   }
#define JOP_DEBUG_1 \
    printf("  filters passed: qcols: %d tmatch: %d jan: %d\n", \
             g->se.qcols, w->wf.tmatch, w->wf.jan);
#define JOP_DEBUG_2                                                            \
    printf("    lvl: %d t: %d c: %d len: %d Jcols[%d](%p): ",                  \
            g->co.lvl, jb->js[i].t, jb->js[i].c, Jcols[i].len, i, Jcols[i].s); \
    dumpAobj(printf, &Resp);
#define JOP_DEBUG_3                                                       \
    printf("lvl: %d rrow: %p lhs.T: %d lhs.C: %d lhs.I: %d\n",            \
        g->co.lvl, rrow, ij->lhs.tmatch, ij->lhs.ic.cmatch,               \
        ij->lhs.ic.imatch); \
    if (nkl) dumpFL(printf, "\t", "KLIST", nkl);                          \
    else     printf("nkey: "); dumpAobj(printf, &nk);
#define JOP_DEBUG_4                                                       \
    printf("nkey: "); dumpAobj(printf, &nk);                              \
    printf("imatch: %d tmatch: %d\n", nimatch, ri->tmatch);
#define JOP_DEBUG_5                                                       \
    {   printf("rrow: %p\n", rrow);                                       \
        uchar ctype = Tbl[w->obt[i]].col[w->obc[i]].type;                 \
        printf("ASSIGN: "); dumpObKey(printf, i, jb->ob->keys[i], ctype); }
#define JOP_DEBUG_6                                      \
    printf("    join_op ROW:\n");                        \
    dumpRow(printf, g->co.btr, rrow, apk, w->wf.tmatch); \
    dumpFL(printf, "\t", "JOIN-FFLIST", jb->fflist);
#define DEBUG_PRE_RECURSE \
  printf("PRE RECURSE: join_op\n"); dumpW(printf, g2.co.w);
#define DEBUG_JOIN_GEN \
    printf("joinGeneric W\n"); dumpW(printf, w); dumpWB(printf, &jb->wb); \
    printf("joinGeneric JB\n"); dumpJB(server.alc.CurrClient, printf, jb);
#define DEBUG_JOIN_QED \
  printer *prn = printf; \
  (*prn)("\t\tJoinQed: %d JoinLim: %ld JoinOfst: %ld\n", \
         JoinQed, JoinLim, JoinOfst);

static bool checkOfst() {
    if (JoinQed) return 1; INCR(JoinLoops); return (JoinLoops >= JoinOfst);
}
static bool checkLimit() {
    if (JoinQed)       return 1;
    if (JoinLim == -1) return 1;
    if (JoinLim ==  0) return 0;
    JoinLim--; return 1;
}

static robj *EmbeddedJoinRobj = NULL;
static robj *join_reply_embedded(range_t *g) {
    if (!EmbeddedJoinRobj) EmbeddedJoinRobj = createObject(REDIS_STRING, NULL);
    erow_t *er = malloc(sizeof(erow_t));
    er->ncols  = g->se.qcols;
    er->cols   = malloc(sizeof(aobj *) * g->se.qcols);
    for (int i = 0; i < g->se.qcols; i++) {
        //TODO the sdsnewlen() is overhead -> factor it out
        sds s       = sdsnewlen(Jcols[i].s, Jcols[i].len); // FREE ME 110
        er->cols[i] = createAobjFromString(s, Jcols[i].len, Jcols[i].type);
        sdsfree(s);                                        // FREED 110
    }
    EmbeddedJoinRobj->ptr = er;
    return EmbeddedJoinRobj;
}
static robj *join_reply_redis(range_t *g) {
    char  pbuf[128];
    sl_t  outs[g->se.qcols];
    uint32 prelen = output_start(pbuf, 128, g->se.qcols);
    uint32 totlen = prelen;
    for (int i = 0; i < g->se.qcols; i++) {
        outs[i]  = outputSL(Jcols[i].type, Jcols[i]);
        totlen  += outs[i].len;
    }
    return write_output_row(g->se.qcols, prelen, pbuf, totlen, outs);
}
static robj *join_reply_norm(range_t *g) {
    uint32  totlen = 0;
    for (int i = 0; i < g->se.qcols; i++) {
        totlen += (Jcols[i].len + 1);
        if (C_IS_S(Jcols[i].type) && Jcols[i].len) totlen += 2; // 2 '' per col
    }
    totlen--; /* -1 no final comma */
    return write_output_row(g->se.qcols, 0, NULL, totlen, Jcols);
}
static robj *join_reply_lua(range_t *g) {
    //printf("join_reply_lua: func: %s\n", server.alc.OutputLuaFunc_Row);
    CLEAR_LUA_STACK lua_getglobal(server.lua, server.alc.OutputLuaFunc_Row);
    for (int i = 0; i < g->se.qcols; i++) {
        aobj a; initAobjFromStr(&a, Jcols[i].s, Jcols[i].len, COL_TYPE_STRING);
        sds cval = sdsnewlen(a.s, a.len);                // FREE 155
        lua_pushstring(server.lua, cval); sdsfree(cval); // FREED 155
    }
    int ret = DXDB_lua_pcall(server.lua, g->se.qcols, 1, 0);
    sds s   = ret ? sdsempty()                                    :
                    sdsnewlen((char*)lua_tostring(server.lua, -1),
                              lua_strlen(server.lua, -1));
    CLEAR_LUA_STACK
    return createObject(REDIS_STRING, s);
}

static robj *outputJoinRow(range_t *g) {
    if      (EREDIS) return join_reply_embedded(g);
    else if (OREDIS) return join_reply_redis   (g);
    else if (LREDIS) return join_reply_lua     (g);
    else             return join_reply_norm    (g);
}
static bool addReplyJoinRow(cli *c, robj *r) {
    return addReplyRow(c, r, -1, NULL, NULL, 0, NULL, 0);
}
static bool join_reply(range_t *g, long *card) {
    bool  ret = 1;
    jb_t *jb  = g->jb;   /* code compaction */
    robj *r   = outputJoinRow(g);
    if (JoinQed) {
        jb->ob->row = cloneRobj(r);
        listAddNodeTail(g->co.ll, jb->ob);
        jb->ob      = cloneOb(jb->ob, jb->wb.nob);       /* DESTROY ME 057 */
    } else {
        if (!addReplyJoinRow(g->co.c, r)) { ret = 0; goto join_rep_end; }
    }
    if (!(EREDIS)) INCR(*card);
    INCR(JoinCard);

join_rep_end:
    if (!(EREDIS)) decrRefCount(r);
    return ret;
}

static bool popKlist(range_t *g,   int   imatch, list **klist,
                     aobj    *apk, void *rrow,   int    jcmatch) {
    r_ind_t *ri   = &Index[imatch];
    list    *o_kl = *klist;
    o_kl->dup     = vcloneFilter;
    *klist        = listDup(o_kl);
    for (int i = 0; i < ri->nclist; i++) {
        listNode *ln = listIndex(*klist, i);
        if (!ln) continue;
        f_t  *flt     = ln->value;
        if (flt->op == NONE) {
            flt->akey = getCol(g->co.btr, rrow, ri->bclist[i], apk, ri->tmatch,
                               NULL); //TODO check what "ls" should be
            flt->op   = EQ;
        } else if (jcmatch == flt->ic.cmatch) {
            aobj akey = getCol(g->co.btr, rrow, ri->bclist[i], apk, ri->tmatch,
                               NULL);
            bool ok = aobjEQ(&flt->akey, &akey);
            releaseAobj(&akey);
            if (!ok) return 0;
        }
    } //dumpFL(printf, "\t", "KLIST", *klist);
    return 1;
}
static bool join_op(range_t *g, aobj *apk, void *rrow, bool q, long *card) {
    (void)q; // compiler warning
    if (JoinErr || JoinMiss)  return 0;
    if (!JoinLim)             return 1; // LIMIT OFFSET has been fulfilled
    jb_t   *jb     = g->jb;   /* code compaction */
    cswc_t *w      = g->co.w; /* code compaction */
    int     tmatch = w->wf.tmatch; /* code compaction */
    list   *flist  = jb->ij[g->co.lvl].flist;                      //JOP_DEBUG_0
    bool    hf     = 0;
    bool  pret     = passFilts(g->co.btr, apk, rrow, flist, tmatch, &hf);
    if (hf) return 0; if (!pret) return 1;
    char *freeme[g->se.qcols];                                     //JOP_DEBUG_1
    int   nfree = 0;
    DECLARE_ICOL(ic, -1)
    for (int i = 0; i < g->se.qcols; i++) { // Extract queried columns
        if (jb->js[i].jan == w->wf.jan) {
            int tm = jb->js[i].t; int cm = ic.cmatch = jb->js[i].c; 
            Resp = getSCol(g->co.btr, rrow, ic, apk, tm, g->se.lfca);//FREE 037
            Jcols[i].len    = Resp.len;
            Jcols[i].freeme = 0; /* freed via freeme[] */
            Jcols[i].type   = (cm == -1) ? COL_TYPE_NONE : Tbl[tm].col[cm].type;
            if (Resp.freeme) Jcols[i].s = Resp.s;
            else             Jcols[i].s = memclone(Resp.s, Resp.len);
            freeme[nfree++] = Jcols[i].s;                          //JOP_DEBUG_2
        }
    }
    int obfreei[jb->wb.nob];
    int obnfree = 0;
    if (jb->wb.nob) { // if ORDERBY store OBC's
        if (!jb->ob) jb->ob = create_obsl(NULL, jb->wb.nob); /*DESTROY ME 057*/
        for (uint32 i = 0; i < jb->wb.nob; i++) {
            if (jb->wb.obt[i] == tmatch) {
                int tm = jb->wb.obt[i]; int cm = jb->wb.obc[i].cmatch;
                uchar ctype = (cm == -1) ? COL_TYPE_NONE : Tbl[tm].col[cm].type;
                bool aret = assignObKey(&jb->wb, g->co.btr, rrow, apk, i,
                                        jb->ob,  tm);
                if (C_IS_S(ctype)) obfreei[obnfree++] = i;
                if (!aret) return 0; // MEMLEAK? need a "goto" cleanup
            }
        }
    }
    if (g->co.lvl == (uint32)jb->hw) { // Deepest Join Step        //JOP_DEBUG_6
        bool lhf  = 0;
        bool lret = passFilts(g->co.btr, apk, rrow, jb->fflist, tmatch, &lhf);
        if (!lhf && lret) {
            if (jb->cstar) { INCR(JoinCard); INCR(*card); }
            else if (checkOfst() && checkLimit()) lret = join_reply(g, card);
        }
        GET_LRUC updateLru(g->co.c, tmatch, apk, lruc, lrud);
        GET_LFUC updateLfu(g->co.c, tmatch, apk, lfuc, lfu);
        //NOTE rrow is no longer valid, updateLru() can change it
        for (int i = 0; i < obnfree; i++) {
            free(jb->ob->keys[obfreei[i]]); jb->ob->keys[obfreei[i]] = NULL;
        }
        for (int i = 0; i < nfree; i++) free(freeme[i]); /* FREE 037 */
        return lret;
    }
    bool ret = 1;
    { /* NOTE: this is the recursion step */
        aobj nk; initAobj(&nk); list *nkl; int nimatch; int jcmatch;
        ijp_t *ij   = &jb->ij[g->co.lvl];
        if (g->co.lvl == ((uint32)jb->hw - 1)) {
            nkl     = jb->fklist;
            nimatch = nkl ? jb->fkimatch : ij->rhs.imatch;
            jcmatch = jb->ij[g->co.lvl].rhs.ic.cmatch; /* RIGHT-HAND-SIDE */
        } else { /* next ij determines Join Index */
            nkl     = jb->ij[g->co.lvl + 1].lhs.klist;
            nimatch = nkl ? jb->ij[g->co.lvl + 1].lhs.imatch : ij->rhs.imatch;
            jcmatch = jb->ij[g->co.lvl].lhs.ic.cmatch; /* LEFT-HAND-SIDE */
        }
        bool ok = 1;
        if (nkl) ok = popKlist(g, nimatch, &nkl, apk, rrow, jcmatch);
        else {
            ic.cmatch = ij->lhs.ic.cmatch;
            nk = getCol(g->co.btr, rrow, ic, apk, ij->lhs.tmatch, g->se.lfca);
        }
        cswc_t w2; range_t g2; qr_t q2;                            //JOP_DEBUG_3
        r_ind_t *ri = &Index[nimatch];                             //JOP_DEBUG_4
        init_check_sql_where_clause(&w2, ri->tmatch, NULL);
        if (ok) {
            init_range(&g2, g->co.c, &w2, &jb->wb, &q2, g->co.ll,
                        g->co.ofree, jb);
            bzero(&q2, sizeof(qr_t)); //TODO only zeroed once, GLOBAL
            g2.se.qcols  = g->se.qcols;
            g2.co.lvl    = g->co.lvl + 1; /* INCR RECURSION LEVEL */
            w2.flist     = w->flist;
            if (nkl) promoteKLorFLtoW(&w2, &nkl, &w2.flist, 1);
            else {
                w2.wf.jan       = ij->rhs.jan;
                w2.wf.akey      = nk;
                w2.wf.imatch    = nimatch;
                w2.wf.tmatch    = ri->tmatch;
                w2.wf.ic.cmatch = ri->icol.cmatch;
                w2.wf.op        = EQ;
                w2.wtype     = (ri->virt) ? SQL_SINGLE_LKP : SQL_SINGLE_FK_LKP;
            }                                                //DEBUG_PRE_RECURSE
            if (w2.wf.imatch == -1) { JoinErr = 1; }
            else {
                long rcard = keyOp(&g2, join_op); /* RECURSE */
                if      (rcard == -1) { JoinMiss = 1; ret = 0; }
                GET_LRUC updateLru(g->co.c, tmatch, apk, lruc, lrud);
                GET_LFUC updateLfu(g->co.c, tmatch, apk, lfuc, lfu);
                //NOTE rrow is no longer valid, updateLru() can change it
                INCRBY(*card, rcard);
            }
        }
        if (nkl) {
            nkl->free = destroyFilter;
            listRelease(nkl); /* w2.wf.klist NULLed to not double-free */
            w2.wf.klist = NULL; releaseFilterR_KL(&w2.wf);
        }
        releaseAobj(&nk);
    }
    for (int i = 0; i < nfree; i++) { free(freeme[i]); /* FREE 037 */ }
    for (int i = 0; i < obnfree; i++) { /* just free this level's ob->leys[] */
        free(jb->ob->keys[obfreei[i]]); jb->ob->keys[obfreei[i]] = NULL;
    }
    return ret;
}
void setupFirstJoinStep(cswc_t *w, jb_t *jb, qr_t *q) {
    init_check_sql_where_clause(w, -1, NULL);
    ijp_t *ij = &jb->ij[0];
    promoteKLorFLtoW(w, &ij->lhs.klist, &ij->flist, 0);        //DEBUG_JOIN_GEN
    setQueued(w, &jb->wb, q);
    JoinQed   = q->qed;
    if (!JoinQed) {
        r_ind_t *ri = &Index[w->wf.imatch];
        for (uint32_t i = 0; i < jb->wb.nob; i++) {
            if (jb->wb.obt[i] != ri->tmatch) { JoinQed = 1; break; }
        }
    }
    JoinLim   = jb->wb.lim; JoinOfst = jb->wb.ofst;            //DEBUG_JOIN_QED
}
static bool checkForDotNotationJoins(cli *c, jb_t *jb, cswc_t *w) {
    if (w->wf.ic.nlo) { addReply(c, shared.joindotnotation); return 0; }
    for (int j = 0; j < jb->hw; j++) {
        ijp_t *ij = &jb->ij[j];
        if (ij->lhs.ic.nlo || ij->rhs.ic.nlo) {
            addReply(c, shared.joindotnotation); return 0;
        }}
    return 1;
}
bool joinGeneric(redisClient *c, jb_t *jb) {
    qr_t q; bzero(&q, sizeof(qr_t));
    cswc_t w; setupFirstJoinStep(&w, jb, &q);
    if (!checkForDotNotationJoins(c, jb, &w)) return 0;
    if (w.wf.imatch == -1) { addReply(c, shared.join_qo_err); return 0; }
    bool ret          = 0;
    c->LruColInSelect = initLRUCS_J(jb); c->LfuColInSelect = initLFUCS_J(jb);
    list *ll          = initOBsort(JoinQed, &jb->wb, 0);
    range_t g; init_range(&g, c, &w, &jb->wb, &q, ll, OBY_FREE_ROBJ, jb);
    g.se.qcols        = jb->qcols;
    JoinLoops         = -1; JoinCard = 0; JoinErr = 0; JoinMiss = 0;
    void *rlen        = jb->cstar ? NULL : addDeferredMultiBulkLength(c);
    long  card        = 0;
    if (Op(&g, join_op) == -1) JoinMiss = 1;
    if (JoinMiss) { replaceDMB(c, rlen, shared.dirty_miss);  goto join_gen_err;}
    if (JoinErr)  { replaceDMB(c, rlen, shared.join_qo_err); goto join_gen_err;}
    card              = JoinCard;
    long sent         =  0;
    if (card) {
        if (JoinQed) {
          if (!opSelectSort(c, ll, &jb->wb, g.co.ofree, &sent, -1))
              goto join_gen_err;
        } else sent = card;
    }
    if (JoinLim != -1 && sent < card) card = sent;
    if      (jb->cstar) addReplyLongLong(c, card);
    else if (!EREDIS)   setDMB_Join_card_cnames(c, jb, card, rlen);
    if (jb->wb.ovar) incrOffsetVar(c, &jb->wb, card);
    ret               = 1;

join_gen_err:
    releaseOBsort(ll);
    //TODO check that the next commented out line is not a mem-leak
    //TODO TEST is using TEXT pk joins
    //releaseAobj(&w.wf.akey);releaseAobj(&w.wf.alow); releaseAobj(&w.wf.ahigh);
    return ret;
}
