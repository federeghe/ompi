/*
 * Copyright (c) 2015-2016 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dirent.h>
#include <errno.h>

#include <src/include/pmix_config.h>
#include <pmix_server.h>
#include <pmix_common.h>
#include "src/include/pmix_globals.h"

#include "src/class/pmix_value_array.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/buffer_ops/types.h"
#include "src/util/pmix_environ.h"
#include "src/util/hash.h"
#include "src/util/error.h"
#include "src/sm/pmix_sm.h"

#include "pmix_dstore.h"
#include "pmix_esh.h"

static int _esh_init(pmix_info_t info[], size_t ninfo);
static int _esh_finalize(void);
static int _esh_store(const char *nspace, pmix_rank_t rank, pmix_kval_t *kv);
static int _esh_fetch(const char *nspace, pmix_rank_t rank, const char *key, pmix_value_t **kvs);
static int _esh_patch_env(const char *nspace, char ***env);
static int _esh_nspace_add(const char *nspace, pmix_info_t info[], size_t ninfo);
static int _esh_nspace_del(const char *nspace);

pmix_dstore_base_module_t pmix_dstore_esh_module = {
    "esh",
    _esh_init,
    _esh_finalize,
    _esh_store,
    _esh_fetch,
    _esh_patch_env,
    _esh_nspace_add,
    _esh_nspace_del
};

#define ESH_REGION_EXTENSION        "EXTENSION_SLOT"
#define ESH_REGION_INVALIDATED      "INVALIDATED"
#define ESH_ENV_INITIAL_SEG_SIZE    "INITIAL_SEG_SIZE"
#define ESH_ENV_NS_META_SEG_SIZE    "NS_META_SEG_SIZE"
#define ESH_ENV_NS_DATA_SEG_SIZE    "NS_DATA_SEG_SIZE"
#define ESH_ENV_LINEAR              "SM_USE_LINEAR_SEARCH"

#define EXT_SLOT_SIZE (PMIX_MAX_KEYLEN + 1 + 2*sizeof(size_t)) /* in ext slot new offset will be stored in case if new data were added for the same process during next commit */
#define KVAL_SIZE(size) (PMIX_MAX_KEYLEN + 1 + sizeof(size_t) + size)

#define _ESH_LOCK(lockfd, operation)                        \
__extension__ ({                                            \
    pmix_status_t ret = PMIX_SUCCESS;                       \
    int i;                                                  \
    struct flock fl = {0};                                  \
    fl.l_type = operation;                                  \
    fl.l_whence = SEEK_SET;                                 \
    for(i = 0; i < 10; i++) {                               \
        if( 0 > fcntl(lockfd, F_SETLKW, &fl) ) {            \
            switch( errno ){                                \
                case EINTR:                                 \
                    continue;                               \
                case ENOENT:                                \
                case EINVAL:                                \
                    ret = PMIX_ERR_NOT_FOUND;               \
                    break;                                  \
                case EBADF:                                 \
                    ret = PMIX_ERR_BAD_PARAM;               \
                    break;                                  \
                case EDEADLK:                               \
                case EFAULT:                                \
                case ENOLCK:                                \
                    ret = PMIX_ERR_RESOURCE_BUSY;           \
                    break;                                  \
                default:                                    \
                    ret = PMIX_ERROR;                       \
                    break;                                  \
            }                                               \
        }                                                   \
        break;                                              \
    }                                                       \
    if (ret) {                                              \
        pmix_output(0, "%s %d:%s lock failed: %s",          \
            __FILE__, __LINE__, __func__, strerror(errno)); \
    }                                                       \
    ret;                                                    \
})

#define _ESH_WRLOCK(lockfd) _ESH_LOCK(lockfd, F_WRLCK)
#define _ESH_RDLOCK(lockfd) _ESH_LOCK(lockfd, F_RDLCK)
#define _ESH_UNLOCK(lockfd) _ESH_LOCK(lockfd, F_UNLCK)

#define ESH_INIT_SESSION_TBL_SIZE 2
#define ESH_INIT_NS_MAP_TBL_SIZE  2

static int _store_data_for_rank(ns_track_elem_t *ns_info, pmix_rank_t rank, pmix_buffer_t *buf);
static seg_desc_t *_create_new_segment(segment_type type, const ns_map_data_t *ns_map, uint32_t id);
static seg_desc_t *_attach_new_segment(segment_type type, const ns_map_data_t *ns_map, uint32_t id);
static int _update_ns_elem(ns_track_elem_t *ns_elem, ns_seg_info_t *info);
static int _put_ns_info_to_initial_segment(const ns_map_data_t *ns_map, pmix_sm_seg_t *metaseg, pmix_sm_seg_t *dataseg);
static ns_seg_info_t *_get_ns_info_from_initial_segment(const ns_map_data_t *ns_map);
static ns_track_elem_t *_get_track_elem_for_namespace(ns_map_data_t *ns_map);
static rank_meta_info *_get_rank_meta_info(pmix_rank_t rank, seg_desc_t *segdesc);
static uint8_t *_get_data_region_by_offset(seg_desc_t *segdesc, size_t offset);
static void _update_initial_segment_info(const ns_map_data_t *ns_map);
static void _set_constants_from_env(void);
static void _delete_sm_desc(seg_desc_t *desc);
static int _pmix_getpagesize(void);
static inline uint32_t _get_univ_size(const char *nspace);

static inline ns_map_data_t * _esh_session_map_search_server(const char *nspace);
static inline ns_map_data_t * _esh_session_map_search_client(const char *nspace);
static inline ns_map_data_t * _esh_session_map(const char *nspace, size_t tbl_idx);
static inline void _esh_session_map_clean(ns_map_t *m);
static inline int _esh_jobuid_tbl_search(uid_t jobuid, size_t *tbl_idx);
static inline int _esh_session_tbl_add(size_t *tbl_idx);
static inline int _esh_session_init(size_t idx, ns_map_data_t *m, size_t jobuid, int setjobuid);
static inline void _esh_session_release(session_t *s);
static inline void _esh_ns_track_cleanup(void);
static inline void _esh_sessions_cleanup(void);
static inline void _esh_ns_map_cleanup(void);
static inline int _esh_dir_del(const char *dirname);

static char *_base_path = NULL;
static size_t _initial_segment_size = 0;
static size_t _max_ns_num;
static size_t _meta_segment_size = 0;
static size_t _max_meta_elems;
static size_t _data_segment_size = 0;
static uid_t _jobuid;
static char _setjobuid = 0;

static pmix_value_array_t *_session_array = NULL;
static pmix_value_array_t *_ns_map_array = NULL;
static pmix_value_array_t *_ns_track_array = NULL;

ns_map_data_t * (*_esh_session_map_search)(const char *nspace) = NULL;

#define _ESH_SESSION_path(tbl_idx)         (PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t)[tbl_idx].nspace_path)
#define _ESH_SESSION_lockfile(tbl_idx)     (PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t)[tbl_idx].lockfile)
#define _ESH_SESSION_jobuid(tbl_idx)       (PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t)[tbl_idx].jobuid)
#define _ESH_SESSION_lockfd(tbl_idx)       (PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t)[tbl_idx].lockfd)
#define _ESH_SESSION_sm_seg_first(tbl_idx) (PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t)[tbl_idx].sm_seg_first)
#define _ESH_SESSION_sm_seg_last(tbl_idx)  (PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t)[tbl_idx].sm_seg_last)
#define _ESH_SESSION_ns_info(tbl_idx)      (PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t)[tbl_idx].ns_info)

/* If _direct_mode is set, it means that we use linear search
 * along the array of rank meta info objects inside a meta segment
 * to find the requested rank. Otherwise,  we do a fast lookup
 * based on rank and directly compute offset.
 * This mode is called direct because it's effectively used in
 * sparse communication patterns when direct modex is usually used.
 */
static int _direct_mode = 0;

static void ncon(ns_track_elem_t *p) {
    memset(&p->ns_map, 0, sizeof(p->ns_map));
    p->meta_seg = NULL;
    p->data_seg = NULL;
    p->num_meta_seg = 0;
    p->num_data_seg = 0;
}

static void ndes(ns_track_elem_t *p) {
    _delete_sm_desc(p->meta_seg);
    _delete_sm_desc(p->data_seg);
    memset(&p->ns_map, 0, sizeof(p->ns_map));
}

PMIX_CLASS_INSTANCE(ns_track_elem_t,
                    pmix_value_array_t,
                    ncon, ndes);

static inline void _esh_session_map_clean(ns_map_t *m) {
    memset(m, 0, sizeof(*m));
    m->data.track_idx = -1;
}

static inline const char *_unique_id(void)
{
    static const char *str = NULL;
    if (!str) {
        /* see: pmix_server.c initialize_server_base()
         * to get format of uri
         */
        if (PMIX_PROC_SERVER == pmix_globals.proc_type) {
            static char buf[100];
            snprintf(buf, sizeof(buf) - 1, "pmix-%d", getpid());
            str = buf;
        } else {
            str = getenv("PMIX_SERVER_URI");
            if (str) {
                str = strrchr(str, '/');
            }
            str = (str ? str + 1 : "$$$");
        }
    }
    return str;
}

static inline int _esh_dir_del(const char *path)
{
    DIR *dir;
    struct dirent *d_ptr;
    struct stat st;

    char name[PMIX_PATH_MAX];

    dir = opendir(path);
    if (dir != NULL) {
        while (NULL != (d_ptr = readdir(dir))) {
            snprintf(name, PMIX_PATH_MAX, "%s/%s", path, d_ptr->d_name);

            if (lstat(name, &st) < 0){
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }

            if(S_ISDIR(st.st_mode)) {
                if(strcmp(d_ptr->d_name, ".") && strcmp(d_ptr->d_name, "..")) {
                    _esh_dir_del(name);
                }
            }
            else {
                unlink(name);
            }
        }
        closedir(dir);
    }
    else {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    return rmdir(path);
}

static inline int _esh_tbls_init(void)
{
    pmix_status_t rc = PMIX_SUCCESS;
    size_t idx;

    if (NULL == (_ns_track_array = PMIX_NEW(pmix_value_array_t))) {
        rc = PMIX_ERR_OUT_OF_RESOURCE;
        PMIX_ERROR_LOG(rc);
        goto err_exit;
    }
    if (PMIX_SUCCESS != (rc = pmix_value_array_init(_ns_track_array, sizeof(ns_track_elem_t)))){
        PMIX_ERROR_LOG(rc);
        goto err_exit;
    }

    if (NULL == (_session_array = PMIX_NEW(pmix_value_array_t))){
            rc = PMIX_ERR_OUT_OF_RESOURCE;
    }
    if (PMIX_SUCCESS != (rc = pmix_value_array_init(_session_array, sizeof(session_t)))) {
        goto err_exit;
    }
    if (PMIX_SUCCESS != (rc = pmix_value_array_set_size(_session_array, ESH_INIT_SESSION_TBL_SIZE))) {
        goto err_exit;
    }
    for (idx = 0; idx < ESH_INIT_SESSION_TBL_SIZE; idx++) {
        memset(pmix_value_array_get_item(_session_array, idx), 0, sizeof(session_t));
    }

    if (NULL == (_ns_map_array = PMIX_NEW(pmix_value_array_t))) {
        rc = PMIX_ERR_OUT_OF_RESOURCE;
    }
    if (PMIX_SUCCESS != (rc = pmix_value_array_init(_ns_map_array, sizeof(ns_map_t)))) {
        goto err_exit;
    }
    if (PMIX_SUCCESS != (rc = pmix_value_array_set_size(_ns_map_array, ESH_INIT_NS_MAP_TBL_SIZE))) {
        goto err_exit;
    }
    for (idx = 0; idx < ESH_INIT_NS_MAP_TBL_SIZE; idx++) {
        _esh_session_map_clean(pmix_value_array_get_item(_ns_map_array, idx));
    }


err_exit:
    return rc;
}

static inline void _esh_ns_map_cleanup(void)
{
    size_t idx;
    size_t size;
    ns_map_t *ns_map;

    if (NULL == _ns_map_array) {
        return;
    }

    size = pmix_value_array_get_size(_ns_map_array);
    ns_map = PMIX_VALUE_ARRAY_GET_BASE(_ns_map_array, ns_map_t);

    for (idx = 0; idx < size; idx++) {
        if(ns_map[idx].in_use)
            _esh_session_map_clean(&ns_map[idx]);
    }

    PMIX_RELEASE(_ns_map_array);
    _ns_map_array = NULL;
}

static inline void _esh_sessions_cleanup(void)
{
    size_t idx;
    size_t size;
    session_t *s_tbl;

    if (NULL == _session_array) {
        return;
    }

    size = pmix_value_array_get_size(_session_array);
    s_tbl = PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t);

    for (idx = 0; idx < size; idx++) {
        if(s_tbl[idx].in_use)
            _esh_session_release(&s_tbl[idx]);
    }

    PMIX_RELEASE(_session_array);
    _session_array = NULL;
}

static inline void _esh_ns_track_cleanup(void)
{
    if (NULL == _ns_track_array) {
        return;
    }

    PMIX_RELEASE(_ns_track_array);
    _ns_track_array = NULL;
}

static inline ns_map_data_t * _esh_session_map(const char *nspace, size_t tbl_idx)
{
    size_t map_idx;
    size_t size = pmix_value_array_get_size(_ns_map_array);;
    ns_map_t *ns_map = PMIX_VALUE_ARRAY_GET_BASE(_ns_map_array, ns_map_t);;
    ns_map_t *new_map = NULL;

    if (NULL == nspace) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    for(map_idx = 0; map_idx < size; map_idx++) {
        if (!ns_map[map_idx].in_use) {
            ns_map[map_idx].in_use = true;
            strncpy(ns_map[map_idx].data.name, nspace, sizeof(ns_map[map_idx].data.name)-1);
            ns_map[map_idx].data.tbl_idx = tbl_idx;
            return  &ns_map[map_idx].data;
        }
    }

    if (NULL == (new_map = pmix_value_array_get_item(_ns_map_array, map_idx))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return NULL;
    }

    _esh_session_map_clean(new_map);
    new_map->in_use = true;
    new_map->data.tbl_idx = tbl_idx;
    strncpy(new_map->data.name, nspace, sizeof(new_map->data.name)-1);

    return  &new_map->data;
}

static inline int _esh_jobuid_tbl_search(uid_t jobuid, size_t *tbl_idx)
{
    size_t idx, size;
    session_t *session_tbl = NULL;

    size = pmix_value_array_get_size(_session_array);
    session_tbl = PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t);

    for(idx = 0; idx < size; idx++) {
        if (session_tbl[idx].in_use && session_tbl[idx].jobuid == jobuid) {
            *tbl_idx = idx;
            return PMIX_SUCCESS;
        }
    }
    return PMIX_ERROR;
}

static inline int _esh_session_tbl_add(size_t *tbl_idx)
{
    size_t idx;
    size_t size = pmix_value_array_get_size(_session_array);
    session_t *s_tbl = PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t);
    session_t *new_sesion;

    for(idx = 0; idx < size; idx ++) {
        if (0 == s_tbl[idx].in_use) {
            s_tbl[idx].in_use = 1;
            *tbl_idx = idx;
            return PMIX_SUCCESS;
        }
    }

    if (NULL == (new_sesion = pmix_value_array_get_item(_session_array, idx))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    s_tbl[idx].in_use = 1;
    *tbl_idx = idx;

    return PMIX_SUCCESS;
}

static inline ns_map_data_t * _esh_session_map_search_server(const char *nspace)
{
    size_t idx, size = pmix_value_array_get_size(_ns_map_array);
    ns_map_t *ns_map = PMIX_VALUE_ARRAY_GET_BASE(_ns_map_array, ns_map_t);
    if (NULL == nspace) {
        return NULL;
    }

    for (idx = 0; idx < size; idx++) {
        if (ns_map[idx].in_use &&
            (0 == strcmp(ns_map[idx].data.name, nspace))) {
                return &ns_map[idx].data;
        }
    }
    return NULL;
}

static inline ns_map_data_t * _esh_session_map_search_client(const char *nspace)
{
    size_t idx, size = pmix_value_array_get_size(_ns_map_array);
    ns_map_t *ns_map = PMIX_VALUE_ARRAY_GET_BASE(_ns_map_array, ns_map_t);

    if (NULL == nspace) {
        return NULL;
    }

    for (idx = 0; idx < size; idx++) {
        if (ns_map[idx].in_use &&
            (0 == strcmp(ns_map[idx].data.name, nspace))) {
                return &ns_map[idx].data;
        }
    }
    return _esh_session_map(nspace, 0);
}

static inline int _esh_session_init(size_t idx, ns_map_data_t *m, size_t jobuid, int setjobuid)
{
    struct stat st = {0};
    seg_desc_t *seg = NULL;
    session_t *s = &(PMIX_VALUE_ARRAY_GET_ITEM(_session_array, session_t, idx));

    if (NULL == s) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    s->jobuid = jobuid;
    s->nspace_path = strdup(_base_path);

    /* create a lock file to prevent clients from reading while server is writing to the shared memory.
    * This situation is quite often, especially in case of direct modex when clients might ask for data
    * simultaneously.*/
    if(0 > asprintf(&s->lockfile, "%s/dstore_sm.lock", s->nspace_path)) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
        "%s:%d:%s _lockfile_name: %s", __FILE__, __LINE__, __func__, s->lockfile));

    if (PMIX_PROC_SERVER == pmix_globals.proc_type) {
        if (stat(s->nspace_path, &st) == -1){
            if (0 != mkdir(s->nspace_path, 0770)) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
        }
        s->lockfd = open(s->lockfile, O_CREAT | O_RDWR | O_EXCL, 0600);

        /* if previous launch was crashed, the lockfile might not be deleted and unlocked,
         * so we delete it and create a new one. */
        if (s->lockfd < 0) {
            unlink(s->lockfile);
            s->lockfd = open(s->lockfile, O_CREAT | O_RDWR, 0600);
            if (s->lockfd < 0) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
        }
        if (setjobuid > 0){
            if (chown(s->nspace_path, (uid_t) jobuid, (gid_t) -1) < 0){
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
            if (chown(s->lockfile, (uid_t) jobuid, (gid_t) -1) < 0) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
            if (0 != chmod(s->lockfile, S_IRUSR | S_IWGRP | S_IRGRP)) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
        }
        seg = _create_new_segment(INITIAL_SEGMENT, m, 0);
    }
    else {
        s->lockfd = open(s->lockfile, O_RDONLY);
        if (-1 == s->lockfd) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            return PMIX_ERROR;
        }
        seg = _attach_new_segment(INITIAL_SEGMENT, m, 0);
    }
    if (NULL != seg) {
        s->sm_seg_first = seg;
        s->sm_seg_last = s->sm_seg_first;
        return PMIX_SUCCESS;
    }

    return PMIX_ERR_OUT_OF_RESOURCE;
}

static inline void _esh_session_release(session_t *s)
{
    if (!s->in_use) {
        return;
    }

    _delete_sm_desc(s->sm_seg_first);
    close(s->lockfd);

    if (NULL != s->lockfile) {
        if(PMIX_PROC_SERVER == pmix_globals.proc_type) {
            unlink(s->lockfile);
        }
        free(s->lockfile);
    }
    if (NULL != s->nspace_path) {
        if(PMIX_PROC_SERVER == pmix_globals.proc_type) {
            _esh_dir_del(s->nspace_path);
        }
        free(s->nspace_path);
    }
    memset ((char *) s, 0, sizeof(*s));
}

int _esh_init(pmix_info_t info[], size_t ninfo)
{
    int rc;
    size_t n;
    char *dstor_tmpdir = NULL;
    size_t tbl_idx;
    struct stat st = {0};
    ns_map_data_t *ns_map = NULL;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s", __FILE__, __LINE__, __func__));

    _jobuid = getuid();
    _setjobuid = 0;

    if (PMIX_SUCCESS != (rc = _esh_tbls_init())) {
        goto err_exit;
    }

    rc = pmix_sm_init();
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto err_exit;
    }

    _set_constants_from_env();

    if (NULL != _base_path) {
        free(_base_path);
        _base_path = NULL;
    }

    /* find the temp dir */
    if (PMIX_PROC_SERVER == pmix_globals.proc_type) {
        _esh_session_map_search = _esh_session_map_search_server;

        /* scan incoming info for directives */
        if (NULL != info) {
            for (n=0; n < ninfo; n++) {
                if (0 == strcmp(PMIX_USERID, info[n].key)) {
                    _jobuid = info[n].value.data.uint32;
                    _setjobuid = 1;
                    continue;
                }
                if (0 == strcmp(PMIX_DSTPATH, info[n].key)) {
                    /* PMIX_DSTPATH is the way for RM to customize the
                     * place where shared memory files are placed.
                     * We need this for the following reasons:
                     * - disk usage: files can be relatively large and the system may
                     *   have a small common temp directory.
                     * - performance: system may have a fast IO device (i.e. burst buffer)
                     *   for the local usage.
                     *
                     * PMIX_DSTPATH has higher priority than PMIX_SERVER_TMPDIR
                     */
                    dstor_tmpdir = (char*)info[n].value.data.ptr;
                    continue;
                }
                if (0 == strcmp(PMIX_SERVER_TMPDIR, info[n].key)) {
                    if (NULL == dstor_tmpdir) {
                        dstor_tmpdir = (char*)info[n].value.data.string;
                    }
                    continue;
                }
            }
        }

        if (NULL == dstor_tmpdir) {
            if (NULL == (dstor_tmpdir = getenv("TMPDIR"))) {
                if (NULL == (dstor_tmpdir = getenv("TEMP"))) {
                    if (NULL == (dstor_tmpdir = getenv("TMP"))) {
                        dstor_tmpdir = "/tmp";
                    }
                }
            }
        }

        asprintf(&_base_path, "%s/pmix_dstor_%d", dstor_tmpdir, getpid());
        if (NULL == _base_path) {
            PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }

        if (stat(_base_path, &st) == -1){
            if (0 != mkdir(_base_path, 0770)) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                goto err_exit;
            }
        }
        if (_setjobuid > 0) {
            if (chown(_base_path, (uid_t) _jobuid, (gid_t) -1) < 0){
                PMIX_ERROR_LOG(PMIX_ERROR);
                goto err_exit;
            }
        }
        _esh_session_map_search = _esh_session_map_search_server;
        return PMIX_SUCCESS;
    }
    /* for clients */
    else {
        if (NULL == (dstor_tmpdir = getenv(PMIX_DSTORE_ESH_BASE_PATH))){
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            goto err_exit;
        }
        if (NULL == (_base_path = strdup(dstor_tmpdir))) {
            rc = PMIX_ERR_OUT_OF_RESOURCE;
            PMIX_ERROR_LOG(rc);
            goto err_exit;
        }
        _esh_session_map_search = _esh_session_map_search_client;
    }

    rc = _esh_session_tbl_add(&tbl_idx);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto err_exit;
    }

    ns_map = _esh_session_map(pmix_globals.myid.nspace, tbl_idx);
    if (NULL == ns_map) {
        rc = PMIX_ERR_OUT_OF_RESOURCE;
        goto err_exit;
    }

    if (PMIX_SUCCESS != (rc =_esh_session_init(tbl_idx, ns_map, _jobuid, _setjobuid))) {
        goto err_exit;
    }

    return PMIX_SUCCESS;
err_exit:
    pmix_output(0, "Cannot initialize dstore/esh component");
    return rc;
}

int _esh_finalize(void)
{
    struct stat st = {0};
    pmix_status_t rc;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s", __FILE__, __LINE__, __func__));

    _esh_sessions_cleanup();
    _esh_ns_map_cleanup();
    _esh_ns_track_cleanup();

    pmix_sm_finalize();

    if (NULL != _base_path){
        if(PMIX_PROC_SERVER == pmix_globals.proc_type) {
            if (lstat(_base_path, &st) >= 0){
                if (PMIX_SUCCESS != (rc = _esh_dir_del(_base_path))) {
                    PMIX_ERROR_LOG(PMIX_ERROR);
                }
            }
        }
        free(_base_path);
        _base_path = NULL;
    }

    return PMIX_SUCCESS;
}

int _esh_store(const char *nspace, pmix_rank_t rank, pmix_kval_t *kv)
{
    int rc = PMIX_ERROR, lock_rc;
    ns_track_elem_t *elem;
    pmix_buffer_t pbkt, xfer;
    ns_seg_info_t ns_info;
    ns_map_data_t *ns_map = NULL;

    if (NULL == kv) {
        return PMIX_ERROR;
    }

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s: for %s:%u",
                         __FILE__, __LINE__, __func__, nspace, rank));

    if (NULL == (ns_map =_esh_session_map_search(nspace))) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    /* set exclusive lock */
    if (PMIX_SUCCESS != (rc = _ESH_WRLOCK(_ESH_SESSION_lockfd(ns_map->tbl_idx)))) {
        return rc;
    }

    /* First of all, we go through local track list (list of ns_track_elem_t structures)
     * and look for an element for the target namespace.
     * If it is there, then shared memory segments for it are created, so we take it.
     * Otherwise, create a new element, fill its fields, create corresponding meta
     * and data segments for this namespace, add it to the local track list,
     * and put this info (ns_seg_info_t) to the initial segment. If initial segment
     * if full, then extend it by creating a new one and mark previous one as full.
     * All this stuff is done inside _get_track_elem_for_namespace function.
     */

    elem = _get_track_elem_for_namespace(ns_map);
    if (NULL == elem) {
        PMIX_ERROR_LOG(rc);
        goto err_exit;
    }

    /* If a new element was just created, we need to create corresponding meta and
     * data segments and update corresponding element's fields. */
    if (NULL == elem->meta_seg || NULL == elem->data_seg) {
        memset(&ns_info.ns_map, 0, sizeof(ns_info.ns_map));
        strncpy(ns_info.ns_map.name, ns_map->name, sizeof(ns_info.ns_map.name)-1);
        ns_info.ns_map.tbl_idx = ns_map->tbl_idx;
        ns_info.num_meta_seg = 1;
        ns_info.num_data_seg = 1;
        rc = _update_ns_elem(elem, &ns_info);
        if (PMIX_SUCCESS != rc || NULL == elem->meta_seg || NULL == elem->data_seg) {
            PMIX_ERROR_LOG(rc);
            goto err_exit;
        }

        /* zero created shared memory segments for this namespace */
        memset(elem->meta_seg->seg_info.seg_base_addr, 0, _meta_segment_size);
        memset(elem->data_seg->seg_info.seg_base_addr, 0, _data_segment_size);

        /* put ns's shared segments info to the global meta segment. */
        rc = _put_ns_info_to_initial_segment(ns_map, &elem->meta_seg->seg_info, &elem->data_seg->seg_info);
        if (PMIX_SUCCESS != rc) {
            goto err_exit;
        }
    }

    /* Now we know info about meta segment for this namespace. If meta segment
     * is not empty, then we look for data for the target rank. If they present, replace it. */
    PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
    PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
    PMIX_LOAD_BUFFER(&xfer, kv->value->data.bo.bytes, kv->value->data.bo.size);
    pmix_buffer_t *pxfer = &xfer;
    pmix_bfrop.pack(&pbkt, &pxfer, 1, PMIX_BUFFER);
    xfer.base_ptr = NULL;
    xfer.bytes_used = 0;

    rc = _store_data_for_rank(elem, rank, &pbkt);

    PMIX_DESTRUCT(&xfer);
    PMIX_DESTRUCT(&pbkt);

    /* unset lock */
    if (PMIX_SUCCESS != (lock_rc = _ESH_UNLOCK(_ESH_SESSION_lockfd(ns_map->tbl_idx)))) {
        return lock_rc;
    }
    return rc;

err_exit:
    /* unset lock */
    if (PMIX_SUCCESS != (lock_rc = _ESH_UNLOCK(_ESH_SESSION_lockfd(ns_map->tbl_idx)))) {
        return lock_rc;
    }
    return rc;
}

int _esh_fetch(const char *nspace, pmix_rank_t rank, const char *key, pmix_value_t **kvs)
{
    ns_seg_info_t *ns_info = NULL;
    int rc = PMIX_ERROR, lock_rc;
    ns_track_elem_t *elem;
    rank_meta_info *rinfo = NULL;
    size_t kval_cnt;
    seg_desc_t *meta_seg, *data_seg;
    uint8_t *addr;
    pmix_buffer_t buffer;
    pmix_value_t val;
    uint32_t nprocs;
    pmix_rank_t cur_rank;
    ns_map_data_t *ns_map = NULL;

    if (NULL == key) {
        PMIX_OUTPUT_VERBOSE((7, pmix_globals.debug_output,
                             "dstore: Does not support passed parameters"));
        return PMIX_ERROR;
    }

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s: for %s:%u look for key %s",
                         __FILE__, __LINE__, __func__, nspace, rank, key));

    if (NULL == (ns_map =_esh_session_map_search(nspace))) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    if (kvs) {
        *kvs = NULL;
    }

    if (PMIX_RANK_UNDEF == rank) {
        nprocs = _get_univ_size(ns_map->name);
        cur_rank = PMIX_RANK_UNDEF;
    } else {
        nprocs = 1;
        cur_rank = rank;
    }

    /* set shared lock */
    if (PMIX_SUCCESS != (lock_rc = _ESH_RDLOCK(_ESH_SESSION_lockfd(ns_map->tbl_idx)))) {
        return lock_rc;
    }

    /* First of all, we go through all initial segments and look at their field.
     * If it's 1, then generate name of next initial segment incrementing id by one and attach to it.
     * We need this step to synchronize initial shared segments with our local track list.
     * Then we look for the target namespace in all initial segments.
     * If it is found, we get numbers of meta & data segments and
     * compare these numbers with the number of trackable meta & data
     * segments for this namespace in the local track list.
     * If the first number exceeds the last, or the local track list
     * doesn't track current namespace yet, then we update it (attach
     * to additional segments).
     */

    /* first update local information about initial segments. they can be extended, so then we need to attach to new segments. */
    _update_initial_segment_info(ns_map);

    rc = PMIX_ERROR;
    /* get information about shared segments per this namespace from the initial segment. */

    //pmix_output(0, "%s:%d:%s: ns %s %s",__FILE__, __LINE__, __func__, nspace, ns_map->name);

    ns_info = _get_ns_info_from_initial_segment(ns_map);
    if (NULL == ns_info) {
        /* no data for this namespace is found in the shared memory. */
        PMIX_OUTPUT_VERBOSE((7, pmix_globals.debug_output,
                    "%s:%d:%s:  no data for ns %s is found in the shared memory.",
                    __FILE__, __LINE__, __func__, ns_map->name));
        goto done;
    }

    /* get ns_track_elem_t object for the target namespace from the local track list. */
    elem = _get_track_elem_for_namespace(ns_map);
    if (NULL == elem) {
        goto done;
    }
    /* need to update tracker:
     * attach to shared memory regions for this namespace and store its info locally
     * to operate with address and detach/unlink afterwards. */
    rc = _update_ns_elem(elem, ns_info);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

    /* now we know info about meta segment for this namespace. */
    meta_seg = elem->meta_seg;
    data_seg = elem->data_seg;

    while (nprocs--) {
        if (PMIX_RANK_UNDEF == rank) {
            cur_rank++;
        }
        /* Then we look for the rank meta info in the shared meta segment. */
        rinfo = _get_rank_meta_info(cur_rank, meta_seg);
        if (NULL == rinfo) {
            PMIX_OUTPUT_VERBOSE((7, pmix_globals.debug_output,
                        "%s:%d:%s:  no data for this rank is found in the shared memory. rank %u",
                        __FILE__, __LINE__, __func__, cur_rank));
            rc = PMIX_ERR_PROC_ENTRY_NOT_FOUND;
            continue;
        }
        addr = _get_data_region_by_offset(data_seg, rinfo->offset);
        if (NULL == addr) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            rc = PMIX_ERR_PROC_ENTRY_NOT_FOUND;
            continue;
        }
        kval_cnt = rinfo->count;
        /* TODO: probably PMIX_ERR_NOT_FOUND is a better way but
         * setting to one initiates wrong next logic for unknown reason */
        rc = PMIX_ERROR;

        while (0 < kval_cnt) {
            /* data is stored in the following format:
             * key[PMIX_MAX_KEYLEN+1]
             * size_t size
             * byte buffer containing pmix_value, should be loaded to pmix_buffer_t and unpacked.
             * next kval pair
             * .....
             * EXTENSION slot which has key = EXTENSION_SLOT and a size_t value for offset to next data address for this process.
             */
            if (0 == strncmp((const char *)addr, ESH_REGION_INVALIDATED, PMIX_MAX_KEYLEN+1)) {
                PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                            "%s:%d:%s: for rank %s:%u, skip %s region",
                            __FILE__, __LINE__, __func__, nspace, cur_rank, ESH_REGION_INVALIDATED));
                /*skip it */
                size_t size;
                memcpy(&size, addr + PMIX_MAX_KEYLEN + 1, sizeof(size_t));
                /* go to next item, updating address */
                addr += KVAL_SIZE(size);
            } else if (0 == strncmp((const char *)addr, ESH_REGION_EXTENSION, PMIX_MAX_KEYLEN+1)) {
                size_t offset;
                memcpy(&offset, addr + PMIX_MAX_KEYLEN + 1 + sizeof(size_t), sizeof(size_t));
                PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                            "%s:%d:%s: for rank %s:%u, reached %s with %lu value",
                            __FILE__, __LINE__, __func__, nspace, cur_rank, ESH_REGION_EXTENSION, offset));
                if (0 < offset) {
                    /* go to next item, updating address */
                    addr = _get_data_region_by_offset(data_seg, offset);
                    if (NULL == addr) {
                        PMIX_ERROR_LOG(rc);
                        goto done;
                    }
                } else {
                    /* no more data for this rank */
                    PMIX_OUTPUT_VERBOSE((7, pmix_globals.debug_output,
                                "%s:%d:%s:  no more data for this rank is found in the shared memory. rank %u key %s not found",
                                __FILE__, __LINE__, __func__, cur_rank, key));
                    break;
                }
            } else if (0 == strncmp((const char *)addr, key, PMIX_MAX_KEYLEN+1)) {
                PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                            "%s:%d:%s: for rank %s:%u, found target key %s",
                            __FILE__, __LINE__, __func__, nspace, cur_rank, key));
                /* target key is found, get value */
                size_t size;
                memcpy(&size, addr + PMIX_MAX_KEYLEN + 1, sizeof(size_t));
                addr += PMIX_MAX_KEYLEN + 1 + sizeof(size_t);
                PMIX_CONSTRUCT(&buffer, pmix_buffer_t);
                PMIX_LOAD_BUFFER(&buffer, addr, size);
                int cnt = 1;
                /* unpack value for this key from the buffer. */
                PMIX_VALUE_CONSTRUCT(&val);
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&buffer, &val, &cnt, PMIX_VALUE))) {
                    PMIX_ERROR_LOG(rc);
                } else if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)kvs, &val, PMIX_VALUE))) {
                    PMIX_ERROR_LOG(rc);
                }
                PMIX_VALUE_DESTRUCT(&val);
                buffer.base_ptr = NULL;
                buffer.bytes_used = 0;
                PMIX_DESTRUCT(&buffer);
                goto done;
            } else {
                char ckey[PMIX_MAX_KEYLEN+1] = {0};
                strncpy(ckey, (const char *)addr, PMIX_MAX_KEYLEN+1);
                size_t size;
                memcpy(&size, addr + PMIX_MAX_KEYLEN + 1, sizeof(size_t));
                PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                            "%s:%d:%s: for rank %s:%u, skip key %s look for key %s", __FILE__, __LINE__, __func__, nspace, cur_rank, ckey, key));
                /* go to next item, updating address */
                addr += KVAL_SIZE(size);
                kval_cnt--;
            }
        }
    }

done:
    /* unset lock */
    if (PMIX_SUCCESS != (lock_rc = _ESH_UNLOCK(_ESH_SESSION_lockfd(ns_map->tbl_idx)))) {
        return lock_rc;
    }
    return rc;
}

static int _esh_patch_env(const char *nspace, char ***env)
{
    pmix_status_t rc;
    ns_map_data_t *ns_map = NULL;

    if (NULL == _esh_session_map_search) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    if (NULL == (ns_map =_esh_session_map_search(nspace))) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    if ((NULL == _base_path) || (strlen(_base_path) == 0)){
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    if(PMIX_SUCCESS != (rc = pmix_setenv(PMIX_DSTORE_ESH_BASE_PATH,
                                        _ESH_SESSION_path(ns_map->tbl_idx), true, env))){
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    return PMIX_SUCCESS;
}

static int _esh_nspace_add(const char *nspace, pmix_info_t info[], size_t ninfo)
{
    int rc;
    size_t tbl_idx;
    uid_t jobuid = _jobuid;
    char setjobuid = _setjobuid;
    size_t n;
    ns_map_data_t *ns_map = NULL;

    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strcmp(PMIX_USERID, info[n].key)) {
                jobuid = info[n].value.data.uint32;
                setjobuid = 1;
                continue;
            }
        }
    }

    if (PMIX_SUCCESS != _esh_jobuid_tbl_search(jobuid, &tbl_idx)) {

        rc = _esh_session_tbl_add(&tbl_idx);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        ns_map = _esh_session_map(nspace, tbl_idx);
        if (NULL == ns_map) {
            return PMIX_ERROR;
        }

        if (PMIX_SUCCESS != (rc =_esh_session_init(tbl_idx, ns_map, jobuid, setjobuid))) {
            return rc;
        }
    }
    else {
        ns_map = _esh_session_map(nspace, tbl_idx);
        if (NULL == ns_map) {
            return PMIX_ERROR;
        }
    }

    return PMIX_SUCCESS;
}

static int _esh_nspace_del(const char *nspace)
{
    pmix_status_t rc = PMIX_SUCCESS;
    size_t map_idx, size;
    int in_use = 0;
    ns_map_data_t *ns_map_data = NULL;
    ns_map_t *ns_map;
    session_t *session_tbl = NULL;
    ns_track_elem_t *trk = NULL;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
        "%s:%d:%s delete nspace `%s`", __FILE__, __LINE__, __func__, nspace));

    if (NULL == (ns_map_data =_esh_session_map_search(nspace))) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    size = pmix_value_array_get_size(_ns_map_array);
    ns_map = PMIX_VALUE_ARRAY_GET_BASE(_ns_map_array, ns_map_t);

    for (map_idx = 0; map_idx < size; map_idx++){
        if (ns_map[map_idx].in_use &&
                        (ns_map[map_idx].data.tbl_idx == ns_map_data->tbl_idx)) {
            if (0 == strcmp(ns_map[map_idx].data.name, nspace)) {
                _esh_session_map_clean(&ns_map[map_idx]);
                continue;
            }
            in_use++;
            break;
        }
    }

    if(ns_map_data->track_idx >= 0) {
        trk = pmix_value_array_get_item(_ns_track_array, ns_map_data->track_idx);
        if((ns_map_data->track_idx + 1) > (int)pmix_value_array_get_size(_ns_track_array)) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            return PMIX_ERROR;
        }
        PMIX_DESTRUCT(trk);
    }

    /* A lot of nspaces may be using same session info
     * session record can only be deleted once all references are gone */
    if (!in_use) {
        session_tbl = PMIX_VALUE_ARRAY_GET_BASE(_session_array, session_t);

        PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
            "%s:%d:%s delete session for jobuid: %d", __FILE__, __LINE__, __func__, session_tbl[ns_map_data->tbl_idx].jobuid));
        _esh_session_release(&session_tbl[ns_map_data->tbl_idx]);
     }

    return rc;
}

static void _set_constants_from_env()
{
    char *str;
    int page_size = _pmix_getpagesize();

    if( NULL != (str = getenv(ESH_ENV_INITIAL_SEG_SIZE)) ) {
        _initial_segment_size = strtoul(str, NULL, 10);
        if ((size_t)page_size > _initial_segment_size) {
            _initial_segment_size = (size_t)page_size;
        }
    }
    if (0 == _initial_segment_size) {
        _initial_segment_size = INITIAL_SEG_SIZE;
    }
    if( NULL != (str = getenv(ESH_ENV_NS_META_SEG_SIZE)) ) {
        _meta_segment_size = strtoul(str, NULL, 10);
        if ((size_t)page_size > _meta_segment_size) {
            _meta_segment_size = (size_t)page_size;
        }
    }
    if (0 == _meta_segment_size) {
        _meta_segment_size = NS_META_SEG_SIZE;
    }
    if( NULL != (str = getenv(ESH_ENV_NS_DATA_SEG_SIZE)) ) {
        _data_segment_size = strtoul(str, NULL, 10);
        if ((size_t)page_size > _data_segment_size) {
            _data_segment_size = (size_t)page_size;
        }
    }
    if (0 == _data_segment_size) {
        _data_segment_size = NS_DATA_SEG_SIZE;
    }
    if (NULL != (str = getenv(ESH_ENV_LINEAR))) {
        if (1 == strtoul(str, NULL, 10)) {
            _direct_mode = 1;
        }
    }

    _max_ns_num = (_initial_segment_size - sizeof(size_t) * 2) / sizeof(ns_seg_info_t);
    _max_meta_elems = (_meta_segment_size - sizeof(size_t)) / sizeof(rank_meta_info);

}

static void _delete_sm_desc(seg_desc_t *desc)
{
    seg_desc_t *tmp;

    /* free all global segments */
    while (NULL != desc) {
        tmp = desc->next;
        /* detach & unlink from current desc */
        if (desc->seg_info.seg_cpid == getpid()) {
            pmix_sm_segment_unlink(&desc->seg_info);
        }
        pmix_sm_segment_detach(&desc->seg_info);
        free(desc);
        desc = tmp;
    }
}

static int _pmix_getpagesize(void)
{
#if defined(_SC_PAGESIZE )
    return sysconf(_SC_PAGESIZE);
#elif defined(_SC_PAGE_SIZE)
    return sysconf(_SC_PAGE_SIZE);
#else
    return 65536; /* safer to overestimate than under */
#endif
}

static seg_desc_t *_create_new_segment(segment_type type, const ns_map_data_t *ns_map, uint32_t id)
{
    int rc;
    char file_name[PMIX_PATH_MAX];
    size_t size;
    seg_desc_t *new_seg = NULL;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s: segment type %d, nspace %s, id %u",
                         __FILE__, __LINE__, __func__, type, ns_map->name, id));

    switch (type) {
        case INITIAL_SEGMENT:
            size = _initial_segment_size;
            snprintf(file_name, PMIX_PATH_MAX, "%s/initial-pmix_shared-segment-%u",
                _ESH_SESSION_path(ns_map->tbl_idx), id);
            break;
        case NS_META_SEGMENT:
            size = _meta_segment_size;
            snprintf(file_name, PMIX_PATH_MAX, "%s/smseg-%s-%u",
                _ESH_SESSION_path(ns_map->tbl_idx), ns_map->name, id);
            break;
        case NS_DATA_SEGMENT:
            size = _data_segment_size;
            snprintf(file_name, PMIX_PATH_MAX, "%s/smdataseg-%s-%d",
                _ESH_SESSION_path(ns_map->tbl_idx), ns_map->name, id);
            break;
        default:
            PMIX_ERROR_LOG(PMIX_ERROR);
            return NULL;
    }
    new_seg = (seg_desc_t*)malloc(sizeof(seg_desc_t));
    if (new_seg) {
        new_seg->id = id;
        new_seg->next = NULL;
        new_seg->type = type;
        rc = pmix_sm_segment_create(&new_seg->seg_info, file_name, size);
        if (PMIX_SUCCESS == rc) {
            memset(new_seg->seg_info.seg_base_addr, 0, size);
        } else {
            free(new_seg);
            new_seg = NULL;
            PMIX_ERROR_LOG(rc);
        }

        if (_setjobuid > 0){
            rc = PMIX_SUCCESS;
            if (chown(file_name, (uid_t) _jobuid, (gid_t) -1) < 0){
                PMIX_ERROR_LOG(PMIX_ERROR);
                rc = PMIX_ERROR;
            }
            /* set the mode as required */
            if (0 != chmod(file_name, S_IRUSR | S_IRGRP | S_IWGRP )) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                rc = PMIX_ERROR;
            }
            if (rc != PMIX_SUCCESS) {
                free(new_seg);
                new_seg = NULL;
            }
        }
    }
    return new_seg;
}

static seg_desc_t *_attach_new_segment(segment_type type, const ns_map_data_t *ns_map, uint32_t id)
{
    int rc;
    seg_desc_t *new_seg = NULL;
    new_seg = (seg_desc_t*)malloc(sizeof(seg_desc_t));
    new_seg->id = id;
    new_seg->next = NULL;
    new_seg->type = type;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s: segment type %d, nspace %s, id %u",
                         __FILE__, __LINE__, __func__, type, ns_map->name, id));

    switch (type) {
        case INITIAL_SEGMENT:
            new_seg->seg_info.seg_size = _initial_segment_size;
            snprintf(new_seg->seg_info.seg_name, PMIX_PATH_MAX, "%s/initial-pmix_shared-segment-%u",
                _ESH_SESSION_path(ns_map->tbl_idx), id);
            break;
        case NS_META_SEGMENT:
            new_seg->seg_info.seg_size = _meta_segment_size;
            snprintf(new_seg->seg_info.seg_name, PMIX_PATH_MAX, "%s/smseg-%s-%u",
                _ESH_SESSION_path(ns_map->tbl_idx), ns_map->name, id);
            break;
        case NS_DATA_SEGMENT:
            new_seg->seg_info.seg_size = _data_segment_size;
            snprintf(new_seg->seg_info.seg_name, PMIX_PATH_MAX, "%s/smdataseg-%s-%d",
                _ESH_SESSION_path(ns_map->tbl_idx), ns_map->name, id);
            break;
        default:
            PMIX_ERROR_LOG(PMIX_ERROR);
            return NULL;
    }
    rc = pmix_sm_segment_attach(&new_seg->seg_info, PMIX_SM_RONLY);
    if (PMIX_SUCCESS != rc) {
        free(new_seg);
        new_seg = NULL;
        PMIX_ERROR_LOG(rc);
    }
    return new_seg;
}

/* This function synchronizes the content of initial shared segment and the local track list. */
static int _update_ns_elem(ns_track_elem_t *ns_elem, ns_seg_info_t *info)
{
    seg_desc_t *seg, *tmp = NULL;
    size_t i, offs;
    ns_map_data_t *ns_map = NULL;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s",
                         __FILE__, __LINE__, __func__));

    if (NULL == (ns_map =_esh_session_map_search(info->ns_map.name))) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    tmp = ns_elem->meta_seg;
    if (NULL != tmp) {
        while(NULL != tmp->next) {
            tmp = tmp->next;
        }
    }

    /* synchronize number of meta segments for the target namespace. */
    for (i = ns_elem->num_meta_seg; i < info->num_meta_seg; i++) {
        if (PMIX_PROC_SERVER == pmix_globals.proc_type) {
            seg = _create_new_segment(NS_META_SEGMENT, &info->ns_map, i);
        } else {
            seg = _attach_new_segment(NS_META_SEGMENT, &info->ns_map, i);
        }
        if (NULL == seg) {
            return PMIX_ERROR;
        }
        if (NULL == tmp) {
            ns_elem->meta_seg = seg;
        } else {
            tmp->next = seg;
        }
        tmp = seg;
        ns_elem->num_meta_seg++;
    }

    tmp = ns_elem->data_seg;
    if (NULL != tmp) {
        while(NULL != tmp->next) {
            tmp = tmp->next;
        }
    }
    /* synchronize number of data segments for the target namespace. */
    for (i = ns_elem->num_data_seg; i < info->num_data_seg; i++) {
        if (PMIX_PROC_SERVER == pmix_globals.proc_type) {
            seg = _create_new_segment(NS_DATA_SEGMENT, &info->ns_map, i);
            if (seg) {
                offs = sizeof(size_t);//shift on offset field itself
                memcpy(seg->seg_info.seg_base_addr, &offs, sizeof(size_t));
            }
        } else {
            seg = _attach_new_segment(NS_DATA_SEGMENT, &info->ns_map, i);
        }
        if (NULL == seg) {
            return PMIX_ERROR;
        }
        if (NULL == tmp) {
            ns_elem->data_seg = seg;
        } else {
            tmp->next = seg;
        }
        tmp = seg;
        ns_elem->num_data_seg++;
    }

    return PMIX_SUCCESS;
}

static seg_desc_t *extend_segment(seg_desc_t *segdesc, const ns_map_data_t *ns_map)
{
    seg_desc_t *tmp, *seg;

    PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                         "%s:%d:%s",
                         __FILE__, __LINE__, __func__));
    /* find last segment */
    tmp = segdesc;
    while (NULL != tmp->next) {
        tmp = tmp->next;
    }
    /* create another segment, the old one is full. */
    seg = _create_new_segment(segdesc->type, ns_map, tmp->id + 1);
    tmp->next = seg;

    return seg;
}

static int _put_ns_info_to_initial_segment(const ns_map_data_t *ns_map, pmix_sm_seg_t *metaseg, pmix_sm_seg_t *dataseg)
{
    ns_seg_info_t elem;
    size_t num_elems;
    num_elems = *((size_t*)(_ESH_SESSION_sm_seg_last(ns_map->tbl_idx)->seg_info.seg_base_addr));
    seg_desc_t *last_seg = _ESH_SESSION_sm_seg_last(ns_map->tbl_idx);

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s", __FILE__, __LINE__, __func__));

    if (_max_ns_num == num_elems) {
        num_elems = 0;
        if (NULL == (last_seg = extend_segment(last_seg, ns_map))) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            return PMIX_ERROR;
        }
        /* mark previous segment as full */
        size_t full = 1;
        memcpy((uint8_t*)(_ESH_SESSION_sm_seg_last(ns_map->tbl_idx)->seg_info.seg_base_addr + sizeof(size_t)), &full, sizeof(size_t));
        _ESH_SESSION_sm_seg_last(ns_map->tbl_idx) = last_seg;
        memset(_ESH_SESSION_sm_seg_last(ns_map->tbl_idx)->seg_info.seg_base_addr, 0, _initial_segment_size);
    }
    memset(&elem.ns_map, 0, sizeof(elem.ns_map));
    strncpy(elem.ns_map.name, ns_map->name, sizeof(elem.ns_map.name)-1);
    elem.ns_map.tbl_idx = ns_map->tbl_idx;
    elem.num_meta_seg = 1;
    elem.num_data_seg = 1;
    memcpy((uint8_t*)(_ESH_SESSION_sm_seg_last(ns_map->tbl_idx)->seg_info.seg_base_addr) + sizeof(size_t) * 2 + num_elems * sizeof(ns_seg_info_t),
            &elem, sizeof(ns_seg_info_t));
    num_elems++;
    memcpy((uint8_t*)(_ESH_SESSION_sm_seg_last(ns_map->tbl_idx)->seg_info.seg_base_addr), &num_elems, sizeof(size_t));
    return PMIX_SUCCESS;
}

/* clients should sync local info with information from initial segment regularly */
static void _update_initial_segment_info(const ns_map_data_t *ns_map)
{
    seg_desc_t *tmp;
    tmp = _ESH_SESSION_sm_seg_first(ns_map->tbl_idx);

    PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                         "%s:%d:%s", __FILE__, __LINE__, __func__));

    /* go through all global segments */
    do {
        /* check if current segment was marked as full but no more next segment is in the chain */
        if (NULL == tmp->next && 1 == *((size_t*)((uint8_t*)(tmp->seg_info.seg_base_addr) + sizeof(size_t)))) {
            tmp->next = _attach_new_segment(INITIAL_SEGMENT, ns_map, tmp->id+1);
        }
        tmp = tmp->next;
    }
    while (NULL != tmp);
}

/* this function will be used by clients to get ns data from the initial segment and add them to the tracker list */
static ns_seg_info_t *_get_ns_info_from_initial_segment(const ns_map_data_t *ns_map)
{
    int rc;
    size_t i;
    seg_desc_t *tmp;
    ns_seg_info_t *elem, *cur_elem;
    elem = NULL;
    size_t num_elems;

    PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                         "%s:%d:%s", __FILE__, __LINE__, __func__));

    tmp = _ESH_SESSION_sm_seg_first(ns_map->tbl_idx);

    rc = 1;
    /* go through all global segments */
    do {
        num_elems = *((size_t*)(tmp->seg_info.seg_base_addr));
        for (i = 0; i < num_elems; i++) {
            cur_elem = (ns_seg_info_t*)((uint8_t*)(tmp->seg_info.seg_base_addr) + sizeof(size_t) * 2 + i * sizeof(ns_seg_info_t));
            if (0 == (rc = strncmp(cur_elem->ns_map.name, ns_map->name, strlen(ns_map->name)+1))) {
                break;
            }
        }
        if (0 == rc) {
            elem = cur_elem;
            break;
        }
        tmp = tmp->next;
    }
    while (NULL != tmp);
    return elem;
}

static ns_track_elem_t *_get_track_elem_for_namespace(ns_map_data_t *ns_map)
{
    ns_track_elem_t *new_elem = NULL;
    size_t size = pmix_value_array_get_size(_ns_track_array);

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s: nspace %s",
                         __FILE__, __LINE__, __func__, ns_map->name));

    /* check if this namespace is already being tracked to avoid duplicating data. */
    if (ns_map->track_idx >= 0) {
        if ((ns_map->track_idx + 1) > (int)size) {
            return NULL;
        }
        /* data for this namespace should be already stored in shared memory region. */
        /* so go and just put new data. */
        return pmix_value_array_get_item(_ns_track_array, ns_map->track_idx);
    }

    /* create shared memory regions for this namespace and store its info locally
     * to operate with address and detach/unlink afterwards. */
    if (NULL == (new_elem = pmix_value_array_get_item(_ns_track_array, size))) {
        return NULL;
    }
    PMIX_CONSTRUCT(new_elem, ns_track_elem_t);
    strncpy(new_elem->ns_map.name, ns_map->name, sizeof(new_elem->ns_map.name)-1);
    /* save latest track idx to info of nspace */
    ns_map->track_idx = size;

    return new_elem;
}

static rank_meta_info *_get_rank_meta_info(pmix_rank_t rank, seg_desc_t *segdesc)
{
    size_t i;
    rank_meta_info *elem = NULL;
    seg_desc_t *tmp = segdesc;
    size_t num_elems, rel_offset;
    int id;
    rank_meta_info *cur_elem;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s",
                         __FILE__, __LINE__, __func__));

    if (1 == _direct_mode) {
        /* do linear search to find the requested rank inside all meta segments
         * for this namespace. */
        /* go through all existing meta segments for this namespace */
        do {
            num_elems = *((size_t*)(tmp->seg_info.seg_base_addr));
            for (i = 0; i < num_elems; i++) {
                cur_elem = (rank_meta_info*)((uint8_t*)(tmp->seg_info.seg_base_addr) + sizeof(size_t) + i * sizeof(rank_meta_info));
                if (rank == cur_elem->rank) {
                    elem = cur_elem;
                    break;
                }
            }
            tmp = tmp->next;
        }
        while (NULL != tmp && NULL == elem);
    } else {
        /* directly compute index of meta segment (id) and relative offset (rel_offset)
         * inside this segment for fast lookup a rank_meta_info object for the requested rank. */
        id = rank/_max_meta_elems;
        rel_offset = (rank%_max_meta_elems) * sizeof(rank_meta_info) + sizeof(size_t);
        /* go through all existing meta segments for this namespace.
         * Stop at id number if it exists. */
        while (NULL != tmp->next && 0 != id) {
            tmp = tmp->next;
            id--;
        }
        if (0 == id) {
            /* the segment is found, looking for data for the target rank. */
            elem = (rank_meta_info*)((uint8_t*)(tmp->seg_info.seg_base_addr) + rel_offset);
            if ( 0 == elem->offset) {
                /* offset can never be 0, it means that there is no data for this rank yet. */
                elem = NULL;
            }
        }
    }
    return elem;
}

static int set_rank_meta_info(ns_track_elem_t *ns_info, rank_meta_info *rinfo)
{
    /* it's claimed that there is still no meta info for this rank stored */
    seg_desc_t *tmp;
    size_t num_elems, rel_offset;
    int id, count;
    rank_meta_info *cur_elem;

    if (!ns_info || !rinfo) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                         "%s:%d:%s: nspace %s, add rank %lu offset %lu count %lu meta info",
                         __FILE__, __LINE__, __func__,
                         ns_info->ns_map.name, rinfo->rank, rinfo->offset, rinfo->count));

    tmp = ns_info->meta_seg;
    if (1 == _direct_mode) {
        /* get the last meta segment to put new rank_meta_info at the end. */
        while (NULL != tmp->next) {
            tmp = tmp->next;
        }
        num_elems = *((size_t*)(tmp->seg_info.seg_base_addr));
        if (_max_meta_elems <= num_elems) {
            PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                        "%s:%d:%s: extend meta segment for nspace %s",
                        __FILE__, __LINE__, __func__, ns_info->ns_map.name));
            /* extend meta segment, so create a new one */
            tmp = extend_segment(tmp, &ns_info->ns_map);
            if (NULL == tmp) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
            ns_info->num_meta_seg++;
            memset(tmp->seg_info.seg_base_addr, 0, sizeof(rank_meta_info));
            /* update number of meta segments for namespace in initial_segment */
            ns_seg_info_t *elem = _get_ns_info_from_initial_segment(&ns_info->ns_map);
            if (NULL == elem) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
            if (ns_info->num_meta_seg != elem->num_meta_seg) {
                elem->num_meta_seg = ns_info->num_meta_seg;
            }
            num_elems = 0;
        }
        cur_elem = (rank_meta_info*)((uint8_t*)(tmp->seg_info.seg_base_addr) + sizeof(size_t) + num_elems * sizeof(rank_meta_info));
        memcpy(cur_elem, rinfo, sizeof(rank_meta_info));
        num_elems++;
        memcpy(tmp->seg_info.seg_base_addr, &num_elems, sizeof(size_t));
    } else {
        /* directly compute index of meta segment (id) and relative offset (rel_offset)
         * inside this segment for fast lookup a rank_meta_info object for the requested rank. */
        id = rinfo->rank/_max_meta_elems;
        rel_offset = (rinfo->rank % _max_meta_elems) * sizeof(rank_meta_info) + sizeof(size_t);
        count = id;
        /* go through all existing meta segments for this namespace.
         * Stop at id number if it exists. */
        while (NULL != tmp->next && 0 != count) {
            tmp = tmp->next;
            count--;
        }
        /* if there is no segment with this id, then create all missing segments till the id number. */
        if ((int)ns_info->num_meta_seg < (id+1)) {
            while ((int)ns_info->num_meta_seg != (id+1)) {
                /* extend meta segment, so create a new one */
                tmp = extend_segment(tmp, &ns_info->ns_map);
                if (NULL == tmp) {
                    PMIX_ERROR_LOG(PMIX_ERROR);
                    return PMIX_ERROR;
                }
                memset(tmp->seg_info.seg_base_addr, 0, sizeof(rank_meta_info));
                ns_info->num_meta_seg++;
            }
            /* update number of meta segments for namespace in initial_segment */
            ns_seg_info_t *elem = _get_ns_info_from_initial_segment(&ns_info->ns_map);
            if (NULL == elem) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
            if (ns_info->num_meta_seg != elem->num_meta_seg) {
                elem->num_meta_seg = ns_info->num_meta_seg;
            }
        }
        /* store rank_meta_info object by rel_offset. */
        cur_elem = (rank_meta_info*)((uint8_t*)(tmp->seg_info.seg_base_addr) + rel_offset);
        memcpy(cur_elem, rinfo, sizeof(rank_meta_info));
    }
    return PMIX_SUCCESS;
}

static uint8_t *_get_data_region_by_offset(seg_desc_t *segdesc, size_t offset)
{
    seg_desc_t *tmp = segdesc;
    size_t rel_offset = offset;
    uint8_t *dataaddr = NULL;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s",
                         __FILE__, __LINE__, __func__));

    /* go through all existing data segments for this namespace */
    do {
        if (rel_offset >= _data_segment_size) {
            rel_offset -= _data_segment_size;
        } else {
            dataaddr = tmp->seg_info.seg_base_addr + rel_offset;
        }
        tmp = tmp->next;
    }
    while (NULL != tmp && NULL == dataaddr);
    return dataaddr;
}

static size_t get_free_offset(seg_desc_t *data_seg)
{
    size_t offset;
    seg_desc_t *tmp;
    int id = 0;
    tmp = data_seg;
    /* first find the last data segment */
    while (NULL != tmp->next) {
        tmp = tmp->next;
        id++;
    }
    offset = *((size_t*)(tmp->seg_info.seg_base_addr));
    if (0 == offset) {
        /* this is the first created data segment, the first 8 bytes are used to place the free offset value itself */
        offset = sizeof(size_t);
    }
    return (id * _data_segment_size + offset);
}

static int put_empty_ext_slot(seg_desc_t *dataseg)
{
    size_t global_offset, rel_offset, data_ended, sz, val;
    uint8_t *addr;
    global_offset = get_free_offset(dataseg);
    rel_offset = global_offset % _data_segment_size;
    if (rel_offset + EXT_SLOT_SIZE > _data_segment_size) {
        return PMIX_ERROR;
    }
    addr = _get_data_region_by_offset(dataseg, global_offset);
    strncpy((char *)addr, ESH_REGION_EXTENSION, PMIX_MAX_KEYLEN+1);
    val = 0;
    sz = sizeof(size_t);
    memcpy(addr + PMIX_MAX_KEYLEN + 1, &sz, sz);
    memcpy(addr + PMIX_MAX_KEYLEN + 1 + sizeof(size_t), &val, sz);

    /* update offset at the beginning of current segment */
    data_ended = rel_offset + EXT_SLOT_SIZE;
    addr = (uint8_t*)(addr - rel_offset);
    memcpy(addr, &data_ended, sizeof(size_t));
    return PMIX_SUCCESS;
}

static size_t put_data_to_the_end(ns_track_elem_t *ns_info, seg_desc_t *dataseg, char *key, void *buffer, size_t size)
{
    size_t offset;
    seg_desc_t *tmp;
    int id = 0;
    size_t global_offset, data_ended;
    uint8_t *addr;
    size_t sz;

    PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                         "%s:%d:%s: key %s",
                         __FILE__, __LINE__, __func__, key));

    tmp = dataseg;
    while (NULL != tmp->next) {
        tmp = tmp->next;
        id++;
    }
    global_offset = get_free_offset(dataseg);
    offset = global_offset % _data_segment_size;

    /* We should provide additional space at the end of segment to place EXTENSION_SLOT to have an ability to enlarge data for this rank.*/
    if (sizeof(size_t) + KVAL_SIZE(size) + EXT_SLOT_SIZE > _data_segment_size) {
        /* this is an error case: segment is so small that cannot place evem a single key-value pair.
         * warn a user about it and fail. */
        offset = 0; /* offset cannot be 0 in normal case, so we use this value to indicate a problem. */
        pmix_output(0, "PLEASE set NS_DATA_SEG_SIZE to value which is larger when %lu.",
                sizeof(size_t) + PMIX_MAX_KEYLEN + 1 + sizeof(size_t) + size + EXT_SLOT_SIZE);
        return offset;
    }
    if (offset + KVAL_SIZE(size) + EXT_SLOT_SIZE > _data_segment_size)  {
        id++;
        /* create a new data segment. */
        tmp = extend_segment(tmp, &ns_info->ns_map);
        if (NULL == tmp) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            offset = 0; /* offset cannot be 0 in normal case, so we use this value to indicate a problem. */
            return offset;
        }
        ns_info->num_data_seg++;
        /* update_ns_info_in_initial_segment */
        ns_seg_info_t *elem = _get_ns_info_from_initial_segment(&ns_info->ns_map);
        if (NULL == elem) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            return PMIX_ERROR;
        }
        elem->num_data_seg++;

        offset = sizeof(size_t);
    }
    global_offset = offset + id * _data_segment_size;
    addr = (uint8_t*)(tmp->seg_info.seg_base_addr)+offset;
    strncpy((char *)addr, key, PMIX_MAX_KEYLEN+1);
    sz = size;
    memcpy(addr + PMIX_MAX_KEYLEN + 1, &sz, sizeof(size_t));
    memcpy(addr + PMIX_MAX_KEYLEN + 1 + sizeof(size_t), buffer, size);

    /* update offset at the beginning of current segment */
    data_ended = offset + KVAL_SIZE(size);
    addr = (uint8_t*)(tmp->seg_info.seg_base_addr);
    memcpy(addr, &data_ended, sizeof(size_t));
    PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                         "%s:%d:%s: key %s, rel start offset %lu, rel end offset %lu, abs shift %lu size %lu",
                         __FILE__, __LINE__, __func__, key, offset, data_ended, id * _data_segment_size, size));
    return global_offset;
}

static int pmix_sm_store(ns_track_elem_t *ns_info, pmix_rank_t rank, pmix_kval_t *kval, rank_meta_info **rinfo, int data_exist)
{
    size_t offset, size, kval_cnt;
    pmix_buffer_t *buffer;
    int rc;
    seg_desc_t *datadesc;
    uint8_t *addr;

    PMIX_OUTPUT_VERBOSE((2, pmix_globals.debug_output,
                         "%s:%d:%s: for rank %u, replace flag %d",
                         __FILE__, __LINE__, __func__, rank, data_exist));

    datadesc = ns_info->data_seg;
    /* pack value to the buffer */
    buffer = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(buffer, kval->value, 1, PMIX_VALUE))) {
        PMIX_RELEASE(buffer);
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    size = buffer->bytes_used;

    if (0 == data_exist) {
        /* there is no data blob for this rank yet, so add it. */
        size_t free_offset;
        free_offset = get_free_offset(datadesc);
        offset = put_data_to_the_end(ns_info, datadesc, kval->key, buffer->base_ptr, size);
        if (0 == offset) {
            /* this is an error */
            PMIX_RELEASE(buffer);
            PMIX_ERROR_LOG(PMIX_ERROR);
            return PMIX_ERROR;
        }
        /* if it's the first time when we put data for this rank, then *rinfo == NULL,
         * and even if segment was extended, and data was put into the next segment,
         * we don't need to extension slot at the end of previous segment.
         * If we try, we might overwrite other segments memory,
         * because previous segment is already full. */
        if (free_offset != offset && NULL != *rinfo) {
            /* here we compare previous free offset with the offset where we just put data.
             * It should be equal in the normal case. It it's not true, then it means that
             * segment was extended, and we put data to the next segment, so we now need to
             * put extension slot at the end of previous segment with a "reference" to a new_offset */
            size_t sz = sizeof(size_t);
            addr = _get_data_region_by_offset(datadesc, free_offset);
            strncpy((char *)addr, ESH_REGION_EXTENSION, PMIX_MAX_KEYLEN+1);
            memcpy(addr + PMIX_MAX_KEYLEN + 1, &sz, sizeof(size_t));
            memcpy(addr + PMIX_MAX_KEYLEN + 1 + sizeof(size_t), &offset, sizeof(size_t));
        }
        if (NULL == *rinfo) {
            *rinfo = (rank_meta_info*)malloc(sizeof(rank_meta_info));
            (*rinfo)->rank = rank;
            (*rinfo)->offset = offset;
            (*rinfo)->count = 0;
        }
        (*rinfo)->count++;
    } else if (NULL != *rinfo) {
        /* there is data blob for this rank */
        addr = _get_data_region_by_offset(datadesc, (*rinfo)->offset);
        if (NULL == addr) {
            PMIX_RELEASE(buffer);
            PMIX_ERROR_LOG(PMIX_ERROR);
            return rc;
        }
        /* go through previous data region and find key matches.
         * If one is found, then mark this kval as invalidated.
         * Then put a new empty offset to the next extension slot,
         * and add new kval by this offset.
         * no need to update meta info, it's still the same. */
        kval_cnt = (*rinfo)->count;
        int add_to_the_end = 1;
        while (0 < kval_cnt) {
            /* data is stored in the following format:
             * key[PMIX_MAX_KEYLEN+1]
             * size_t size
             * byte buffer containing pmix_value, should be loaded to pmix_buffer_t and unpacked.
             * next kval pair
             * .....
             * extension slot which has key = EXTENSION_SLOT and a size_t value for offset to next data address for this process.
             */
            if (0 == strncmp((const char *)addr, ESH_REGION_EXTENSION, PMIX_MAX_KEYLEN+1)) {
                memcpy(&offset, addr + PMIX_MAX_KEYLEN + 1 + sizeof(size_t), sizeof(size_t));
                if (0 < offset) {
                    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                                "%s:%d:%s: for rank %u, replace flag %d %s is filled with %lu value",
                                __FILE__, __LINE__, __func__, rank, data_exist, ESH_REGION_EXTENSION, offset));
                    /* go to next item, updating address */
                    addr = _get_data_region_by_offset(datadesc, offset);
                    if (NULL == addr) {
                        PMIX_RELEASE(buffer);
                        PMIX_ERROR_LOG(PMIX_ERROR);
                        return rc;
                    }
                } else {
                    /* should not be, we should be out of cycle when this happens */
                }
            } else if (0 == strncmp((const char *)addr, kval->key, PMIX_MAX_KEYLEN+1)) {
                PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                            "%s:%d:%s: for rank %u, replace flag %d found target key %s",
                            __FILE__, __LINE__, __func__, rank, data_exist, kval->key));
                /* target key is found, compare value sizes */
                size_t cur_size;
                memcpy(&cur_size, addr + PMIX_MAX_KEYLEN + 1, sizeof(size_t));
                if (cur_size != size) {
                //if (1) { /* if we want to test replacing values for existing keys. */
                    /* invalidate current value and store another one at the end of data region. */
                    strncpy((char *)addr, ESH_REGION_INVALIDATED, PMIX_MAX_KEYLEN+1);
                    /* decrementing count, it will be incremented back when we add a new value for this key at the end of region. */
                    (*rinfo)->count--;
                    kval_cnt--;
                    /* go to next item, updating address */
                    addr += KVAL_SIZE(cur_size);
                    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                                "%s:%d:%s: for rank %u, replace flag %d mark key %s regions as invalidated. put new data at the end.",
                                __FILE__, __LINE__, __func__, rank, data_exist, kval->key));
                } else {
                    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                                "%s:%d:%s: for rank %u, replace flag %d replace data for key %s type %d in place",
                                __FILE__, __LINE__, __func__, rank, data_exist, kval->key, kval->value->type));
                    /* replace old data with new one. */
                    addr += PMIX_MAX_KEYLEN + 1;
                    memcpy(addr, &size, sizeof(size_t));
                    addr += sizeof(size_t);
                    memset(addr, 0, cur_size);
                    memcpy(addr, buffer->base_ptr, size);
                    addr += cur_size;
                    add_to_the_end = 0;
                    break;
                }
            } else {
                char ckey[PMIX_MAX_KEYLEN+1] = {0};
                strncpy(ckey, (const char *)addr, PMIX_MAX_KEYLEN+1);
                PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                            "%s:%d:%s: for rank %u, replace flag %d skip %s key, look for %s key",
                            __FILE__, __LINE__, __func__, rank, data_exist, ckey, kval->key));
                /* Skip it: key is "INVALIDATED" or key is valid but different from target one. */
                if (0 != strncmp(ESH_REGION_INVALIDATED, ckey, PMIX_MAX_KEYLEN+1)) {
                    /* count only valid items */
                    kval_cnt--;
                }
                size_t size;
                memcpy(&size, addr + PMIX_MAX_KEYLEN + 1, sizeof(size_t));
                /* go to next item, updating address */
                addr += KVAL_SIZE(size);
            }
        }
        if (1 == add_to_the_end) {
            /* if we get here, it means that we want to add a new item for the target rank, or
             * we mark existing item with the same key as "invalidated" and want to add new item
             * for the same key. */
            (*rinfo)->count++;
            size_t free_offset;
            free_offset = get_free_offset(datadesc);
            /* add to the end */
            offset = put_data_to_the_end(ns_info, datadesc, kval->key, buffer->base_ptr, size);
            if (0 == offset) {
                PMIX_RELEASE(buffer);
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
            /* we just reached the end of data for the target rank, and there can be two cases:
             * (1) - we are in the middle of data segment; data for this rank is separated from
             * data for different ranks, and that's why next element is EXTENSION_SLOT.
             * We put new data to the end of data region and just update EXTENSION_SLOT value by new offset.
             */
            if (0 == strncmp((const char *)addr, ESH_REGION_EXTENSION, PMIX_MAX_KEYLEN+1)) {
                PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                            "%s:%d:%s: for rank %u, replace flag %d %s should be filled with offset %lu value",
                            __FILE__, __LINE__, __func__, rank, data_exist, ESH_REGION_EXTENSION, offset));
                memcpy(addr + PMIX_MAX_KEYLEN + 1 + sizeof(size_t), &offset, sizeof(size_t));
            } else {
                /* (2) - we point to the first free offset, no more data is stored further in this segment.
                 * There is no EXTENSION_SLOT by this addr since we continue pushing data for the same rank,
                 * and there is no need to split it.
                 * But it's possible that we reached the end of current data region and just jumped to the new region
                 * to put new data, in that case free_offset != offset and we must put EXTENSION_SLOT by the current addr
                 * forcibly and store new offset in its value. */
                if (free_offset != offset) {
                    /* segment was extended, need to put extension slot by free_offset indicating new_offset */
                    size_t sz = sizeof(size_t);
                    strncpy((char *)addr, ESH_REGION_EXTENSION, PMIX_MAX_KEYLEN+1);
                    memcpy(addr + PMIX_MAX_KEYLEN + 1, &sz, sz);
                    memcpy(addr + PMIX_MAX_KEYLEN + 1 + sizeof(size_t), &offset, sz);
                }
            }
            PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                        "%s:%d:%s: for rank %u, replace flag %d item not found ext slot empty, put key %s to the end",
                        __FILE__, __LINE__, __func__, rank, data_exist, kval->key));
        }
    }
    buffer->base_ptr = NULL;
    buffer->bytes_used = 0;
    PMIX_RELEASE(buffer);
    return rc;
}

static int _store_data_for_rank(ns_track_elem_t *ns_info, pmix_rank_t rank, pmix_buffer_t *buf)
{
    int rc;
    int32_t cnt;

    pmix_buffer_t *bptr;
    pmix_kval_t *kp;
    seg_desc_t *metadesc, *datadesc;

    rank_meta_info *rinfo = NULL;
    size_t num_elems, free_offset, new_free_offset;
    int data_exist;

    PMIX_OUTPUT_VERBOSE((10, pmix_globals.debug_output,
                         "%s:%d:%s: for rank %u", __FILE__, __LINE__, __func__, rank));

    metadesc = ns_info->meta_seg;
    datadesc = ns_info->data_seg;

    if (NULL == datadesc || NULL == metadesc) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERROR;
    }

    num_elems = *((size_t*)(metadesc->seg_info.seg_base_addr));
    data_exist = 0;
    /* when we don't use linear search (_direct_mode ==0 ) we don't use num_elems field,
     * so anyway try to get rank_meta_info first. */
    if (0 < num_elems || 0 == _direct_mode) {
        /* go through all elements in meta segment and look for target rank. */
        rinfo = _get_rank_meta_info(rank, metadesc);
        if (NULL != rinfo) {
            data_exist = 1;
        }
    }
    /* incoming buffer may contain several inner buffers for different scopes,
     * so unpack these buffers, and then unpack kvals from each modex buffer,
     * storing them in the shared memory dstore.
     */
    cnt = 1;
    free_offset = get_free_offset(datadesc);
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(buf, &bptr, &cnt, PMIX_BUFFER))) {
        cnt = 1;
        kp = PMIX_NEW(pmix_kval_t);
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, kp, &cnt, PMIX_KVAL))) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "pmix: unpacked key %s", kp->key);
            if (PMIX_SUCCESS != (rc = pmix_sm_store(ns_info, rank, kp, &rinfo, data_exist))) {
                PMIX_ERROR_LOG(rc);
            }
            PMIX_RELEASE(kp); // maintain acctg - hash_store does a retain
            cnt = 1;
            kp = PMIX_NEW(pmix_kval_t);
        }
        cnt = 1;
        PMIX_RELEASE(kp);
        PMIX_RELEASE(bptr);  // free's the data region
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }

    /* Check if new data was put at the end of data segment.
     * It's possible that old data just was replaced with new one,
     * in that case we don't reserve space for EXTENSION_SLOT, it's
     * already reserved.
     * */
    new_free_offset = get_free_offset(datadesc);
    if (new_free_offset != free_offset) {
        /* Reserve space for EXTENSION_SLOT at the end of data blob.
         * We need it to split data for one rank from data for different
         * ranks and to allow extending data further.
         * We also put EXTENSION_SLOT at the end of each data segment, and
         * its value points to the beginning of next data segment.
         * */
        rc = put_empty_ext_slot(ns_info->data_seg);
        if (PMIX_SUCCESS != rc) {
            if (NULL != rinfo) {
                free(rinfo);
            }
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    /* if this is the first data posted for this rank, then
     * update meta info for it */
    if (0 == data_exist) {
        set_rank_meta_info(ns_info, rinfo);
        if (NULL != rinfo) {
            free(rinfo);
        }
    }

    return rc;
}

static inline uint32_t _get_univ_size(const char *nspace)
{
    pmix_value_t *val = NULL;
    uint32_t nprocs = 0;
    pmix_nspace_t *ns, *nptr;

    nptr = NULL;
    PMIX_LIST_FOREACH(ns, &pmix_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(nspace, ns->nspace)) {
            nptr = ns;
            break;
        }
    }

    if (nptr && (PMIX_SUCCESS == pmix_hash_fetch(&nptr->internal, PMIX_RANK_WILDCARD, PMIX_UNIV_SIZE, &val))) {
        if (val->type == PMIX_UINT32) {
            nprocs = val->data.uint32;
        }
        PMIX_VALUE_RELEASE(val);
    }

    return nprocs;
}
