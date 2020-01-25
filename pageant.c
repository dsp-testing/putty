/*
 * pageant.c: cross-platform code to implement Pageant.
 */

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#include "putty.h"
#include "mpint.h"
#include "ssh.h"
#include "sshcr.h"
#include "pageant.h"

/*
 * We need this to link with the RSA code, because rsa_ssh1_encrypt()
 * pads its data with random bytes. Since we only use rsa_ssh1_decrypt()
 * and the signing functions, which are deterministic, this should
 * never be called.
 *
 * If it _is_ called, there is a _serious_ problem, because it
 * won't generate true random numbers. So we must scream, panic,
 * and exit immediately if that should happen.
 */
void random_read(void *buf, size_t size)
{
    modalfatalbox("Internal error: attempt to use random numbers in Pageant");
}

static bool pageant_local = false;

typedef struct PageantKeySort PageantKeySort;
typedef struct PageantKey PageantKey;
typedef struct PageantAsyncOp PageantAsyncOp;
typedef struct PageantAsyncOpVtable PageantAsyncOpVtable;
typedef struct PageantClientRequestNode PageantClientRequestNode;
typedef struct PageantKeyRequestNode PageantKeyRequestNode;

struct PageantClientRequestNode {
    PageantClientRequestNode *prev, *next;
};
struct PageantKeyRequestNode {
    PageantKeyRequestNode *prev, *next;
};

struct PageantClientInfo {
    PageantClient *pc; /* goes to NULL when client is unregistered */
    PageantClientRequestNode head;
};

struct PageantAsyncOp {
    const PageantAsyncOpVtable *vt;
    PageantClientInfo *info;
    PageantClientRequestNode cr;
    PageantClientRequestId *reqid;
};
struct PageantAsyncOpVtable {
    void (*coroutine)(PageantAsyncOp *pao);
    void (*free)(PageantAsyncOp *pao);
};
static inline void pageant_async_op_coroutine(PageantAsyncOp *pao)
{ pao->vt->coroutine(pao); }
static inline void pageant_async_op_free(PageantAsyncOp *pao)
{
    delete_callbacks_for_context(pao);
    pao->vt->free(pao);
}
static inline void pageant_async_op_unlink(PageantAsyncOp *pao)
{
    pao->cr.prev->next = pao->cr.next;
    pao->cr.next->prev = pao->cr.prev;
}
static inline void pageant_async_op_unlink_and_free(PageantAsyncOp *pao)
{
    pageant_async_op_unlink(pao);
    pageant_async_op_free(pao);
}
static void pageant_async_op_callback(void *vctx)
{
    pageant_async_op_coroutine((PageantAsyncOp *)vctx);
}

/*
 * Master list of all the keys we have stored, in any form at all.
 */
static tree234 *keytree;
struct PageantKeySort {
    /* Prefix of the main PageantKey structure which contains all the
     * data that the sorting order depends on. Also simple enough that
     * you can construct one for lookup purposes. */
    int ssh_version; /* 1 or 2; primary sort key */
    ptrlen public_blob; /* secondary sort key */
};
struct PageantKey {
    PageantKeySort sort;
    strbuf *public_blob; /* the true owner of sort.public_blob */
    char *comment;       /* stored separately, whether or not in rkey/skey */
    union {
        RSAKey *rkey;       /* if ssh_version == 1 */
        ssh2_userkey *skey; /* if ssh_version == 2 */
    };
};

static void failure(PageantClient *pc, PageantClientRequestId *reqid,
                    strbuf *sb, const char *fmt, ...);

static void pk_free(PageantKey *pk)
{
    if (pk->public_blob) strbuf_free(pk->public_blob);
    sfree(pk->comment);
    if (pk->sort.ssh_version == 1 && pk->rkey) {
        freersakey(pk->rkey);
        sfree(pk->rkey);
    }
    if (pk->sort.ssh_version == 2 && pk->skey) {
        sfree(pk->skey->comment);
        ssh_key_free(pk->skey->key);
        sfree(pk->skey);
    }
    sfree(pk);
}

static int cmpkeys(void *av, void *bv)
{
    PageantKeySort *a = (PageantKeySort *)av, *b = (PageantKeySort *)bv;

    if (a->ssh_version != b->ssh_version)
        return a->ssh_version < b->ssh_version ? -1 : +1;
    else
        return ptrlen_strcmp(a->public_blob, b->public_blob);
}

static inline PageantKeySort keysort(int version, ptrlen blob)
{
    PageantKeySort sort;
    sort.ssh_version = version;
    sort.public_blob = blob;
    return sort;
}

static strbuf *makeblob1(RSAKey *rkey)
{
    strbuf *blob = strbuf_new();
    rsa_ssh1_public_blob(BinarySink_UPCAST(blob), rkey,
                         RSA_SSH1_EXPONENT_FIRST);
    return blob;
}

static strbuf *makeblob2(ssh2_userkey *skey)
{
    strbuf *blob = strbuf_new();
    ssh_key_public_blob(skey->key, BinarySink_UPCAST(blob));
    return blob;
}

static PageantKey *findkey1(RSAKey *reqkey)
{
    strbuf *blob = makeblob1(reqkey);
    PageantKeySort sort = keysort(1, ptrlen_from_strbuf(blob));
    PageantKey *toret = find234(keytree, &sort, NULL);
    strbuf_free(blob);
    return toret;
}

static PageantKey *findkey2(ptrlen blob)
{
    PageantKeySort sort = keysort(2, blob);
    return find234(keytree, &sort, NULL);
}

static int find_first_key_for_version(int ssh_version)
{
    PageantKeySort sort = keysort(ssh_version, PTRLEN_LITERAL(""));
    int pos;
    if (findrelpos234(keytree, &sort, NULL, REL234_GE, &pos))
        return pos;
    return count234(keytree);
}

static int count_keys(int ssh_version)
{
    return (find_first_key_for_version(ssh_version + 1) -
            find_first_key_for_version(ssh_version));
}
int pageant_count_ssh1_keys(void) { return count_keys(1); }
int pageant_count_ssh2_keys(void) { return count_keys(2); }

bool pageant_add_ssh1_key(RSAKey *rkey)
{
    PageantKey *pk = snew(PageantKey);
    memset(pk, 0, sizeof(PageantKey));
    pk->sort.ssh_version = 1;
    pk->public_blob = makeblob1(rkey);
    pk->sort.public_blob = ptrlen_from_strbuf(pk->public_blob);

    if (add234(keytree, pk) == pk) {
        pk->rkey = rkey;
        if (rkey->comment)
            pk->comment = dupstr(rkey->comment);
        return true;
    } else {
        pk_free(pk);
        return false;
    }
}

bool pageant_add_ssh2_key(ssh2_userkey *skey)
{
    PageantKey *pk = snew(PageantKey);
    memset(pk, 0, sizeof(PageantKey));
    pk->sort.ssh_version = 2;
    pk->public_blob = makeblob2(skey);
    pk->sort.public_blob = ptrlen_from_strbuf(pk->public_blob);

    if (add234(keytree, pk) == pk) {
        pk->skey = skey;
        if (skey->comment)
            pk->comment = dupstr(skey->comment);
        return true;
    } else {
        pk_free(pk);
        return false;
    }
}

static void remove_all_keys(int ssh_version)
{
    int start = find_first_key_for_version(ssh_version);
    int end = find_first_key_for_version(ssh_version + 1);
    while (end > start) {
        PageantKey *pk = delpos234(keytree, --end);
        assert(pk->sort.ssh_version == ssh_version);
        pk_free(pk);
    }
}

static void list_keys(BinarySink *bs, int ssh_version)
{
    int i;
    PageantKey *pk;

    put_uint32(bs, count_keys(ssh_version));
    for (i = find_first_key_for_version(ssh_version);
         NULL != (pk = index234(keytree, i)); i++) {
        if (pk->sort.ssh_version != ssh_version)
            break;

        if (ssh_version > 1)
            put_stringpl(bs, pk->sort.public_blob);
        else
            put_datapl(bs, pk->sort.public_blob); /* no header */

        put_stringpl(bs, ptrlen_from_asciz(pk->comment));
    }
}

void pageant_make_keylist1(BinarySink *bs) { return list_keys(bs, 1); }
void pageant_make_keylist2(BinarySink *bs) { return list_keys(bs, 2); }

void pageant_register_client(PageantClient *pc)
{
    pc->info = snew(PageantClientInfo);
    pc->info->pc = pc;
    pc->info->head.prev = pc->info->head.next = &pc->info->head;
}

void pageant_unregister_client(PageantClient *pc)
{
    PageantClientInfo *info = pc->info;
    assert(info);
    assert(info->pc == pc);

    while (pc->info->head.next != &pc->info->head) {
        PageantAsyncOp *pao = container_of(pc->info->head.next,
                                           PageantAsyncOp, cr);
        pageant_async_op_unlink_and_free(pao);
    }

    sfree(pc->info);
}

static void failure(PageantClient *pc, PageantClientRequestId *reqid,
                    strbuf *sb, const char *fmt, ...)
{
    strbuf_clear(sb);
    put_byte(sb, SSH_AGENT_FAILURE);
    if (!pc->suppress_logging) {
        va_list ap;
        va_start(ap, fmt);
        char *msg = dupprintf(fmt, ap);
        va_end(ap);
        pageant_client_log(pc, reqid, "reply: SSH_AGENT_FAILURE (%s)", msg);
        sfree(msg);
    }
}

typedef struct PageantImmOp PageantImmOp;
struct PageantImmOp {
    int crLine;
    strbuf *response;

    PageantAsyncOp pao;
};

static void immop_free(PageantAsyncOp *pao)
{
    PageantImmOp *io = container_of(pao, PageantImmOp, pao);
    strbuf_free(io->response);
    sfree(io);
}

static void immop_coroutine(PageantAsyncOp *pao)
{
    PageantImmOp *io = container_of(pao, PageantImmOp, pao);

    crBegin(io->crLine);

    if (0) crReturnV;

    pageant_client_got_response(io->pao.info->pc, io->pao.reqid,
                                ptrlen_from_strbuf(io->response));
    pageant_async_op_unlink_and_free(&io->pao);
    crFinishFreedV;
}

static struct PageantAsyncOpVtable immop_vtable = {
    immop_coroutine,
    immop_free,
};

void pageant_handle_msg(PageantClient *pc, PageantClientRequestId *reqid,
                        ptrlen msgpl)
{
    BinarySource msg[1];
    strbuf *sb = strbuf_new_nm();
    int type;

    BinarySource_BARE_INIT_PL(msg, msgpl);

    type = get_byte(msg);
    if (get_err(msg)) {
        failure(pc, reqid, sb, "message contained no type code");
        goto responded;
    }

    switch (type) {
      case SSH1_AGENTC_REQUEST_RSA_IDENTITIES:
        /*
         * Reply with SSH1_AGENT_RSA_IDENTITIES_ANSWER.
         */
        {
            pageant_client_log(pc, reqid,
                               "request: SSH1_AGENTC_REQUEST_RSA_IDENTITIES");

            put_byte(sb, SSH1_AGENT_RSA_IDENTITIES_ANSWER);
            pageant_make_keylist1(BinarySink_UPCAST(sb));

            pageant_client_log(pc, reqid,
                               "reply: SSH1_AGENT_RSA_IDENTITIES_ANSWER");
            if (!pc->suppress_logging) {
                int i;
                RSAKey *rkey;
                for (i = 0; NULL != (rkey = pageant_nth_ssh1_key(i)); i++) {
                    char *fingerprint = rsa_ssh1_fingerprint(rkey);
                    pageant_client_log(pc, reqid, "returned key: %s",
                                       fingerprint);
                    sfree(fingerprint);
                }
            }
        }
        break;
      case SSH2_AGENTC_REQUEST_IDENTITIES:
        /*
         * Reply with SSH2_AGENT_IDENTITIES_ANSWER.
         */
        {
            pageant_client_log(pc, reqid,
                               "request: SSH2_AGENTC_REQUEST_IDENTITIES");

            put_byte(sb, SSH2_AGENT_IDENTITIES_ANSWER);
            pageant_make_keylist2(BinarySink_UPCAST(sb));

            pageant_client_log(pc, reqid,
                               "reply: SSH2_AGENT_IDENTITIES_ANSWER");
            if (!pc->suppress_logging) {
                int i;
                ssh2_userkey *skey;
                for (i = 0; NULL != (skey = pageant_nth_ssh2_key(i)); i++) {
                    char *fingerprint = ssh2_fingerprint(skey->key);
                    pageant_client_log(pc, reqid, "returned key: %s %s",
                                       fingerprint, skey->comment);
                    sfree(fingerprint);
                }
            }
        }
        break;
      case SSH1_AGENTC_RSA_CHALLENGE:
        /*
         * Reply with either SSH1_AGENT_RSA_RESPONSE or
         * SSH_AGENT_FAILURE, depending on whether we have that key
         * or not.
         */
        {
            RSAKey reqkey;
            PageantKey *pk;
            mp_int *challenge, *response;
            ptrlen session_id;
            unsigned response_type;
            unsigned char response_md5[16];
            int i;

            pageant_client_log(pc, reqid,
                               "request: SSH1_AGENTC_RSA_CHALLENGE");

            response = NULL;
            memset(&reqkey, 0, sizeof(reqkey));

            get_rsa_ssh1_pub(msg, &reqkey, RSA_SSH1_EXPONENT_FIRST);
            challenge = get_mp_ssh1(msg);
            session_id = get_data(msg, 16);
            response_type = get_uint32(msg);

            if (get_err(msg)) {
                failure(pc, reqid, sb, "unable to decode request");
                goto challenge1_cleanup;
            }
            if (response_type != 1) {
                failure(pc, reqid, sb,
                        "response type other than 1 not supported");
                goto challenge1_cleanup;
            }

            if (!pc->suppress_logging) {
                char *fingerprint;
                reqkey.comment = NULL;
                fingerprint = rsa_ssh1_fingerprint(&reqkey);
                pageant_client_log(pc, reqid, "requested key: %s",
                                   fingerprint);
                sfree(fingerprint);
            }

            if ((pk = findkey1(&reqkey)) == NULL) {
                failure(pc, reqid, sb, "key not found");
                goto challenge1_cleanup;
            }
            response = rsa_ssh1_decrypt(challenge, pk->rkey);

            {
                ssh_hash *h = ssh_hash_new(&ssh_md5);
                for (i = 0; i < 32; i++)
                    put_byte(h, mp_get_byte(response, 31 - i));
                put_datapl(h, session_id);
                ssh_hash_final(h, response_md5);
            }

            put_byte(sb, SSH1_AGENT_RSA_RESPONSE);
            put_data(sb, response_md5, 16);

            pageant_client_log(pc, reqid, "reply: SSH1_AGENT_RSA_RESPONSE");

          challenge1_cleanup:
            if (response)
                mp_free(response);
            mp_free(challenge);
            freersakey(&reqkey);
        }
        break;
      case SSH2_AGENTC_SIGN_REQUEST:
        /*
         * Reply with either SSH2_AGENT_SIGN_RESPONSE or
         * SSH_AGENT_FAILURE, depending on whether we have that key
         * or not.
         */
        {
            PageantKey *pk;
            ptrlen keyblob, sigdata;
            strbuf *signature;
            uint32_t flags, supported_flags;

            pageant_client_log(pc, reqid, "request: SSH2_AGENTC_SIGN_REQUEST");

            keyblob = get_string(msg);
            sigdata = get_string(msg);

            if (get_err(msg)) {
                failure(pc, reqid, sb, "unable to decode request");
                goto responded;
            }

            /*
             * Later versions of the agent protocol added a flags word
             * on the end of the sign request. That hasn't always been
             * there, so we don't complain if we don't find it.
             *
             * get_uint32 will default to returning zero if no data is
             * available.
             */
            bool have_flags = false;
            flags = get_uint32(msg);
            if (!get_err(msg))
                have_flags = true;

            if (!pc->suppress_logging) {
                char *fingerprint = ssh2_fingerprint_blob(keyblob);
                pageant_client_log(pc, reqid, "requested key: %s",
                                   fingerprint);
                sfree(fingerprint);
            }
            if ((pk = findkey2(keyblob)) == NULL) {
                failure(pc, reqid, sb, "key not found");
                goto responded;
            }

            if (have_flags)
                pageant_client_log(pc, reqid, "signature flags = 0x%08"PRIx32,
                                   flags);
            else
                pageant_client_log(pc, reqid, "no signature flags");

            supported_flags = ssh_key_alg(pk->skey->key)->supported_flags;
            if (flags & ~supported_flags) {
                /*
                 * We MUST reject any message containing flags we
                 * don't understand.
                 */
                failure(pc, reqid, sb, "unsupported flag bits 0x%08"PRIx32,
                        flags & ~supported_flags);
                goto responded;
            }

            char *invalid = ssh_key_invalid(pk->skey->key, flags);
            if (invalid) {
                failure(pc, reqid, sb, "key invalid: %s", invalid);
                sfree(invalid);
                goto responded;
            }

            signature = strbuf_new();
            ssh_key_sign(pk->skey->key, sigdata, flags,
                         BinarySink_UPCAST(signature));

            put_byte(sb, SSH2_AGENT_SIGN_RESPONSE);
            put_stringsb(sb, signature);

            pageant_client_log(pc, reqid, "reply: SSH2_AGENT_SIGN_RESPONSE");
        }
        break;
      case SSH1_AGENTC_ADD_RSA_IDENTITY:
        /*
         * Add to the list and return SSH_AGENT_SUCCESS, or
         * SSH_AGENT_FAILURE if the key was malformed.
         */
        {
            RSAKey *key;

            pageant_client_log(pc, reqid,
                               "request: SSH1_AGENTC_ADD_RSA_IDENTITY");

            key = get_rsa_ssh1_priv_agent(msg);
            key->comment = mkstr(get_string(msg));

            if (get_err(msg)) {
                failure(pc, reqid, sb, "unable to decode request");
                goto add1_cleanup;
            }

            if (!rsa_verify(key)) {
                failure(pc, reqid, sb, "key is invalid");
                goto add1_cleanup;
            }

            if (!pc->suppress_logging) {
                char *fingerprint = rsa_ssh1_fingerprint(key);
                pageant_client_log(pc, reqid,
                                   "submitted key: %s", fingerprint);
                sfree(fingerprint);
            }

            if (pageant_add_ssh1_key(key)) {
                keylist_update();
                put_byte(sb, SSH_AGENT_SUCCESS);
                pageant_client_log(pc, reqid, "reply: SSH_AGENT_SUCCESS");
                key = NULL;            /* don't free it in cleanup */
            } else {
                failure(pc, reqid, sb, "key already present");
            }

          add1_cleanup:
            if (key) {
                freersakey(key);
                sfree(key);
            }
        }
        break;
      case SSH2_AGENTC_ADD_IDENTITY:
        /*
         * Add to the list and return SSH_AGENT_SUCCESS, or
         * SSH_AGENT_FAILURE if the key was malformed.
         */
        {
            ssh2_userkey *key = NULL;
            ptrlen algpl;
            const ssh_keyalg *alg;

            pageant_client_log(pc, reqid, "request: SSH2_AGENTC_ADD_IDENTITY");

            algpl = get_string(msg);

            key = snew(ssh2_userkey);
            key->key = NULL;
            key->comment = NULL;
            alg = find_pubkey_alg_len(algpl);
            if (!alg) {
                failure(pc, reqid, sb, "algorithm unknown");
                goto add2_cleanup;
            }

            key->key = ssh_key_new_priv_openssh(alg, msg);

            if (!key->key) {
                failure(pc, reqid, sb, "key setup failed");
                goto add2_cleanup;
            }

            key->comment = mkstr(get_string(msg));

            if (get_err(msg)) {
                failure(pc, reqid, sb, "unable to decode request");
                goto add2_cleanup;
            }

            if (!pc->suppress_logging) {
                char *fingerprint = ssh2_fingerprint(key->key);
                pageant_client_log(pc, reqid, "submitted key: %s %s",
                                   fingerprint, key->comment);
                sfree(fingerprint);
            }

            if (pageant_add_ssh2_key(key)) {
                keylist_update();
                put_byte(sb, SSH_AGENT_SUCCESS);

                pageant_client_log(pc, reqid, "reply: SSH_AGENT_SUCCESS");

                key = NULL;            /* don't clean it up */
            } else {
                failure(pc, reqid, sb, "key already present");
            }

          add2_cleanup:
            if (key) {
                if (key->key)
                    ssh_key_free(key->key);
                if (key->comment)
                    sfree(key->comment);
                sfree(key);
            }
        }
        break;
      case SSH1_AGENTC_REMOVE_RSA_IDENTITY:
        /*
         * Remove from the list and return SSH_AGENT_SUCCESS, or
         * perhaps SSH_AGENT_FAILURE if it wasn't in the list to
         * start with.
         */
        {
            RSAKey reqkey;
            PageantKey *pk;

            pageant_client_log(pc, reqid,
                               "request: SSH1_AGENTC_REMOVE_RSA_IDENTITY");

            memset(&reqkey, 0, sizeof(reqkey));
            get_rsa_ssh1_pub(msg, &reqkey, RSA_SSH1_EXPONENT_FIRST);

            if (get_err(msg)) {
                failure(pc, reqid, sb, "unable to decode request");
                freersakey(&reqkey);
                goto responded;
            }

            if (!pc->suppress_logging) {
                char *fingerprint;
                reqkey.comment = NULL;
                fingerprint = rsa_ssh1_fingerprint(&reqkey);
                pageant_client_log(pc, reqid, "unwanted key: %s", fingerprint);
                sfree(fingerprint);
            }

            pk = findkey1(&reqkey);
            freersakey(&reqkey);
            if (pk) {
                pageant_client_log(pc, reqid, "found with comment: %s",
                                   pk->rkey->comment);

                del234(keytree, pk);
                keylist_update();
                pk_free(pk);
                put_byte(sb, SSH_AGENT_SUCCESS);

                pageant_client_log(pc, reqid, "reply: SSH_AGENT_SUCCESS");
            } else {
                failure(pc, reqid, sb, "key not found");
            }
        }
        break;
      case SSH2_AGENTC_REMOVE_IDENTITY:
        /*
         * Remove from the list and return SSH_AGENT_SUCCESS, or
         * perhaps SSH_AGENT_FAILURE if it wasn't in the list to
         * start with.
         */
        {
            PageantKey *pk;
            ptrlen blob;

            pageant_client_log(pc, reqid,
                               "request: SSH2_AGENTC_REMOVE_IDENTITY");

            blob = get_string(msg);

            if (get_err(msg)) {
                failure(pc, reqid, sb, "unable to decode request");
                goto responded;
            }

            if (!pc->suppress_logging) {
                char *fingerprint = ssh2_fingerprint_blob(blob);
                pageant_client_log(pc, reqid, "unwanted key: %s", fingerprint);
                sfree(fingerprint);
            }

            pk = findkey2(blob);
            if (!pk) {
                failure(pc, reqid, sb, "key not found");
                goto responded;
            }

            pageant_client_log(pc, reqid,
                               "found with comment: %s", pk->skey->comment);

            del234(keytree, pk);
            keylist_update();
            pk_free(pk);
            put_byte(sb, SSH_AGENT_SUCCESS);

            pageant_client_log(pc, reqid, "reply: SSH_AGENT_SUCCESS");
        }
        break;
      case SSH1_AGENTC_REMOVE_ALL_RSA_IDENTITIES:
        /*
         * Remove all SSH-1 keys. Always returns success.
         */
        {
            pageant_client_log(pc, reqid, "request:"
                               " SSH1_AGENTC_REMOVE_ALL_RSA_IDENTITIES");

            remove_all_keys(1);
            keylist_update();

            put_byte(sb, SSH_AGENT_SUCCESS);

            pageant_client_log(pc, reqid, "reply: SSH_AGENT_SUCCESS");
        }
        break;
      case SSH2_AGENTC_REMOVE_ALL_IDENTITIES:
        /*
         * Remove all SSH-2 keys. Always returns success.
         */
        {
            pageant_client_log(pc, reqid,
                               "request: SSH2_AGENTC_REMOVE_ALL_IDENTITIES");

            remove_all_keys(2);
            keylist_update();

            put_byte(sb, SSH_AGENT_SUCCESS);

            pageant_client_log(pc, reqid, "reply: SSH_AGENT_SUCCESS");
        }
        break;
      default:
        pageant_client_log(pc, reqid, "request: unknown message type %d",
                           type);
        failure(pc, reqid, sb, "unrecognised message");
        break;
    }

  responded:;

    PageantImmOp *io = snew(PageantImmOp);
    io->pao.vt = &immop_vtable;
    io->pao.info = pc->info;
    io->pao.cr.prev = pc->info->head.prev;
    io->pao.cr.next = &pc->info->head;
    io->pao.reqid = reqid;
    io->response = sb;
    io->crLine = 0;
    queue_toplevel_callback(pageant_async_op_callback, &io->pao);
}

void pageant_init(void)
{
    pageant_local = true;
    keytree = newtree234(cmpkeys);
}

RSAKey *pageant_nth_ssh1_key(int i)
{
    PageantKey *pk = index234(keytree, find_first_key_for_version(1) + i);
    if (pk && pk->sort.ssh_version == 1)
        return pk->rkey;
    else
        return NULL;
}

ssh2_userkey *pageant_nth_ssh2_key(int i)
{
    PageantKey *pk = index234(keytree, find_first_key_for_version(2) + i);
    if (pk && pk->sort.ssh_version == 2)
        return pk->skey;
    else
        return NULL;
}

bool pageant_delete_ssh1_key(RSAKey *rkey)
{
    strbuf *blob = makeblob1(rkey);
    PageantKeySort sort = keysort(1, ptrlen_from_strbuf(blob));
    PageantKey *deleted = del234(keytree, &sort);
    strbuf_free(blob);

    if (!deleted)
        return false;
    assert(deleted->sort.ssh_version == 1);
    assert(deleted->rkey == rkey);
    return true;
}

bool pageant_delete_ssh2_key(ssh2_userkey *skey)
{
    strbuf *blob = makeblob2(skey);
    PageantKeySort sort = keysort(2, ptrlen_from_strbuf(blob));
    PageantKey *deleted = del234(keytree, &sort);
    strbuf_free(blob);

    if (!deleted)
        return false;
    assert(deleted->sort.ssh_version == 2);
    assert(deleted->skey == skey);
    return true;
}

/* ----------------------------------------------------------------------
 * The agent plug.
 */

/*
 * An extra coroutine macro, specific to this code which is consuming
 * 'const char *data'.
 */
#define crGetChar(c) do                                         \
    {                                                           \
        while (len == 0) {                                      \
            *crLine =__LINE__; return; case __LINE__:;          \
        }                                                       \
        len--;                                                  \
        (c) = (unsigned char)*data++;                           \
    } while (0)

struct pageant_conn_queued_response {
    struct pageant_conn_queued_response *next, *prev;
    size_t req_index;      /* for indexing requests in log messages */
    strbuf *sb;
    PageantClientRequestId reqid;
};

struct pageant_conn_state {
    Socket *connsock;
    PageantListenerClient *plc;
    unsigned char lenbuf[4], pktbuf[AGENT_MAX_MSGLEN];
    unsigned len, got;
    bool real_packet;
    size_t conn_index;     /* for indexing connections in log messages */
    size_t req_index;      /* for indexing requests in log messages */
    int crLine;            /* for coroutine in pageant_conn_receive */

    struct pageant_conn_queued_response response_queue;

    PageantClient pc;
    Plug plug;
};

static void pageant_conn_closing(Plug *plug, const char *error_msg,
                                 int error_code, bool calling_back)
{
    struct pageant_conn_state *pc = container_of(
        plug, struct pageant_conn_state, plug);
    if (error_msg)
        pageant_listener_client_log(pc->plc, "c#%zu: error: %s",
                                    pc->conn_index, error_msg);
    else
        pageant_listener_client_log(pc->plc, "c#%zu: connection closed",
                                    pc->conn_index);
    sk_close(pc->connsock);
    pageant_unregister_client(&pc->pc);
    sfree(pc);
}

static void pageant_conn_sent(Plug *plug, size_t bufsize)
{
    /* struct pageant_conn_state *pc = container_of(
        plug, struct pageant_conn_state, plug); */

    /*
     * We do nothing here, because we expect that there won't be a
     * need to throttle and unthrottle the connection to an agent -
     * clients will typically not send many requests, and will wait
     * until they receive each reply before sending a new request.
     */
}

static void pageant_conn_log(PageantClient *pc, PageantClientRequestId *reqid,
                             const char *fmt, va_list ap)
{
    struct pageant_conn_state *pcs =
        container_of(pc, struct pageant_conn_state, pc);
    struct pageant_conn_queued_response *qr =
        container_of(reqid, struct pageant_conn_queued_response, reqid);

    char *formatted = dupvprintf(fmt, ap);
    pageant_listener_client_log(pcs->plc, "c#%zu,r#%zu: %s",
                                pcs->conn_index, qr->req_index, formatted);
    sfree(formatted);
}

static void pageant_conn_got_response(
    PageantClient *pc, PageantClientRequestId *reqid, ptrlen response)
{
    struct pageant_conn_state *pcs =
        container_of(pc, struct pageant_conn_state, pc);
    struct pageant_conn_queued_response *qr =
        container_of(reqid, struct pageant_conn_queued_response, reqid);

    qr->sb = strbuf_new_nm();
    put_stringpl(qr->sb, response);

    while (pcs->response_queue.next != &pcs->response_queue &&
           pcs->response_queue.next->sb) {
        qr = pcs->response_queue.next;
        sk_write(pcs->connsock, qr->sb->u, qr->sb->len);
        qr->next->prev = qr->prev;
        qr->prev->next = qr->next;
        strbuf_free(qr->sb);
        sfree(qr);
    }
}

static const struct PageantClientVtable pageant_connection_clientvt = {
    pageant_conn_log,
    pageant_conn_got_response,
};

static void pageant_conn_receive(
    Plug *plug, int urgent, const char *data, size_t len)
{
    struct pageant_conn_state *pc = container_of(
        plug, struct pageant_conn_state, plug);
    char c;

    crBegin(pc->crLine);

    while (len > 0) {
        pc->got = 0;
        while (pc->got < 4) {
            crGetChar(c);
            pc->lenbuf[pc->got++] = c;
        }

        pc->len = GET_32BIT_MSB_FIRST(pc->lenbuf);
        pc->got = 0;
        pc->real_packet = (pc->len < AGENT_MAX_MSGLEN-4);

        {
            struct pageant_conn_queued_response *qr =
                snew(struct pageant_conn_queued_response);
            qr->prev = pc->response_queue.prev;
            qr->next = &pc->response_queue;
            qr->prev->next = qr->next->prev = qr;
            qr->sb = NULL;
            qr->req_index = pc->req_index++;
        }

        if (!pc->real_packet) {
            /*
             * Send failure immediately, before consuming the packet
             * data. That way we notify the client reasonably early
             * even if the data channel has just started spewing
             * nonsense.
             */
            pageant_client_log(&pc->pc, &pc->response_queue.prev->reqid,
                               "early reply: SSH_AGENT_FAILURE "
                               "(overlong message, length %u)", pc->len);
            static const unsigned char failure[] = { SSH_AGENT_FAILURE };
            pageant_conn_got_response(&pc->pc, &pc->response_queue.prev->reqid,
                                      make_ptrlen(failure, lenof(failure)));
        }

        while (pc->got < pc->len) {
            crGetChar(c);
            if (pc->real_packet)
                pc->pktbuf[pc->got] = c;
            pc->got++;
        }

        if (pc->real_packet)
            pageant_handle_msg(&pc->pc, &pc->response_queue.prev->reqid,
                               make_ptrlen(pc->pktbuf, pc->len));
    }

    crFinishV;
}

struct pageant_listen_state {
    Socket *listensock;
    PageantListenerClient *plc;
    size_t conn_index;     /* for indexing connections in log messages */

    Plug plug;
};

static void pageant_listen_closing(Plug *plug, const char *error_msg,
                                   int error_code, bool calling_back)
{
    struct pageant_listen_state *pl = container_of(
        plug, struct pageant_listen_state, plug);
    if (error_msg)
        pageant_listener_client_log(pl->plc, "listening socket: error: %s",
                                    error_msg);
    sk_close(pl->listensock);
    pl->listensock = NULL;
}

static const PlugVtable pageant_connection_plugvt = {
    NULL, /* no log function, because that's for outgoing connections */
    pageant_conn_closing,
    pageant_conn_receive,
    pageant_conn_sent,
    NULL /* no accepting function, because we've already done it */
};

static int pageant_listen_accepting(Plug *plug,
                                    accept_fn_t constructor, accept_ctx_t ctx)
{
    struct pageant_listen_state *pl = container_of(
        plug, struct pageant_listen_state, plug);
    struct pageant_conn_state *pc;
    const char *err;
    SocketPeerInfo *peerinfo;

    pc = snew(struct pageant_conn_state);
    pc->plug.vt = &pageant_connection_plugvt;
    pc->pc.vt = &pageant_connection_clientvt;
    pc->plc = pl->plc;
    pc->response_queue.next = pc->response_queue.prev = &pc->response_queue;
    pc->conn_index = pl->conn_index++;
    pc->req_index = 0;
    pc->crLine = 0;

    pc->connsock = constructor(ctx, &pc->plug);
    if ((err = sk_socket_error(pc->connsock)) != NULL) {
        sk_close(pc->connsock);
        sfree(pc);
        return 1;
    }

    sk_set_frozen(pc->connsock, 0);

    peerinfo = sk_peer_info(pc->connsock);
    if (peerinfo && peerinfo->log_text) {
        pageant_listener_client_log(pl->plc, "c#%zu: new connection from %s",
                                    pc->conn_index, peerinfo->log_text);
    } else {
        pageant_listener_client_log(pl->plc, "c#%zu: new connection",
                                    pc->conn_index);
    }
    sk_free_peer_info(peerinfo);

    pageant_register_client(&pc->pc);

    return 0;
}

static const PlugVtable pageant_listener_plugvt = {
    NULL, /* no log function, because that's for outgoing connections */
    pageant_listen_closing,
    NULL, /* no receive function on a listening socket */
    NULL, /* no sent function on a listening socket */
    pageant_listen_accepting
};

struct pageant_listen_state *pageant_listener_new(
    Plug **plug, PageantListenerClient *plc)
{
    struct pageant_listen_state *pl = snew(struct pageant_listen_state);
    pl->plug.vt = &pageant_listener_plugvt;
    pl->plc = plc;
    pl->listensock = NULL;
    pl->conn_index = 0;
    *plug = &pl->plug;
    return pl;
}

void pageant_listener_got_socket(struct pageant_listen_state *pl, Socket *sock)
{
    pl->listensock = sock;
}

void pageant_listener_free(struct pageant_listen_state *pl)
{
    if (pl->listensock)
        sk_close(pl->listensock);
    sfree(pl);
}

/* ----------------------------------------------------------------------
 * Code to perform agent operations either as a client, or within the
 * same process as the running agent.
 */

static tree234 *passphrases = NULL;

/*
 * After processing a list of filenames, we want to forget the
 * passphrases.
 */
void pageant_forget_passphrases(void)
{
    if (!passphrases)                  /* in case we never set it up at all */
        return;

    while (count234(passphrases) > 0) {
        char *pp = index234(passphrases, 0);
        smemclr(pp, strlen(pp));
        delpos234(passphrases, 0);
        sfree(pp);
    }
}

void *pageant_get_keylist1(int *length)
{
    void *ret;

    if (!pageant_local) {
        strbuf *request;
        unsigned char *response;
        void *vresponse;
        int resplen;

        request = strbuf_new_for_agent_query();
        put_byte(request, SSH1_AGENTC_REQUEST_RSA_IDENTITIES);
        agent_query_synchronous(request, &vresponse, &resplen);
        strbuf_free(request);

        response = vresponse;
        if (resplen < 5 || response[4] != SSH1_AGENT_RSA_IDENTITIES_ANSWER) {
            sfree(response);
            return NULL;
        }

        ret = snewn(resplen-5, unsigned char);
        memcpy(ret, response+5, resplen-5);
        sfree(response);

        if (length)
            *length = resplen-5;
    } else {
        strbuf *buf = strbuf_new();
        pageant_make_keylist1(BinarySink_UPCAST(buf));
        *length = buf->len;
        ret = strbuf_to_str(buf);
    }
    return ret;
}

void *pageant_get_keylist2(int *length)
{
    void *ret;

    if (!pageant_local) {
        strbuf *request;
        unsigned char *response;
        void *vresponse;
        int resplen;

        request = strbuf_new_for_agent_query();
        put_byte(request, SSH2_AGENTC_REQUEST_IDENTITIES);
        agent_query_synchronous(request, &vresponse, &resplen);
        strbuf_free(request);

        response = vresponse;
        if (resplen < 5 || response[4] != SSH2_AGENT_IDENTITIES_ANSWER) {
            sfree(response);
            return NULL;
        }

        ret = snewn(resplen-5, unsigned char);
        memcpy(ret, response+5, resplen-5);
        sfree(response);

        if (length)
            *length = resplen-5;
    } else {
        strbuf *buf = strbuf_new();
        pageant_make_keylist2(BinarySink_UPCAST(buf));
        *length = buf->len;
        ret = strbuf_to_str(buf);
    }
    return ret;
}

int pageant_add_keyfile(Filename *filename, const char *passphrase,
                        char **retstr)
{
    RSAKey *rkey = NULL;
    ssh2_userkey *skey = NULL;
    bool needs_pass;
    int ret;
    int attempts;
    char *comment;
    const char *this_passphrase;
    const char *error = NULL;
    int type;

    if (!passphrases) {
        passphrases = newtree234(NULL);
    }

    *retstr = NULL;

    type = key_type(filename);
    if (type != SSH_KEYTYPE_SSH1 && type != SSH_KEYTYPE_SSH2) {
        *retstr = dupprintf("Couldn't load this key (%s)",
                            key_type_to_str(type));
        return PAGEANT_ACTION_FAILURE;
    }

    /*
     * See if the key is already loaded (in the primary Pageant,
     * which may or may not be us).
     */
    {
        strbuf *blob = strbuf_new();
        unsigned char *keylist, *p;
        int i, nkeys, keylistlen;

        if (type == SSH_KEYTYPE_SSH1) {
            if (!rsa1_loadpub_f(filename, BinarySink_UPCAST(blob),
                                NULL, &error)) {
                *retstr = dupprintf("Couldn't load private key (%s)", error);
                strbuf_free(blob);
                return PAGEANT_ACTION_FAILURE;
            }
            keylist = pageant_get_keylist1(&keylistlen);
        } else {
            /* For our purposes we want the blob prefixed with its
             * length, so add a placeholder here to fill in
             * afterwards */
            put_uint32(blob, 0);
            if (!ppk_loadpub_f(filename, NULL, BinarySink_UPCAST(blob),
                               NULL, &error)) {
                *retstr = dupprintf("Couldn't load private key (%s)", error);
                strbuf_free(blob);
                return PAGEANT_ACTION_FAILURE;
            }
            PUT_32BIT_MSB_FIRST(blob->s, blob->len - 4);
            keylist = pageant_get_keylist2(&keylistlen);
        }
        if (keylist) {
            if (keylistlen < 4) {
                *retstr = dupstr("Received broken key list from agent");
                sfree(keylist);
                strbuf_free(blob);
                return PAGEANT_ACTION_FAILURE;
            }
            nkeys = toint(GET_32BIT_MSB_FIRST(keylist));
            if (nkeys < 0) {
                *retstr = dupstr("Received broken key list from agent");
                sfree(keylist);
                strbuf_free(blob);
                return PAGEANT_ACTION_FAILURE;
            }
            p = keylist + 4;
            keylistlen -= 4;

            for (i = 0; i < nkeys; i++) {
                if (!memcmp(blob->s, p, blob->len)) {
                    /* Key is already present; we can now leave. */
                    sfree(keylist);
                    strbuf_free(blob);
                    return PAGEANT_ACTION_OK;
                }
                /* Now skip over public blob */
                if (type == SSH_KEYTYPE_SSH1) {
                    int n = rsa_ssh1_public_blob_len(
                        make_ptrlen(p, keylistlen));
                    if (n < 0) {
                        *retstr = dupstr("Received broken key list from agent");
                        sfree(keylist);
                        strbuf_free(blob);
                        return PAGEANT_ACTION_FAILURE;
                    }
                    p += n;
                    keylistlen -= n;
                } else {
                    int n;
                    if (keylistlen < 4) {
                        *retstr = dupstr("Received broken key list from agent");
                        sfree(keylist);
                        strbuf_free(blob);
                        return PAGEANT_ACTION_FAILURE;
                    }
                    n = GET_32BIT_MSB_FIRST(p);
                    p += 4;
                    keylistlen -= 4;

                    if (n < 0 || n > keylistlen) {
                        *retstr = dupstr("Received broken key list from agent");
                        sfree(keylist);
                        strbuf_free(blob);
                        return PAGEANT_ACTION_FAILURE;
                    }
                    p += n;
                    keylistlen -= n;
                }
                /* Now skip over comment field */
                {
                    int n;
                    if (keylistlen < 4) {
                        *retstr = dupstr("Received broken key list from agent");
                        sfree(keylist);
                        strbuf_free(blob);
                        return PAGEANT_ACTION_FAILURE;
                    }
                    n = GET_32BIT_MSB_FIRST(p);
                    p += 4;
                    keylistlen -= 4;

                    if (n < 0 || n > keylistlen) {
                        *retstr = dupstr("Received broken key list from agent");
                        sfree(keylist);
                        strbuf_free(blob);
                        return PAGEANT_ACTION_FAILURE;
                    }
                    p += n;
                    keylistlen -= n;
                }
            }

            sfree(keylist);
        }

        strbuf_free(blob);
    }

    error = NULL;
    if (type == SSH_KEYTYPE_SSH1)
        needs_pass = rsa1_encrypted_f(filename, &comment);
    else
        needs_pass = ppk_encrypted_f(filename, &comment);
    attempts = 0;
    if (type == SSH_KEYTYPE_SSH1)
        rkey = snew(RSAKey);

    /*
     * Loop round repeatedly trying to load the key, until we either
     * succeed, fail for some serious reason, or run out of
     * passphrases to try.
     */
    while (1) {
        if (needs_pass) {

            /*
             * If we've been given a passphrase on input, try using
             * it. Otherwise, try one from our tree234 of previously
             * useful passphrases.
             */
            if (passphrase) {
                this_passphrase = (attempts == 0 ? passphrase : NULL);
            } else {
                this_passphrase = (const char *)index234(passphrases, attempts);
            }

            if (!this_passphrase) {
                /*
                 * Run out of passphrases to try.
                 */
                *retstr = comment;
                sfree(rkey);
                return PAGEANT_ACTION_NEED_PP;
            }
        } else
            this_passphrase = "";

        if (type == SSH_KEYTYPE_SSH1)
            ret = rsa1_load_f(filename, rkey, this_passphrase, &error);
        else {
            skey = ppk_load_f(filename, this_passphrase, &error);
            if (skey == SSH2_WRONG_PASSPHRASE)
                ret = -1;
            else if (!skey)
                ret = 0;
            else
                ret = 1;
        }

        if (ret == 0) {
            /*
             * Failed to load the key file, for some reason other than
             * a bad passphrase.
             */
            *retstr = dupstr(error);
            sfree(rkey);
            if (comment)
                sfree(comment);
            return PAGEANT_ACTION_FAILURE;
        } else if (ret == 1) {
            /*
             * Successfully loaded the key file.
             */
            break;
        } else {
            /*
             * Passphrase wasn't right; go round again.
             */
            attempts++;
        }
    }

    /*
     * If we get here, we've successfully loaded the key into
     * rkey/skey, but not yet added it to the agent.
     */

    /*
     * If the key was successfully decrypted, save the passphrase for
     * use with other keys we try to load.
     */
    {
        char *pp_copy = dupstr(this_passphrase);
        if (addpos234(passphrases, pp_copy, 0) != pp_copy) {
            /* No need; it was already there. */
            smemclr(pp_copy, strlen(pp_copy));
            sfree(pp_copy);
        }
    }

    if (comment)
        sfree(comment);

    if (type == SSH_KEYTYPE_SSH1) {
        if (!pageant_local) {
            strbuf *request;
            unsigned char *response;
            void *vresponse;
            int resplen;

            request = strbuf_new_for_agent_query();
            put_byte(request, SSH1_AGENTC_ADD_RSA_IDENTITY);
            rsa_ssh1_private_blob_agent(BinarySink_UPCAST(request), rkey);
            put_stringz(request, rkey->comment);
            agent_query_synchronous(request, &vresponse, &resplen);
            strbuf_free(request);

            response = vresponse;
            if (resplen < 5 || response[4] != SSH_AGENT_SUCCESS) {
                *retstr = dupstr("The already running Pageant "
                                 "refused to add the key.");
                freersakey(rkey);
                sfree(rkey);
                sfree(response);
                return PAGEANT_ACTION_FAILURE;
            }
            freersakey(rkey);
            sfree(rkey);
            sfree(response);
        } else {
            if (!pageant_add_ssh1_key(rkey)) {
                freersakey(rkey);
                sfree(rkey);           /* already present, don't waste RAM */
            }
        }
    } else {
        if (!pageant_local) {
            strbuf *request;
            unsigned char *response;
            void *vresponse;
            int resplen;

            request = strbuf_new_for_agent_query();
            put_byte(request, SSH2_AGENTC_ADD_IDENTITY);
            put_stringz(request, ssh_key_ssh_id(skey->key));
            ssh_key_openssh_blob(skey->key, BinarySink_UPCAST(request));
            put_stringz(request, skey->comment);
            agent_query_synchronous(request, &vresponse, &resplen);
            strbuf_free(request);

            response = vresponse;
            if (resplen < 5 || response[4] != SSH_AGENT_SUCCESS) {
                *retstr = dupstr("The already running Pageant "
                                 "refused to add the key.");
                sfree(response);
                return PAGEANT_ACTION_FAILURE;
            }

            ssh_key_free(skey->key);
            sfree(skey);
            sfree(response);
        } else {
            if (!pageant_add_ssh2_key(skey)) {
                ssh_key_free(skey->key);
                sfree(skey);           /* already present, don't waste RAM */
            }
        }
    }
    return PAGEANT_ACTION_OK;
}

int pageant_enum_keys(pageant_key_enum_fn_t callback, void *callback_ctx,
                      char **retstr)
{
    unsigned char *keylist;
    int i, nkeys, keylistlen;
    ptrlen comment;
    struct pageant_pubkey cbkey;
    BinarySource src[1];

    keylist = pageant_get_keylist1(&keylistlen);
    if (!keylist) {
        *retstr = dupstr("Did not receive an SSH-1 key list from agent");
        return PAGEANT_ACTION_FAILURE;
    }
    BinarySource_BARE_INIT(src, keylist, keylistlen);

    nkeys = toint(get_uint32(src));
    for (i = 0; i < nkeys; i++) {
        RSAKey rkey;
        char *fingerprint;

        /* public blob and fingerprint */
        memset(&rkey, 0, sizeof(rkey));
        get_rsa_ssh1_pub(src, &rkey, RSA_SSH1_EXPONENT_FIRST);
        comment = get_string(src);

        if (get_err(src)) {
            *retstr = dupstr("Received broken SSH-1 key list from agent");
            freersakey(&rkey);
            sfree(keylist);
            return PAGEANT_ACTION_FAILURE;
        }

        fingerprint = rsa_ssh1_fingerprint(&rkey);

        cbkey.blob = makeblob1(&rkey);
        cbkey.comment = mkstr(comment);
        cbkey.ssh_version = 1;
        callback(callback_ctx, fingerprint, cbkey.comment, &cbkey);
        strbuf_free(cbkey.blob);
        freersakey(&rkey);
        sfree(cbkey.comment);
        sfree(fingerprint);
    }

    sfree(keylist);

    if (get_err(src) || get_avail(src) != 0) {
        *retstr = dupstr("Received broken SSH-1 key list from agent");
        return PAGEANT_ACTION_FAILURE;
    }

    keylist = pageant_get_keylist2(&keylistlen);
    if (!keylist) {
        *retstr = dupstr("Did not receive an SSH-2 key list from agent");
        return PAGEANT_ACTION_FAILURE;
    }
    BinarySource_BARE_INIT(src, keylist, keylistlen);

    nkeys = toint(get_uint32(src));
    for (i = 0; i < nkeys; i++) {
        ptrlen pubblob;
        char *fingerprint;

        pubblob = get_string(src);
        comment = get_string(src);

        if (get_err(src)) {
            *retstr = dupstr("Received broken SSH-2 key list from agent");
            sfree(keylist);
            return PAGEANT_ACTION_FAILURE;
        }

        fingerprint = ssh2_fingerprint_blob(pubblob);
        cbkey.blob = strbuf_new();
        put_datapl(cbkey.blob, pubblob);

        cbkey.ssh_version = 2;
        cbkey.comment = mkstr(comment);
        callback(callback_ctx, fingerprint, cbkey.comment, &cbkey);
        sfree(fingerprint);
        sfree(cbkey.comment);
    }

    sfree(keylist);

    if (get_err(src) || get_avail(src) != 0) {
        *retstr = dupstr("Received broken SSH-2 key list from agent");
        return PAGEANT_ACTION_FAILURE;
    }

    return PAGEANT_ACTION_OK;
}

int pageant_delete_key(struct pageant_pubkey *key, char **retstr)
{
    strbuf *request;
    unsigned char *response;
    int resplen, ret;
    void *vresponse;

    request = strbuf_new_for_agent_query();

    if (key->ssh_version == 1) {
        put_byte(request, SSH1_AGENTC_REMOVE_RSA_IDENTITY);
        put_data(request, key->blob->s, key->blob->len);
    } else {
        put_byte(request, SSH2_AGENTC_REMOVE_IDENTITY);
        put_string(request, key->blob->s, key->blob->len);
    }

    agent_query_synchronous(request, &vresponse, &resplen);
    strbuf_free(request);

    response = vresponse;
    if (resplen < 5 || response[4] != SSH_AGENT_SUCCESS) {
        *retstr = dupstr("Agent failed to delete key");
        ret = PAGEANT_ACTION_FAILURE;
    } else {
        *retstr = NULL;
        ret = PAGEANT_ACTION_OK;
    }
    sfree(response);
    return ret;
}

int pageant_delete_all_keys(char **retstr)
{
    strbuf *request;
    unsigned char *response;
    int resplen;
    bool success;
    void *vresponse;

    request = strbuf_new_for_agent_query();
    put_byte(request, SSH2_AGENTC_REMOVE_ALL_IDENTITIES);
    agent_query_synchronous(request, &vresponse, &resplen);
    strbuf_free(request);
    response = vresponse;
    success = (resplen >= 4 && response[4] == SSH_AGENT_SUCCESS);
    sfree(response);
    if (!success) {
        *retstr = dupstr("Agent failed to delete SSH-2 keys");
        return PAGEANT_ACTION_FAILURE;
    }

    request = strbuf_new_for_agent_query();
    put_byte(request, SSH1_AGENTC_REMOVE_ALL_RSA_IDENTITIES);
    agent_query_synchronous(request, &vresponse, &resplen);
    strbuf_free(request);
    response = vresponse;
    success = (resplen >= 4 && response[4] == SSH_AGENT_SUCCESS);
    sfree(response);
    if (!success) {
        *retstr = dupstr("Agent failed to delete SSH-1 keys");
        return PAGEANT_ACTION_FAILURE;
    }

    *retstr = NULL;
    return PAGEANT_ACTION_OK;
}

struct pageant_pubkey *pageant_pubkey_copy(struct pageant_pubkey *key)
{
    struct pageant_pubkey *ret = snew(struct pageant_pubkey);
    ret->blob = strbuf_new();
    put_data(ret->blob, key->blob->s, key->blob->len);
    ret->comment = key->comment ? dupstr(key->comment) : NULL;
    ret->ssh_version = key->ssh_version;
    return ret;
}

void pageant_pubkey_free(struct pageant_pubkey *key)
{
    sfree(key->comment);
    strbuf_free(key->blob);
    sfree(key);
}
