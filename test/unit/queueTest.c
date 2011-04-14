/*************************************************************************\
* Copyright (c) 2008 UChicago Argonne LLC, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/
/* Copyright 2010 Helmholtz-Zentrum Berlin f. Materialien und Energie GmbH
   (see file Copyright.HZB included in this distribution)
*/
#include "seq.h"
#include "epicsThread.h"
#include "epicsEvent.h"
#include "epicsUnitTest.h"
#include "testMain.h"

typedef unsigned long long ELEM;

static void check(QUEUE q, size_t expectedFree)
{
    size_t numElems = seqQueueNumElems(q);
    size_t expectedUsed = numElems - expectedFree;
    int expectedEmpty = (expectedUsed == 0);
    int expectedFull = (expectedFree == 0);
    size_t nFree = seqQueueFree(q);
    size_t nUsed = seqQueueUsed(q);
    int isEmpty = seqQueueIsEmpty(q);
    int isFull = seqQueueIsFull(q);

    testOk(nFree == expectedFree, "Free: %d == %d", nFree, expectedFree);
    testOk(nUsed == expectedUsed, "Used: %d == %d", nUsed, expectedUsed);
    testOk(isEmpty == expectedEmpty, "Empty: %d == %d", isEmpty, expectedEmpty);
    testOk(isFull == expectedFull, "Full: %d == %d", isFull, expectedFull);
}

static epicsEventId wdone, rdone, ready;

static const int threadTestIterations = 1000000;
static const size_t threadTestMaxNumElems = 20;

static int readerLost, writerLost;

static void readerTask(void *arg)
{
    QUEUE q = (QUEUE)arg;
    string data;
    boolean empty;
    int i, j;

    for (i = 0; i < threadTestIterations; i++) {
        do {
            empty = seqQueueGet(q, data);
        } while (empty);
        j = atoi(data);
        if (j<i) {
            testAbort("%d<=%d", i, j);
        }
        if (j>threadTestIterations) {
            testAbort("%d<=%d", j, threadTestIterations);
        }
        readerLost+=(j-i);
        i = j;
    }
    epicsEventWait(wdone);
    epicsEventSignal(rdone);
}

static void writerTask(void *arg)
{
    QUEUE q = (QUEUE)arg;
    string data;
    boolean full;
    int i;

    for (i = 0; i < threadTestIterations; i++) {
        sprintf(data, "%d", i);
        full = seqQueuePut(q, data);
        if (full) writerLost++;
    }
    epicsEventSignal(wdone);
}

MAIN(queueTest)
{
    size_t numElems;
    QUEUE q;
    epicsThreadId reader, writer;

    testPlan(212 + 2*threadTestMaxNumElems);

    testOk1(seqQueueCreate(1,0)==0);
    testOk1(seqQueueCreate(0,1)==0);

    for (numElems = 1; numElems <= 3; numElems++) {
        ELEM put[2*numElems];
        ELEM get[2*numElems];
        size_t i;

        testDiag("sequential queueTest with numElems=%u", (unsigned)numElems);

        q = seqQueueCreate(numElems, sizeof(ELEM));
        if (!q) {
            testAbort("seqQueueCreate failed");
        }
        check(q, numElems);

        for (i = 0 ; i < 2*numElems ; i++)
            put[i] = i;
        for(i = 1 ; i <= 2*numElems ; i++) {
            size_t j;
            for (j = 0; j < i; j++) {
                int full = seqQueuePut(q, put+j);
                testOk(full==(j>=numElems), "q put %d", j);
            }
            check(q, numElems > i ? numElems - i : 0);
            for (j = 0; j < i; j++) {
                get[j] = -1ull;
                int empty = seqQueueGet(q, get+j);
                testOk(empty==(j>=numElems), "q get %d", j);
            }
            check(q, numElems);
            for (j = 0; j < i; j++) {
                if (j < numElems-1) {
                    testOk(get[j]==put[j], "get[%d]==put[%d]", j, j);
                } else if (j == numElems-1 && i>=numElems) {
                    testOk(get[j]==put[i-1],
                        "overflow: get[%d]==put[%d]", j, i-1);
                } else {
                    testOk(get[j]==-1ull, "get[%d] unchanged", j);
                }
            }
        }
        seqQueueDestroy(q);
    }

    for (numElems = 1; numElems <= threadTestMaxNumElems; numElems++) {

        testDiag("concurrent queueTest with numElems=%u", (unsigned)numElems);

        q = seqQueueCreate(numElems, sizeof(string));
        wdone = epicsEventCreate(epicsEventEmpty);
        rdone = epicsEventCreate(epicsEventEmpty);
        ready = epicsEventCreate(epicsEventEmpty);
        if (!wdone || !rdone || !ready) {
            testAbort("epicsEventCreate failed");
        }
        readerLost = writerLost = 0;
        reader = epicsThreadCreate("reader", epicsThreadPriorityMedium,
            epicsThreadGetStackSize(epicsThreadStackSmall), readerTask, q);
        writer = epicsThreadCreate("writer", epicsThreadPriorityMedium,
            epicsThreadGetStackSize(epicsThreadStackSmall), writerTask, q);
        if (!reader || !writer) {
            testAbort("epicsThreadCreate failed");
        }
        epicsEventWait(rdone);
        testOk(readerLost == writerLost, "%d==%d", readerLost, writerLost);

        seqQueueDestroy(q);
        testPass("ok");
    }

    epicsEventDestroy(wdone);
    epicsEventDestroy(rdone);
    epicsEventDestroy(ready);

    return testDone();
}
