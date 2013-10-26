
#include "ts_lua_util.h"

#define TS_LUA_FUNCTION_HTTP_INTERCEPT            "do_intercept"

static int ts_lua_http_intercept(lua_State *L);
static int ts_lua_http_intercept_entry(TSCont contp, TSEvent event, void *edata);
static void ts_lua_http_intercept_process(ts_lua_http_ctx *http_ctx, TSVConn conn);
static void ts_lua_http_intercept_setup_read(ts_lua_http_intercept_ctx *ictx);
static void ts_lua_http_intercept_setup_write(ts_lua_http_intercept_ctx *ictx);
static int ts_lua_http_intercept_handler(TSCont contp, TSEvent event, void *edata);
static int ts_lua_http_intercept_run_coroutine(ts_lua_http_intercept_ctx *ictx);
static int ts_lua_http_intercept_process_read(TSEvent event, ts_lua_http_intercept_ctx *ictx);
static int ts_lua_http_intercept_process_write(TSEvent event, ts_lua_http_intercept_ctx *ictx);


void
ts_lua_inject_http_intercept_api(lua_State *L)
{
    lua_pushcfunction(L, ts_lua_http_intercept);
    lua_setfield(L, -2, "intercept");
}

static int
ts_lua_http_intercept(lua_State *L)
{
    TSCont              contp;
    int                 type;
    ts_lua_http_ctx     *http_ctx;

    http_ctx = ts_lua_get_http_ctx(L);

    type = lua_type(L, 1);

    if (type != LUA_TFUNCTION) {
        fprintf(stderr, "[%s] param in ts.http.intercept should be a function\n", __FUNCTION__);
        return 0;
    }

    lua_pushvalue(L, 1);
    lua_setglobal(L, TS_LUA_FUNCTION_HTTP_INTERCEPT);

    contp = TSContCreate(ts_lua_http_intercept_entry, TSMutexCreate());
    TSContDataSet(contp, http_ctx);
    TSHttpTxnIntercept(contp, http_ctx->txnp);

    return 0;
}

static int
ts_lua_http_intercept_entry(TSCont contp, TSEvent event, void *edata)
{
    switch (event) {

        case TS_EVENT_NET_ACCEPT_FAILED:
            if (edata)
                TSVConnClose((TSVConn)edata);
            break;

        case TS_EVENT_NET_ACCEPT:
            ts_lua_http_intercept_process((ts_lua_http_ctx*)TSContDataGet(contp), (TSVConn)edata);
            break;

        default:
            break;
    }

    TSContDestroy(contp);
    return 0;
}

static void
ts_lua_http_intercept_process(ts_lua_http_ctx *http_ctx, TSVConn conn)
{
    TSCont          contp;
    lua_State       *l;
    ts_lua_http_intercept_ctx   *ictx;

    ictx = ts_lua_create_http_intercept_ctx(http_ctx);

    contp = TSContCreate(ts_lua_http_intercept_handler, TSMutexCreate());
    TSContDataSet(contp, ictx);

    ictx->contp = contp;
    ictx->net_vc = conn;

    l = ictx->lua;

    // set up read.
    ts_lua_http_intercept_setup_read(ictx);

    // invoke function here
    lua_getglobal(l, TS_LUA_FUNCTION_HTTP_INTERCEPT);
    ts_lua_http_intercept_run_coroutine(ictx);
}

static void
ts_lua_http_intercept_setup_read(ts_lua_http_intercept_ctx *ictx)
{
    ictx->input.buffer = TSIOBufferCreate();
    ictx->input.reader = TSIOBufferReaderAlloc(ictx->input.buffer);
    ictx->input.vio = TSVConnRead(ictx->net_vc, ictx->contp, ictx->input.buffer, INT64_MAX);
}

static void
ts_lua_http_intercept_setup_write(ts_lua_http_intercept_ctx *ictx)
{
    ictx->output.buffer = TSIOBufferCreate();
    ictx->output.reader = TSIOBufferReaderAlloc(ictx->output.buffer);
    ictx->output.vio = TSVConnWrite(ictx->net_vc, ictx->contp, ictx->output.reader, INT64_MAX);
}

static int
ts_lua_http_intercept_handler(TSCont contp, TSEvent event, void *edata)
{
    int     ret;
    ts_lua_http_intercept_ctx *ictx = (ts_lua_http_intercept_ctx*)TSContDataGet(contp);

    if (edata == ictx->input.vio) {
        ret = ts_lua_http_intercept_process_read(event, ictx);

    } else if (edata == ictx->output.vio) {
        ret = ts_lua_http_intercept_process_write(event, ictx);

    } else {
        ret = ts_lua_http_intercept_run_coroutine(ictx);
    }

    if (ret || (ictx->send_complete && ictx->recv_complete)) {
        TSContDestroy(contp);
        ts_lua_destroy_http_intercept_ctx(ictx);
    }

    return 0;
}

static int
ts_lua_http_intercept_run_coroutine(ts_lua_http_intercept_ctx *ictx)
{
    int             ret;
    const char      *res;
    size_t          res_len;
    lua_State       *L;

    L = ictx->lua;

    ret = lua_resume(L, 0);

    switch (ret) {

        case 0:             // finished
            res = lua_tolstring(L, -1, &res_len);
            ts_lua_http_intercept_setup_write(ictx);
            TSIOBufferWrite(ictx->output.buffer, res, res_len);
            TSVIONBytesSet(ictx->output.vio, res_len);
            break;

        case 1:             // yield
            break;

        default:            // error
            fprintf(stderr, "lua_resume failed: %s\n", lua_tostring(L, -1));
            return -1;
    }

    return 0;
}

static int
ts_lua_http_intercept_process_read(TSEvent event, ts_lua_http_intercept_ctx *ictx)
{
    int64_t avail = TSIOBufferReaderAvail(ictx->input.reader);
    TSIOBufferReaderConsume(ictx->input.reader, avail);
    
    switch (event) {
        case TS_EVENT_VCONN_READ_READY:
            TSVIOReenable(ictx->input.vio);
            break;

        case TS_EVENT_VCONN_READ_COMPLETE:
        case TS_EVENT_VCONN_EOS:
            ictx->recv_complete = 1;
            break;

        default:
            return -1;
    }

    return 0;
}

static int
ts_lua_http_intercept_process_write(TSEvent event, ts_lua_http_intercept_ctx *ictx)
{
    switch (event) {
        case TS_EVENT_VCONN_WRITE_READY:
            if (TSIOBufferReaderAvail(ictx->output.reader))
                TSVIOReenable(ictx->output.vio);
            break;

        case TS_EVENT_VCONN_WRITE_COMPLETE:
            ictx->send_complete = 1;
            break;

        case TS_EVENT_ERROR:
        default:
            return -1; 
    }   

    return 0;
}

