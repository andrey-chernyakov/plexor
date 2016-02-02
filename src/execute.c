#include "plexor.h"


static int epoll_fd = -1;
// fixme close(epoll_fd) нигде не делается;

/* Initialize plexor function cache */
void
execute_init(void)
{
    if (epoll_fd == -1)
        epoll_fd = epoll_create(1);
}

static void
pg_result_error(PGresult *pg_result)
{
    const char *diag_sqlstate = PQresultErrorField(pg_result, PG_DIAG_SQLSTATE);
    const char *diag_primary  = PQresultErrorField(pg_result, PG_DIAG_MESSAGE_PRIMARY);
    const char *diag_detail   = PQresultErrorField(pg_result, PG_DIAG_MESSAGE_DETAIL);
    const char *diag_context  = PQresultErrorField(pg_result, PG_DIAG_CONTEXT);
    const char *diag_hint     = PQresultErrorField(pg_result, PG_DIAG_MESSAGE_HINT);
    int         sqlstate;

    if (diag_sqlstate)
        sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
                                 diag_sqlstate[1],
                                 diag_sqlstate[2],
                                 diag_sqlstate[3],
                                 diag_sqlstate[4]);
    else
        sqlstate = ERRCODE_CONNECTION_FAILURE;

    PQclear(pg_result);
    ereport(ERROR,
            (errcode(sqlstate),
             errmsg("Remote error: %s", diag_primary),
             diag_detail  ? errdetail("Remote detail: %s", diag_detail) : 0,
             diag_hint ? errhint("Remote hint: %s", diag_hint) : 0,
             diag_context ? errcontext("Remote context: %s", diag_context) : 0));
}

static void
wait_for_flush(PlxFn *plx_fn, PGconn *pq_conn)
{
    struct epoll_event listenev;
    struct epoll_event event;
    int                res;

    res = PQflush(pq_conn);
    if (!res)
        return;
    if (res == -1)
        plx_error(plx_fn, "PQflush error %s", PQerrorMessage(pq_conn));

    listenev.events = EPOLLOUT;
    listenev.data.fd = PQsocket(pq_conn);

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listenev.data.fd, &listenev) < 0)
        plx_error(plx_fn, "epoll: socket adding failed");

    while (res)
    {
        CHECK_FOR_INTERRUPTS();
        epoll_wait(epoll_fd, &event, 1, 1);
        res = PQflush(pq_conn);
        if (res == -1)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listenev.data.fd, &listenev);
            plx_error(plx_fn, "%s", PQerrorMessage(pq_conn));
        }
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listenev.data.fd, &listenev);
}

static int
is_pq_busy(PGconn *pq_conn)
{
    return PQconsumeInput(pq_conn) ? PQisBusy(pq_conn) : -1;
}

static PGresult*
wait_for_result(PlxFn *plx_fn, PlxConn *plx_conn)
{
    struct epoll_event  listenev;
    struct epoll_event  event;
    PGconn             *pq_conn   = plx_conn->pq_conn;
    PGresult           *pg_result = NULL;

    listenev.events = EPOLLIN;
    listenev.data.fd = PQsocket(pq_conn);

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listenev.data.fd, &listenev) < 0)
        plx_error(plx_fn, "epoll: socket adding failed");

    PG_TRY();
    {
        int tmp;

        while ((tmp = is_pq_busy(pq_conn)))
        {
            if (tmp == -1)
            {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listenev.data.fd, &listenev);
                plx_error(plx_fn, "%s", PQerrorMessage(pq_conn));
            }
            CHECK_FOR_INTERRUPTS();
            epoll_wait(epoll_fd, &event, 1, 10000);
        }
    }
    PG_CATCH();
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listenev.data.fd, &listenev);

        if (geterrcode() == ERRCODE_QUERY_CANCELED)
            PQrequestCancel(pq_conn);
        pg_result = PQgetResult(pq_conn);
        if (pg_result)
            PQclear(pg_result);
        PG_RE_THROW();
    }
    PG_END_TRY();

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listenev.data.fd, &listenev);
    return PQgetResult(pq_conn);
}

static PGresult*
get_pg_result(PlxFn *plx_fn, PlxConn *plx_conn)
{
    PGresult *pg_result     = NULL;
    PGresult *tmp_pg_result = NULL;

    while((tmp_pg_result = wait_for_result(plx_fn, plx_conn)))
    {
        ExecStatusType status = PQresultStatus(tmp_pg_result);
        if (status == PGRES_TUPLES_OK)
        {
            if (pg_result)
            {
                PQclear(pg_result);
                PQclear(tmp_pg_result);
                plx_error(plx_fn, "second pg_result???");
            }
            else
                pg_result = tmp_pg_result;
        }
        else
        {
            if (pg_result)
                PQclear(pg_result);
            delete_plx_conn(plx_conn);
            pg_result_error(tmp_pg_result);
        }
    }
    return pg_result;
}

static void
plx_send_query(PlxFn    *plx_fn,
               PlxConn  *plx_conn,
               char     *sql,
               char    **args,
               int       nargs,
               int      *arg_lens,
               int      *arg_fmts)
{
    if (!PQsendQueryParams(plx_conn->pq_conn,
                           (const char *) sql,
                           nargs,
                           NULL,
                           (const char * const*) args,
                           arg_lens,
                           arg_fmts,
                           plx_fn->is_binary))
    {
        delete_plx_conn(plx_conn);
        plx_error(plx_fn,
                  "failed to send query %s %s", sql, PQerrorMessage(plx_conn->pq_conn));
    }
    wait_for_flush(plx_fn, plx_conn->pq_conn);
}

static void
create_fn_args(PlxFn*              plx_fn,
               FunctionCallInfo    fcinfo,
               char             ***args,
               int               **arg_lens,
               int               **arg_fmts)
{
    PlxQuery  *plx_q = plx_fn->run_query;

    (* args)     = palloc0(sizeof(char *) * plx_q->nargs);
    (* arg_lens) = palloc0(sizeof(int)    * plx_q->nargs);
    (* arg_fmts) = palloc0(sizeof(int)    * plx_q->nargs);

    if (plx_fn->is_binary)
    {
        bytea *bin;
        for (int i = 0; i < plx_q->nargs; i++)
        {
            int idx = plx_q->plx_fn_arg_indexes[i];

            bin = SendFunctionCall(&plx_fn->arg_types[idx]->send_fn, PG_GETARG_DATUM(idx));
            (* args)[i]     = VARDATA(bin);
            (* arg_lens)[i] = VARSIZE(bin) - VARHDRSZ;
            (* arg_fmts)[i] = 1;
        }
    }
    else
    {
        for (int i = 0; i < plx_q->nargs; i++)
        {
            int idx = plx_q->plx_fn_arg_indexes[i];
            if (PG_ARGISNULL(idx))
                (* args)[i] = NULL;
            else
                (* args)[i] = OutputFunctionCall(
                    &plx_fn->arg_types[idx]->output_fn,
                    PG_GETARG_DATUM(idx));
            (* arg_lens)[i] = 0;
            (* arg_fmts)[i] = 0;
        }
    }
}

static StringInfo
get_dymanic_record_fields(PlxFn *plx_fn, FunctionCallInfo fcinfo)
{
    StringInfo buf;
    Oid        oid;
    TupleDesc  tuple_desc;

    get_call_result_type(fcinfo, &oid, &tuple_desc);

    buf = makeStringInfo();
    for (int i = 0; i < tuple_desc->natts; i++)
    {
        Form_pg_attribute a;
        HeapTuple         type_tuple;
        Form_pg_type      type_struct;

        a = tuple_desc->attrs[i];
        type_tuple = SearchSysCache(TYPEOID, ObjectIdGetDatum(a->atttypid), 0, 0, 0);
        if (!HeapTupleIsValid(type_tuple))
            plx_error(plx_fn, "cache lookup failed for type %u", a->atttypid);
        type_struct = (Form_pg_type) GETSTRUCT(type_tuple);
        {
            appendStringInfo(
                buf,
                "%s%s %s",
                ((i > 0) ? ", " : ""),
                quote_identifier(NameStr(a->attname)),
                quote_identifier(NameStr(type_struct->typname)));
        }
        ReleaseSysCache(type_tuple);
    }
    return buf;
}

static void
prepare_execute(PlxFn              *plx_fn,
                FunctionCallInfo    fcinfo,
                StringInfo         *sql,
                char             ***args,
                int               **arg_lens,
                int               **arg_fmts)
{
    PlxQuery      *plx_q = plx_fn->run_query;
    create_fn_args(plx_fn, fcinfo, args, arg_lens, arg_fmts);
    *sql = makeStringInfo();
    if (plx_fn->is_untyped_record)
    {
        StringInfo buf = get_dymanic_record_fields(plx_fn, fcinfo);
        appendStringInfo(*sql, UNTYPED_SQL_TMPL, plx_q->sql->data, buf->data);
    }
    else
        appendStringInfo(*sql, TYPED_SQL_TMPL, plx_q->sql->data);
}

void
remote_execute(PlxConn *plx_conn, PlxFn *plx_fn, FunctionCallInfo fcinfo)
{
    PlxQuery    *plx_q    = plx_fn->run_query;
    char       **args     = NULL;
    int         *arg_lens = NULL;
    int         *arg_fmts = NULL;
    StringInfo   sql;

    /* memory will be alloced in ExprContext - not necessary to free it */
    prepare_execute(plx_fn, fcinfo, &sql, &args, &arg_lens, &arg_fmts);
    start_transaction(plx_conn);
    plx_send_query(plx_fn, plx_conn, sql->data, args, plx_q->nargs, arg_lens, arg_fmts);
    /* code below will be never executed if pg_result is wrong */
    plx_result_insert_cache(fcinfo, plx_fn, get_pg_result(plx_fn, plx_conn));
}