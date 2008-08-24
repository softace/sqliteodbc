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
#include <sqlite3.h>

/* tpc bm b scaling rules */
static int tps = 1;		/* the tps scaling factor: here it is 1 */
static int nbranches = 1;	/* number of branches in 1 tps db */
static int ntellers = 10;	/* number of tellers in  1 tps db */
static int naccounts = 100000;	/* number of accounts in 1 tps db */

static char *dbname = NULL;
static char *combegtrans = "COMMIT TRANSACTION ; BEGIN TRANSACTION";
static char *begtrans = "BEGIN TRANSACTION";

#define TELLER 0
#define BRANCH 1
#define ACCOUNT 2

static int *failed_transactions = NULL;
static int *transaction_count = NULL;
static int *stat_counts = NULL;
static int n_clients = 10;
static int n_txn_per_client = 10;

#ifdef _WIN32
static int start_time;
#else
static struct timeval start_time;
#endif
static int transactions = 1;
static int verbose = 0;
static int useexcl = 0;

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
    if (transactions) {
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

#ifdef TRACE_DBINIT
static void dbtrace(void *arg, const char *msg)
{
    if (msg) {
        fprintf(stderr, "%s\n", msg);
    }
}
#endif

static void createDatabase()
{
    sqlite3 *sqlite;
    int dotrans = 0;
    int accountsnb = 0;
    int i, nrows, ncols;
    char **rows;

    if (sqlite3_open(dbname, &sqlite) != SQLITE_OK) {
        fprintf(stderr, "unable to connect to %s\n", dbname);
	exit(1);
    }
#ifdef TRACE_DBINIT
    sqlite3_trace(sqlite, dbtrace, NULL);
#endif
    if (sqlite3_exec(sqlite, begtrans, NULL, NULL, NULL)
	== SQLITE_OK) {
        dotrans = 1;
    }
    if (sqlite3_get_table(sqlite, "SELECT count(*) FROM accounts",
			  &rows, &ncols, &nrows, NULL)
	== SQLITE_OK) {
        if (rows && ncols == 1 && nrows == 1 && rows[1]) {
	    accountsnb = strtol(rows[1], NULL, 0);
	}
	if (rows) {
	    sqlite3_free_table(rows);
	}
	if (dotrans) {
	    sqlite3_exec(sqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	}
	if (accountsnb == naccounts * tps) {
            fprintf(stdout, "Already initialized\n");
	    fflush(stdout);
	    sqlite3_close(sqlite);
	    return;
        }
    }
    fprintf(stdout, "Drop old tables if they exist\n");
    fflush(stdout);
    if (sqlite3_exec(sqlite, "DROP TABLE history; "
		     "DROP TABLE accounts; "
		     "DROP TABLE tellers; "
		     "DROP TABLE branches; ", NULL, NULL, NULL)
	== SQLITE_OK) {
	if (dotrans) {
	    sqlite3_exec(sqlite, combegtrans, NULL, NULL, NULL);
	}
    }
    fprintf(stdout, "Create tables\n");
    fflush(stdout);
    sqlite3_exec(sqlite, "CREATE TABLE branches ("
		 "Bid INTEGER NOT NULL PRIMARY KEY, "
		 "Bbalance INTEGER, "
		 "filler CHAR(88))", NULL, NULL, NULL);
    sqlite3_exec(sqlite, "CREATE TABLE tellers ("
		 "Tid INTEGER NOT NULL PRIMARY KEY, "
		 "Bid INTEGER, "
		 "Tbalance INTEGER, "
		 "filler CHAR(84))", NULL, NULL, NULL);
    sqlite3_exec(sqlite, "CREATE TABLE accounts ("
		 "Aid INTEGER NOT NULL PRIMARY KEY, "
		 "Bid INTEGER, "
		 "Abalance INTEGER, "
		 "filler CHAR(84))", NULL, NULL, NULL);
    sqlite3_exec(sqlite, "CREATE TABLE history ("
		 "Tid INTEGER, "
		 "Bid INTEGER, "
		 "Aid INTEGER, "
		 "delta INTEGER, "
		 "tstime TIMESTAMP, "
		 "filler CHAR(22))", NULL, NULL, NULL);
    fprintf(stdout, "Delete elements in table in case DROP didn't work\n");
    fflush(stdout);
    sqlite3_exec(sqlite, "DELETE FROM history; "
		 "DELETE FROM accounts; "
		 "DELETE FROM tellers; "
		 "DELETE FROM branches ", NULL, NULL, NULL);
    if (dotrans) {
        sqlite3_exec(sqlite, combegtrans, NULL, NULL, NULL);
    }
    fprintf(stdout, "Insert data in branches table\n");
    fflush(stdout);
    for (i = 0; i < nbranches * tps; i++) {
        char *sql = sqlite3_mprintf("INSERT INTO branches(Bid,Bbalance) "
				    "VALUES (%d,0)", i);

        sqlite3_exec(sqlite, sql, NULL, NULL, NULL);
	sqlite3_free(sql);
	if (i % 100 == 0 && dotrans) {
	    sqlite3_exec(sqlite, combegtrans, NULL, NULL, NULL);
	}
    }
    if (dotrans) {
        sqlite3_exec(sqlite, combegtrans, NULL, NULL, NULL);
    }
    fprintf(stdout, "Insert data in tellers table\n");
    fflush(stdout);
    for (i = 0; i < ntellers * tps; i++) {
        char *sql = sqlite3_mprintf("INSERT INTO tellers(Tid,Bid,Tbalance) "
				    "VALUES (%d,%d,0)",
				    i, i / ntellers);

        sqlite3_exec(sqlite, sql, NULL, NULL, NULL);
	sqlite3_free(sql);
	if (i % 100 == 0 && dotrans) {
	    sqlite3_exec(sqlite, combegtrans, NULL, NULL, NULL);
	}
    }
    if (dotrans) {
        sqlite3_exec(sqlite, combegtrans, NULL, NULL, NULL);
    }
    fprintf(stdout, "Insert data in accounts table\n");
    fflush(stdout);
    for (i = 0; i < naccounts * tps; i++) {
        char *sql = sqlite3_mprintf("INSERT INTO accounts(Aid,Bid,Abalance) "
				    "VALUES (%d,%d,0)",
				    i, i / naccounts);

        sqlite3_exec(sqlite, sql, NULL, NULL, NULL);
	sqlite3_free(sql);
	if (i % 10000 == 0 && dotrans) {
	    sqlite3_exec(sqlite, combegtrans, NULL, NULL, NULL);
	}
	if (i > 0 && i % 10000 == 0) {
	    fprintf(stdout,"\t%d\trecords inserted\n", i);
	    fflush(stdout);
	}
    }
    if (dotrans) {
        sqlite3_exec(sqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
    }
    fprintf(stdout, "\t%d\trecords inserted\n", naccounts * tps);
    fflush(stdout);
    sqlite3_close(sqlite);
}

static void doOne(sqlite3 *sqlite, int bid, int tid, int aid, int delta)
{
    int aBalance = 0;
    int nrows, ncols, rc, retries = 500, intrans;
    char **rows, *sql = 0;

    if (sqlite == NULL) {
        incrementFailedTransactionCount();
	return;
    }
again:
    intrans = 0;
    if (transactions) {
        rc = sqlite3_exec(sqlite, begtrans, NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
	    stat_counts[0] += 1;
	    goto transfail;
	}
	intrans = 1;
    }
    sql = sqlite3_mprintf("UPDATE accounts "
			  "SET Abalance = Abalance + %d WHERE Aid = %d",
			  delta, aid);
    rc = sqlite3_exec(sqlite, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        stat_counts[1] += 1;
        goto transfail;
    }
    sqlite3_free(sql);
    sql = sqlite3_mprintf("SELECT Abalance "
			  "FROM accounts WHERE Aid = %d", aid);
    rc = sqlite3_get_table(sqlite, sql, &rows, &nrows, &ncols, NULL);
    if (rc != SQLITE_OK) {
        stat_counts[2] += 1;
        goto transfail;
    }
    sqlite3_free(sql);
    if (nrows == 1 && ncols == 1 && rows && rows[1]) {
        aBalance = strtol(rows[1], NULL, 0);
    }
    if (rows) {
        sqlite3_free_table(rows);
    }
    sql = sqlite3_mprintf("UPDATE tellers "
			  "SET Tbalance = Tbalance + %d WHERE Tid = %d",
			  delta, tid);
    rc = sqlite3_exec(sqlite, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        stat_counts[3] += 1;
        goto transfail;
    }
    sqlite3_free(sql);
    sql = sqlite3_mprintf("UPDATE branches "
			  "SET Bbalance = Bbalance + %d WHERE Bid = %d",
			  delta, bid);
    rc = sqlite3_exec(sqlite, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        stat_counts[4] += 1;
        goto transfail;
    }
    sqlite3_free(sql);
    sql = sqlite3_mprintf("INSERT INTO history"
			  "(Tid, Bid, Aid, delta) VALUES"
			  "(%d, %d, %d, %d)",
			  tid, bid, aid, delta);
    rc = sqlite3_exec(sqlite, sql,  NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        stat_counts[5] += 1;
        goto transfail;
    }
    sqlite3_free(sql);
    sql = 0;
    if (transactions) {
try_commit:
        rc = sqlite3_exec(sqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	if (rc == SQLITE_BUSY && --retries > 0) {
	    stat_counts[9] += 1;
#ifdef _WIN32
	    Sleep(10);
#else
	    usleep(10000);
#endif
	    goto try_commit;
	}
	if (rc != SQLITE_OK) {
	    stat_counts[6] += 1;
	    goto transfail;
	}
    }
    return;
transfail:
    if (sql) {
        sqlite3_free(sql);
	sql = 0;
    }
    if (rc == SQLITE_BUSY && --retries > 0) {
        if (intrans) {
	    sqlite3_exec(sqlite, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
	    stat_counts[7] += 1;
	}
	stat_counts[8] += 1;
#ifdef _WIN32
	Sleep(10);
#else
	usleep(10000);
#endif
        goto again;
    }
    incrementFailedTransactionCount();
    if (intrans) {
	stat_counts[10] += 1;
	sqlite3_exec(sqlite, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
    }
}

#ifdef _WIN32
static unsigned __stdcall runClientThread(void *args)
#else
static int runClientThread(void *args)
#endif
{
    sqlite3 *sqlite;
    int ntrans = n_txn_per_client;

    if (sqlite3_open(dbname, &sqlite) != SQLITE_OK) {
        return 0;
    }
    sqlite3_busy_timeout(sqlite, 100000);
    while (ntrans-- > 0) {
        int account = getRandomID(ACCOUNT);
	int branch  = getRandomID(BRANCH);
	int teller  = getRandomID(TELLER);
	int delta   = getRandomInt(0,1000);
	doOne(sqlite, branch, teller, account, delta);
	incrementTransactionCount();
    }
    sqlite3_close(sqlite);
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
        } else if (strcmp(argv[i], "-dbname") == 0) {
	    if (i + 1 < argc) {
	        i++;
		dbname = argv[i];
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
	} else if (strcmp(argv[i], "-excl") == 0) {
	    useexcl++;
	}
    }
    if (dbname == NULL) {
        fprintf(stderr, "usage: %s -dbame DBFILE [-v] [-init] "
		"[-tpc n] [-clients c] [-excl\n\n", argv[0]);
        fprintf(stderr, "-v        verbose error messages\n");
        fprintf(stderr, "-init     initialize the tables\n");
        fprintf(stderr, "-tpc      transactions per client\n");
        fprintf(stderr, "-clients  number of simultaneous clients\n");
	fprintf(stderr, "-excl     use EXCLUSIVE transactions\n");
	exit(1);
    }

    fprintf(stdout, "Scale factor value: %d\n", tps);
    fprintf(stdout, "Number of clients: %d\n", n_clients);
    fprintf(stdout, "Number of transactions per client: %d\n\n",
	    n_txn_per_client);
    fflush(stdout);

    if (useexcl) {
	combegtrans = "COMMIT TRANSACTION ; BEGIN EXCLUSIVE TRANSACTION";
	begtrans = "BEGIN EXCLUSIVE TRANSACTION";
    }

    if (init_db) {
        fprintf(stdout, "Initializing dataset...\n");
	createDatabase();
        fprintf(stdout, "done.\n\n");
	fflush(stdout);
    }

#ifndef _WIN32
    shmid = shmget(IPC_PRIVATE, 200 * sizeof (int), IPC_CREAT | 0666);
    shm = shmat(shmid, NULL, 0);
#endif
    transaction_count = &shm[0];
    failed_transactions = &shm[1];
    stat_counts = &shm[2];
    *transaction_count = 0;
    *failed_transactions = 0;
    memset(stat_counts, 0, 198 * sizeof (int));

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

	    pids[i] = (HANDLE) _beginthreadex(NULL, 0, runClientThread,
					      NULL, 0, &tid);
	}
	for (i = 0; i < n_clients; i++) {
	    WaitForSingleObject(pids[i], INFINITE);
	    CloseHandle(pids[i]);
	}
#else
        for (i = 0; i < n_clients; i++) {
	    pid_t child = fork();
	    int rc;

	    switch (child) {
	    case 0:
	        rc = runClientThread(NULL);
		exit(rc);
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
	    int rc;

	    switch (child) {
	    case 0:
	        rc = runClientThread(NULL);
		exit(rc);
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

    fprintf(stdout, "--------------------\n");
    fprintf(stdout, "Error counters, consult source for stat_counts[].\n");
    for (i = 0; i < 16; i++) {
	if (i == 0) {
	    fprintf(stdout, "stat_counts[0..7]: ");
	}
        fprintf(stdout, " %d", stat_counts[i]);
	if (i == 7) {
	    fprintf(stdout, "\nstat_counts[8..15]:");
	}
    }
    fprintf(stdout, "\n\n");
    return 0;
}
