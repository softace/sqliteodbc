/*
 *  Adapted from JDBCBench.java
 */

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

/* tpc bm b scaling rules */
static int tps = 1;		/* the tps scaling factor: here it is 1 */
static int nbranches = 1;	/* number of branches in 1 tps db */
static int ntellers = 10;	/* number of tellers in  1 tps db */
static int naccounts = 100000;	/* number of accounts in 1 tps db */

static char *dsn = NULL;

#define TELLER 0
#define BRANCH 1
#define ACCOUNT 2

static int *failed_transactions = NULL;
static int *transaction_count = NULL;
static int n_clients = 10;
static int n_txn_per_client = 10;

#ifdef _WIN32
static int start_time;
#else
static struct timeval start_time;
#endif
static int transactions = 0;
static int verbose = 0;

#ifdef _WIN32
static int shm[200];
#else
static int shmid;
static int *shm = NULL;
#endif

static void incrementTransactionCount()
{
    if (transaction_count) {
        (*transaction_count)++;
    }
}

static void incrementFailedTransactionCount()
{
    if (failed_transactions) {
        (*failed_transactions)++;
    }
}

static int getRandomInt(int lo, int hi)
{
    int ret = 0;

    ret = rand() % (hi - lo + 1);
    ret += lo;
    return ret;
}

static int getRandomID(int type)
{
    int min, max, num;

    max = min = 0;
    num = naccounts;
    switch(type) {
    case TELLER:
        min += nbranches;
	num = ntellers;
	/* FALLTHROUGH */
    case BRANCH:
        if (type == BRANCH) {
	    num = nbranches;
	}
	min += naccounts;
	/* FALLTHROUGH */
    case ACCOUNT:
        max = min + num - 1;
    }
    return (getRandomInt(min, max));
}

static void reportDone()
{
#ifdef _WIN32
    int end_time;
#else
    struct timeval end_time;
#endif
    double completion_time;
    double rate;

#ifdef _WIN32
    end_time = GetTickCount();
    completion_time = (double) (end_time - start_time) * 0.001;
#else
    gettimeofday(&end_time, NULL);
    completion_time = (double) end_time.tv_sec +
		      0.000001 * end_time.tv_usec -
		      ((double) start_time.tv_sec +
		       0.000001 * start_time.tv_usec);
#endif
    fprintf(stdout, "Benchmark Report\n");
    fprintf(stdout, "Featuring ");
    fprintf(stdout, "<direct queries> ");
    if (transactions < -1) {
        fprintf(stdout, "<commit each 100 transactions> ");
    } else if (transactions < 0) {
        fprintf(stdout, "<one big transaction> ");
    } else if (transactions > 0) {
        fprintf(stdout, "<transactions> ");
    } else {
        fprintf(stdout, "<auto-commit> ");
    }
    fprintf(stdout, "\n--------------------\n");
    fprintf(stdout, "Time to execute %d transactions: %g seconds.\n",
	    *transaction_count, completion_time);
    fprintf(stdout, "%d/%d failed complete.\n",
	    *failed_transactions, *transaction_count);
    rate = (*transaction_count - *failed_transactions) / completion_time;
    fprintf(stdout, "Transaction rate: %g txn/sec.\n", rate);
    fflush(stdout);
    *transaction_count = 0;
    *failed_transactions = 0;
}

static void createDatabase()
{
    int dotrans = 0, i;
    long accountsnb = 0;
    SQLHENV env;
    SQLHDBC dbc;
    SQLRETURN rc;
    SQLHSTMT s;
    char sqlbuf[1024];

    rc = SQLAllocEnv(&env);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "AllocEnv failed\n");
	exit(1);
    }
    rc = SQLAllocConnect(env, &dbc);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "AllocConnect failed\n");
	exit(1);
    }
    rc = SQLDriverConnect(dbc, NULL, (SQLCHAR *) dsn, SQL_NTS, NULL, 0, NULL,
			  SQL_DRIVER_COMPLETE | SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "DriverConnect failed\n");
	exit(1);
    }
    rc = SQLSetConnectOption(dbc, SQL_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF);
    if (SQL_SUCCEEDED(rc)) {
        dotrans = 1;
    }
    rc = SQLAllocStmt(dbc, &s);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "AllocStmt failed\n");
	exit(1);
    }
    rc = SQLExecDirect(s, (SQLCHAR *) "SELECT count(*) FROM accounts",
		       SQL_NTS);
    if (SQL_SUCCEEDED(rc)) {
        rc = SQLFetch(s);
	if (SQL_SUCCEEDED(rc)) {
	    rc = SQLGetData(s, 1, SQL_C_LONG, &accountsnb,
			    sizeof (accountsnb), NULL);
	    if (SQL_SUCCEEDED(rc)) {
	        if (dotrans) {
		    SQLTransact(NULL, dbc, SQL_COMMIT);
		}
		SQLFreeStmt(s, SQL_DROP);
		if (accountsnb == naccounts * tps) {
		    fprintf(stdout, "Already initialized\n");
		    fflush(stdout);
		    goto done;
		}
	    }
	}
	SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    SQLFreeStmt(s, SQL_CLOSE);
    fprintf(stdout, "Drop old tables if they exist\n");
    fflush(stdout);
    rc = SQLExecDirect(s, (SQLCHAR *) "DROP TABLE history", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    if (SQL_SUCCEEDED(rc) && dotrans) {
	SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    rc = SQLExecDirect(s, (SQLCHAR *) "DROP TABLE accounts", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    if (SQL_SUCCEEDED(rc) && dotrans) {
	SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    rc = SQLExecDirect(s, (SQLCHAR *) "DROP TABLE tellers", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    if (SQL_SUCCEEDED(rc) && dotrans) {
	SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    rc = SQLExecDirect(s, (SQLCHAR *) "DROP TABLE branches", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    if (SQL_SUCCEEDED(rc) && dotrans) {
	SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    fprintf(stdout, "Create tables\n");
    fflush(stdout);
    rc = SQLExecDirect(s, (SQLCHAR *)
		       "CREATE TABLE branches ("
		       "Bid INTEGER NOT NULL PRIMARY KEY, "
		       "Bbalance INTEGER, "
		       "filler CHAR(88))", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    rc = SQLExecDirect(s, (SQLCHAR *)
		       "CREATE TABLE tellers ("
		       "Tid INTEGER NOT NULL PRIMARY KEY, "
		       "Bid INTEGER, "
		       "Tbalance INTEGER, "
		       "filler CHAR(84))", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    rc = SQLExecDirect(s, (SQLCHAR *)
		       "CREATE TABLE accounts ("
		       "Aid INTEGER NOT NULL PRIMARY KEY, "
		       "Bid INTEGER, "
		       "Abalance INTEGER, "
		       "filler CHAR(84))", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    rc = SQLExecDirect(s, (SQLCHAR *)
		       "CREATE TABLE history ("
		       "Tid INTEGER, "
		       "Bid INTEGER, "
		       "Aid INTEGER, "
		       "delta INTEGER, "
		       "tstime TIMESTAMP, "
		       "filler CHAR(22))", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    fprintf(stdout, "Delete elements in table in case DROP didn't work\n");
    fflush(stdout);
    rc = SQLExecDirect(s, (SQLCHAR *) "DELETE FROM history", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    rc = SQLExecDirect(s, (SQLCHAR *) "DELETE FROM accounts", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    rc = SQLExecDirect(s, (SQLCHAR *) "DELETE FROM tellers", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    rc = SQLExecDirect(s, (SQLCHAR *) "DELETE FROM branches", SQL_NTS);
    SQLFreeStmt(s, SQL_CLOSE);
    if (dotrans) {
	SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    fprintf(stdout, "Insert data in branches table\n");
    fflush(stdout);
    for (i = 0; i < nbranches * tps; i++) {
        sprintf(sqlbuf, "INSERT INTO branches(Bid,Bbalance) "
		"VALUES (%d,0)", i);
	rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
	SQLFreeStmt(s, SQL_CLOSE);
	if (i % 100 == 0 && dotrans) {
	    SQLTransact(NULL, dbc, SQL_COMMIT);
	}
    }
    if (dotrans) {
        SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    fprintf(stdout, "Insert data in tellers table\n");
    fflush(stdout);
    for (i = 0; i < ntellers * tps; i++) {
        sprintf(sqlbuf, "INSERT INTO tellers(Tid,Bid,Tbalance) "
		"VALUES (%d,%d,0)", i, i / ntellers);
	rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
	SQLFreeStmt(s, SQL_CLOSE);
	if (i % 100 == 0 && dotrans) {
	    SQLTransact(NULL, dbc, SQL_COMMIT);
	}
    }
    if (dotrans) {
        SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    fprintf(stdout, "Insert data in accounts table\n");
    fflush(stdout);
    for (i = 0; i < naccounts * tps; i++) {
        sprintf(sqlbuf, "INSERT INTO accounts(Aid,Bid,Abalance) "
		"VALUES (%d,%d,0)", i, i / naccounts);
	rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
	SQLFreeStmt(s, SQL_CLOSE);
	if (i % 10000 == 0 && dotrans) {
	    SQLTransact(NULL, dbc, SQL_COMMIT);
	}
	if (i > 0 && i % 10000 == 0) {
	    fprintf(stdout,"\t%d\trecords inserted\n", i);
	    fflush(stdout);
	}
    }
    if (dotrans) {
        SQLTransact(NULL, dbc, SQL_COMMIT);
    }
    fprintf(stdout, "\t%d\trecords inserted\n", naccounts * tps);
    fflush(stdout);
    SQLFreeStmt(s, SQL_DROP);
done:
    SQLDisconnect(dbc);
    SQLFreeConnect(dbc);
    SQLFreeEnv(env);
}

static void doOne(HDBC dbc, int bid, int tid, int aid, int delta)
{
    long aBalance = 0;
    SQLRETURN rc;
    SQLHSTMT s;
    SQLSMALLINT ncols;
    char sqlbuf[1024];

    if (dbc == SQL_NULL_HDBC) {
        incrementFailedTransactionCount();
	return;
    }
    rc = SQLAllocStmt(dbc, &s);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#1 rc=%d\n", rc);
	}
        return;
    }
    sprintf(sqlbuf, "UPDATE accounts "
	    "SET Abalance = Abalance + %d WHERE Aid = %d",
	    delta, aid);
    rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#2 rc=%d\n", rc);
	}
        goto transfail;
    }
    SQLFreeStmt(s, SQL_CLOSE);
    sprintf(sqlbuf, "SELECT Abalance "
	    "FROM accounts WHERE Aid = %d", aid);
    rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#3 rc=%d\n", rc);
	}
        goto transfail;
    }
    rc = SQLNumResultCols(s, &ncols);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#4 rc=%d\n", rc);
	}
        goto transfail;
    }
    rc = SQLFetch(s);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#5 rc=%d\n", rc);
	}
        goto transfail;
    }
    rc = SQLGetData(s, 1, SQL_C_LONG, &aBalance, sizeof (aBalance),
		    NULL);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#6 rc=%d\n", rc);
	}
        goto transfail;
    }
    SQLFreeStmt(s, SQL_CLOSE);
    sprintf(sqlbuf, "UPDATE tellers "
	    "SET Tbalance = Tbalance + %d WHERE Tid = %d",
	    delta, tid);
    rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#7 rc=%d\n", rc);
	}
        goto transfail;
    }
    SQLFreeStmt(s, SQL_CLOSE);
    sprintf(sqlbuf, "UPDATE branches "
	    "SET Bbalance = Bbalance + %d WHERE Bid = %d",
	    delta, bid);
    rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#8 rc=%d\n", rc);
	}
        goto transfail;
    }
    SQLFreeStmt(s, SQL_CLOSE);
    sprintf(sqlbuf, "INSERT INTO history"
	    "(Tid, Bid, Aid, delta) VALUES"
	    "(%d, %d, %d, %d)",
	    tid, bid, aid, delta);
    rc = SQLExecDirect(s, (SQLCHAR *) sqlbuf, SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
	if (verbose) {
	    fprintf(stderr, "doOne: fail#9 rc=%d\n", rc);
	}
        goto transfail;
    }
    SQLFreeStmt(s, SQL_CLOSE);
    if (transactions > 0) {
        rc = SQLTransact(NULL, dbc, SQL_COMMIT);
	if (!SQL_SUCCEEDED(rc)) {
	    if (verbose) {
	       fprintf(stderr, "doOne: fail#10 rc=%d\n", rc);
	    }
	    goto transfail;
	}
    }
    SQLFreeStmt(s, SQL_DROP);
    return;
transfail:
    incrementFailedTransactionCount();
    SQLTransact(NULL, dbc, SQL_ROLLBACK);
    SQLFreeStmt(s, SQL_DROP);
}

#ifdef _WIN32
static unsigned __stdcall runClientThread(void *args)
#else
static int runClientThread(void *args)
#endif
{
    SQLHENV env;
    SQLHDBC dbc;
    SQLRETURN rc;
    int ntrans = n_txn_per_client;

    rc = SQLAllocEnv(&env);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "AllocEnv failed\n");
	return 1;
    }
    rc = SQLAllocConnect(env, &dbc);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "AllocConnect failed\n");
	return 1;
    }
    rc = SQLDriverConnect(dbc, NULL, (SQLCHAR *) dsn, SQL_NTS, NULL, 0, NULL,
			  SQL_DRIVER_COMPLETE | SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
        fprintf(stderr, "DriverConnect failed\n");
	return 1;
    }
    if (transactions) {
        rc = SQLSetConnectOption(dbc, SQL_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF);
	if (!SQL_SUCCEEDED(rc)) {
	    fprintf(stderr, "SetConnectOption(autocommit=off) failed\n");
	    return 1;
	}
    }
    while (ntrans-- > 0) {
        int account = getRandomID(ACCOUNT);
	int branch  = getRandomID(BRANCH);
	int teller  = getRandomID(TELLER);
	int delta   = getRandomInt(0,1000);
	doOne(dbc, branch, teller, account, delta);
	if (transactions < -1 && ntrans > 0 && ntrans % 100 == 0) {
	    rc = SQLTransact(NULL, dbc, SQL_COMMIT);
	    if (!SQL_SUCCEEDED(rc)) {
	        if (verbose) {
		    fprintf(stderr, "runClientThread: COMMIT failed rc=%d\n",
			    rc);
		}
		incrementFailedTransactionCount();
		SQLTransact(NULL, dbc, SQL_ROLLBACK);
	    }
	}
	incrementTransactionCount();
    }
    if (transactions < 0) {
        rc = SQLTransact(NULL, dbc, SQL_COMMIT);
	if (!SQL_SUCCEEDED(rc)) {
	    if (verbose) {
	       fprintf(stderr, "runClientThread: final COMMIT failed rc=%d\n",
		       rc);
	    }
	    incrementFailedTransactionCount();
	    SQLTransact(NULL, dbc, SQL_ROLLBACK);
	}
    }
    SQLDisconnect(dbc);
    SQLFreeConnect(dbc);
    SQLFreeEnv(env);
    return 0;
}


int main(int argc, char **argv)
{
    int init_db = 0, i;
#ifdef _WIN32
    HANDLE *pids;
#else
    pid_t *pids;
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-clients") == 0) {
	    if (i + 1 < argc) {
	        i++;
		n_clients = strtol(argv[i], NULL, 0);
	    }
        } else if (strcmp(argv[i], "-dsn") == 0) {
	    if (i + 1 < argc) {
	        i++;
		dsn = argv[i];
	    }
        } else if (strcmp(argv[i], "-tpc") == 0) {
	    if (i + 1 < argc) {
	        i++;
		n_txn_per_client = strtol(argv[i], NULL, 0);
	    }
        } else if (strcmp(argv[i], "-init") == 0) {
            init_db = 1;
        } else if (strcmp(argv[i], "-tps") == 0) {
	    if (i + 1 < argc) {
	        i++;
		tps = strtol(argv[i], NULL, 0);
	    }
        } else if (strcmp(argv[i], "-v") == 0) {
	    verbose++;
	}
    }
    if (dsn == NULL) {
        fprintf(stderr, "usage: %s -dsn DSN [-v] [-init] "
		"[-tpc n] [-clients]\n\n", argv[0]);
        fprintf(stderr, "-v        verbose error messages\n");
        fprintf(stderr, "-init     initialize the tables\n");
        fprintf(stderr, "-tpc      transactions per client\n");
        fprintf(stderr, "-clients  number of simultaneous clients\n");
	exit(1);
    }

    fprintf(stdout, "Scale factor value: %d\n", tps);
    fprintf(stdout, "Number of clients: %d\n", n_clients);
    fprintf(stdout, "Number of transactions per client: %d\n\n",
	    n_txn_per_client);
    fflush(stdout);

    if (init_db) {
        fprintf(stdout, "Initializing dataset...\n");
	createDatabase();
        fprintf(stdout, "done.\n\n");
	fflush(stdout);
    }
#ifndef _WIN32
    shmid = shmget(IPC_PRIVATE, 2 * sizeof (int), IPC_CREAT | 0666);
    shm = shmat(shmid, NULL, 0);
#endif
    transaction_count = &shm[0];
    failed_transactions = &shm[1];
    *transaction_count = 0;
    *failed_transactions = 0;

#ifdef _WIN32
    pids = malloc(n_clients * sizeof (HANDLE));
#else
    pids = malloc(n_clients * sizeof (pid_t));
#endif
    if (pids == NULL) {
        fprintf(stderr, "malloc failed\n");
	exit(2);
    }

    fprintf(stdout, "Starting Benchmark Run\n");

    transactions = 0;
#ifdef _WIN32
    start_time = GetTickCount();
#else
    gettimeofday(&start_time, NULL);
#endif
    if (n_clients < 2) {
        runClientThread(NULL);
    } else {
#ifdef _WIN32
        for (i = 0; i < n_clients; i++) {
	    unsigned tid;

	    pids[i] = (HANDLE) _beginthreadex(NULL, 0,runClientThread,
					      NULL, 0, &tid);
	}
	for (i = 0; i < n_clients; i++) {
	    WaitForSingleObject(pids[i], INFINITE);
	    CloseHandle(pids[i]);
	}
#else
        for (i = 0; i < n_clients; i++) {
	    pid_t child = fork();

	    switch (child) {
	    case 0:
	        return runClientThread(NULL);
	    default:
	        pids[i] = child;
	    }
	}
	for (i = 0; i < n_clients; i++) {
	    int status;

	    waitpid(pids[i], &status, 0);
	}
#endif
    }
    reportDone();

    transactions = 1;
#ifdef _WIN32
    start_time = GetTickCount();
#else
    gettimeofday(&start_time, NULL);
#endif
    if (n_clients < 2) {
        runClientThread(NULL);
    } else {
#ifdef _WIN32
        for (i = 0; i < n_clients; i++) {
	    unsigned tid;

	    pids[i] = (HANDLE) _beginthreadex(NULL, 0,runClientThread,
					      NULL, 0, &tid);
	}
	for (i = 0; i < n_clients; i++) {
	    WaitForSingleObject(pids[i], INFINITE);
	    CloseHandle(pids[i]);
	}
#else
        for (i = 0; i < n_clients; i++) {
	    pid_t child = fork();

	    switch (child) {
	    case 0:
	        return runClientThread(NULL);
	    default:
	        pids[i] = child;
	    }
	}
	for (i = 0; i < n_clients; i++) {
	    int status;

	    waitpid(pids[i], &status, 0);
	}
#endif
    }
    reportDone();

    transactions = -1;
#ifdef _WIN32
    start_time = GetTickCount();
#else
    gettimeofday(&start_time, NULL);
#endif
    if (n_clients < 2) {
        runClientThread(NULL);
    } else {
#ifdef _WIN32
        for (i = 0; i < n_clients; i++) {
	    unsigned tid;

	    pids[i] = (HANDLE) _beginthreadex(NULL, 0,runClientThread,
					      NULL, 0, &tid);
	}
	for (i = 0; i < n_clients; i++) {
	    WaitForSingleObject(pids[i], INFINITE);
	    CloseHandle(pids[i]);
	}
#else
        for (i = 0; i < n_clients; i++) {
	    pid_t child = fork();

	    switch (child) {
	    case 0:
	        return runClientThread(NULL);
	    default:
	        pids[i] = child;
	    }
	}
	for (i = 0; i < n_clients; i++) {
	    int status;

	    waitpid(pids[i], &status, 0);
	}
#endif
    }
    reportDone();

    transactions = -2;
#ifdef _WIN32
    start_time = GetTickCount();
#else
    gettimeofday(&start_time, NULL);
#endif
    if (n_clients < 2) {
        runClientThread(NULL);
    } else {
#ifdef _WIN32
        for (i = 0; i < n_clients; i++) {
	    unsigned tid;

	    pids[i] = (HANDLE) _beginthreadex(NULL, 0,runClientThread,
					      NULL, 0, &tid);
	}
	for (i = 0; i < n_clients; i++) {
	    WaitForSingleObject(pids[i], INFINITE);
	    CloseHandle(pids[i]);
	}
#else
        for (i = 0; i < n_clients; i++) {
	    pid_t child = fork();

	    switch (child) {
	    case 0:
	        return runClientThread(NULL);
	    default:
	        pids[i] = child;
	    }
	}
	for (i = 0; i < n_clients; i++) {
	    int status;

	    waitpid(pids[i], &status, 0);
	}
#endif
    }
    reportDone();

    return 0;
}
