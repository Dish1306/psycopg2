/* green.c - cooperation with coroutine libraries.
 *
 * Copyright (C) 2010 Daniele Varrazzo <daniele.varrazzo@gmail.com>
 *
 * This file is part of psycopg.
 *
 * psycopg2 is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link this program with the OpenSSL library (or with
 * modified versions of OpenSSL that use the same license as OpenSSL),
 * and distribute linked combinations including the two.
 *
 * You must obey the GNU Lesser General Public License in all respects for
 * all of the code used other than OpenSSL.
 *
 * psycopg2 is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#define PSYCOPG_MODULE
#include "psycopg/psycopg.h"

#include "psycopg/green.h"
#include "psycopg/connection.h"
#include "psycopg/pqpath.h"


HIDDEN PyObject *wait_callback = NULL;

static PyObject *have_wait_callback(void);
static void psyco_panic_cancel(connectionObject *conn);

/* Register a callback function to block waiting for data.
 *
 * The function is exported by the _psycopg module.
 */
PyObject *
psyco_set_wait_callback(PyObject *self, PyObject *obj)
{
    Py_XDECREF(wait_callback);

    if (obj != Py_None) {
        wait_callback = obj;
        Py_INCREF(obj);
    }
    else {
        wait_callback = NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}


/* Return the currently registered wait callback function.
 *
 * The function is exported by the _psycopg module.
 */
PyObject *
psyco_get_wait_callback(PyObject *self, PyObject *obj)
{
    PyObject *ret;

    ret = wait_callback;
    if (!ret) {
        ret = Py_None;
    }

    Py_INCREF(ret);
    return ret;
}


/* Return nonzero if a wait callback should be called. */
int
psyco_green()
{
#ifdef PSYCOPG_EXTENSIONS
    return (NULL != wait_callback);
#else
    return 0;
#endif
}

/* Return the wait callback if available.
 *
 * If not available, set a Python exception and return.
 *
 * The function returns a new reference: decref after use.
 */
static PyObject *
have_wait_callback()
{
    PyObject *cb;

    cb = wait_callback;
    if (!cb) {
        PyErr_SetString(OperationalError, "wait callback not available");
        return NULL;
    }
    Py_INCREF(cb);
    return cb;
}

/* Block waiting for data available in an async connection.
 *
 * This function assumes `wait_callback` to be available:
 * raise `InterfaceError` if it is not. Use `psyco_green()` to check if
 * the function is to be called.
 *
 * Return 0 on success, else nonzero and set a Python exception.
 */
int
psyco_wait(connectionObject *conn)
{
    PyObject *rv;
    PyObject *cb;

    Dprintf("psyco_wait");
    if (!(cb = have_wait_callback())) {
        return -1;
    }

    rv = PyObject_CallFunctionObjArgs(cb, conn, NULL);
    Py_DECREF(cb);

    if (NULL != rv) {
        Py_DECREF(rv);
        return 0;
    } else {
        Dprintf("psyco_wait: error in wait callback");
        return -1;
    }
}

/* Replacement for PQexec using the user-provided wait function.
 *
 * The function should be called helding the connection lock, and
 * the GIL because some Python code is expected to be called.
 *
 * If PGresult is NULL, there may have been either a libpq error
 * or an exception raised by Python code: before raising an exception
 * check if there is already one using `PyErr_Occurred()` */
PGresult *
psyco_exec_green(connectionObject *conn, const char *command)
{
    PGresult *result = NULL;

    /* Check that there is a single concurrently executing query */
    if (conn->async_cursor) {
        PyErr_SetString(ProgrammingError,
            "a single async query can be executed on the same connection");
        goto end;
    }
    /* we don't care about which cursor is executing the query, and
     * it may also be that no cursor is involved at all and this is
     * an internal query. So just store anything in the async_cursor,
     * respecting the code expecting it to be a weakref */
    if (!(conn->async_cursor = PyWeakref_NewRef((PyObject*)conn, NULL))) {
        goto end;
    }

    /* Send the query asynchronously */
    if (0 == pq_send_query(conn, command)) {
        goto end;
    }

    /* Enter the poll loop with a write. When writing is finished the poll
       implementation will set the status to ASYNC_READ without exiting the
       loop. If read is finished the status is finally set to ASYNC_DONE.
    */
    conn->async_status = ASYNC_WRITE;

    if (0 != psyco_wait(conn)) {
        psyco_panic_cancel(conn);
        goto end;
    }

    /* Now we can read the data without fear of blocking. */
    result = pq_get_last_result(conn);

end:
    conn->async_status = ASYNC_DONE;
    Py_CLEAR(conn->async_cursor);
    return result;
}


/* There has been a communication error during query execution. It may have
 * happened e.g. for a network error or an error in the callback, and we
 * cannot tell the two apart. The strategy here to avoid blocking (issue #113)
 * is to try and cancel the query, waiting for the result in non-blocking way.
 * If again we receive an error, we raise an error and close the connection.
 * Discard the result of the currenly executed query, blocking.
 */
static void
psyco_panic_cancel(connectionObject *conn)
{
    PGresult *res;
    PyObject *etype, *evalue, *etb;
    char errbuf[256];

    /* we should have an exception set. */
    PyErr_Fetch(&etype, &evalue, &etb);
    if (NULL == etype) {
        Dprintf("panic_cancel: called without exception set");
    }

    /* Try sending the cancel signal */
    Dprintf("panic_cancel: sending cancel request");
    if (PQcancel(conn->cancel, errbuf, sizeof(errbuf)) == 0) {
        Dprintf("panic_cancel: canceling failed: %s", errbuf);
        /* raise a warning: we'll keep the previous error */
        PyErr_WarnEx(NULL, errbuf, 1);
        goto exit;
    }

    /* go back in the loop for another attempt at async processing */
    /* TODO: should we start on ASYNC_WRITE instead? */
    if (0 != psyco_wait(conn)) {
        Dprintf("panic_cancel: error after cancel: closing the connection");
        PyErr_WarnEx(NULL, "async cancel failed: closing the connection", 1);
        conn_close_locked(conn);
        goto exit;
    }

    /* we must clear the result or we get "another command is already in
     * progress" */
    if (NULL != (res = pq_get_last_result(conn))) {
        PQclear(res);
    }

exit:
    /* restore the exception. If no exception was set at function begin, don't
     * clobber one that may have been set here. */
    if (etype) {
        PyErr_Restore(etype, evalue, etb);
    }
}
