/*************************************************************************\
Copyright (c) 1990-1994 The Regents of the University of California
                        and the University of Chicago.
                        Los Alamos National Laboratory
Copyright (c) 2010-2015 Helmholtz-Zentrum Berlin f. Materialien
                        und Energie GmbH, Germany (HZB)
This file is distributed subject to a Software License Agreement found
in the file LICENSE that is included with this distribution.
\*************************************************************************/
/*************************************************************************\
            Thread creation and control for sequencer state sets
\*************************************************************************/
#include "seq.h"
#include "seq_debug.h"

static void ss_entry(void *arg);

/*
 * sequencer() - Sequencer main thread entry point.
 */
void sequencer (void *arg)	/* ptr to original (global) state program table */
{
	PROG		*sp = (PROG *)arg;
	unsigned	nss;
	size_t		threadLen;
	char		threadName[THREAD_NAME_SIZE+10];

	/* Get this thread's id */
	sp->ss->threadId = epicsThreadGetIdSelf();

	/* Add the program to the program list */
	seqAddProg(sp);

	createOrAttachPvSystem(sp);

	if (!pvSysIsDefined(sp->pvSys))
	{
		sp->die = TRUE;
		goto exit;
	}

	/* Call sequencer init function to initialize variables. */
	sp->initFunc(sp);

	/* Initialize state set variables. In safe mode, copy variable
	   block to state set buffers. Must do all this before connecting. */
	if (optTest(sp, OPT_SAFE))
	{
		for (nss = 0; nss < sp->numSS; nss++)
		{
			SSCB	*ss = sp->ss + nss;
			memcpy(ss->var, sp->var, sp->varSize);
		}
	}

	/* Attach to PV system */
	pvSysAttach(sp->pvSys);

	/* Initiate connect & monitor requests to database channels, waiting
	   for all connections to be established if the option is set. */
	if (seq_connect(sp, optTest(sp, OPT_CONN) != pvStatOK))
		goto exit;

	/* Emulate the 'first monitor event' for anonymous PVs */
	if (optTest(sp, OPT_SAFE))
	{
		unsigned nch;
		for (nch=0; nch<sp->numChans; nch++)
			if (sp->chan[nch].syncedTo && !sp->chan[nch].dbch)
				seq_efSet(sp->ss, sp->chan[nch].syncedTo);
	}

	/* Call program entry function if defined.
	   Treat as if called from 1st state set. */
	if (sp->entryFunc) sp->entryFunc(sp->ss);

	/* Create each additional state set task (additional state set thread
	   names are derived from the first ss) */
	epicsThreadGetName(sp->ss->threadId, threadName, sizeof(threadName));
	threadLen = strlen(threadName);
	for (nss = 1; nss < sp->numSS; nss++)
	{
		SSCB		*ss = sp->ss + nss;
		epicsThreadId	tid;

		/* Form thread name from program name + state set number */
		sprintf(threadName+threadLen, "_%d", nss);

		/* Spawn the task */
		tid = epicsThreadCreate(
			threadName,			/* thread name */
			sp->threadPriority,		/* priority */
			sp->stackSize,			/* stack size */
			ss_entry,			/* entry point */
			ss);				/* parameter */

		DEBUG("Spawning additional state set thread %p: \"%s\"\n", tid, threadName);
	}

	/* First state set jumps directly to entry point */
	ss_entry(sp->ss);

	DEBUG("   Wait for other state sets to exit\n");
	for (nss = 1; nss < sp->numSS; nss++)
	{
		SSCB *ss = sp->ss + nss;
		epicsEventMustWait(ss->dead);
	}

	/* Call program exit function if defined.
	   Treat as if called from 1st state set. */
	if (sp->exitFunc) sp->exitFunc(sp->ss);

exit:
	DEBUG("   Disconnect all channels\n");
	seq_disconnect(sp);
	DEBUG("   Remove program instance from list\n");
	seqDelProg(sp);

	errlogSevPrintf(errlogInfo,
		"Instance %d of sequencer program \"%s\" terminated\n",
		sp->instance, sp->progName);

	/* Free all allocated memory */
	seq_free(sp);
}

/*
 * ss_read_buffer_static() - static version of ss_read_buffer.
 * This is to enable inlining in the for loop in ss_read_all_buffer.
 */
static void ss_read_buffer_static(SSCB *ss, CHAN *ch, boolean dirty_only)
{
	char *val = valPtr(ch,ss);
	char *buf = bufPtr(ch);
	ptrdiff_t nch = chNum(ch);
	/* Must take dbCount for db channels, else we overwrite
	   elements we didn't get */
	size_t count = ch->dbch ? ch->dbch->dbCount : ch->count;
	size_t var_size = ch->type->size * count;

	if (!ss->dirty[nch] && dirty_only)
		return;

	epicsMutexMustLock(ch->varLock);

	DEBUG("ss %s: before read %s", ss->ssName, ch->varName);
	print_channel_value(DEBUG, ch, val);

	memcpy(val, buf, var_size);
	if (ch->dbch)
	{
		/* structure copy */
		ss->metaData[nch] = ch->dbch->metaData;
	}

	DEBUG("ss %s: after read %s", ss->ssName, ch->varName);
	print_channel_value(DEBUG, ch, val);

	ss->dirty[nch] = FALSE;

	epicsMutexUnlock(ch->varLock);
}

/*
 * ss_read_buffer() - Copy value and meta data
 * from shared buffer to state set local buffer
 * and reset corresponding dirty flag. Do this
 * only if dirty flag is set or dirty_only is FALSE.
 */
void ss_read_buffer(SSCB *ss, CHAN *ch, boolean dirty_only)
{
	ss_read_buffer_static(ss, ch, dirty_only);
}

/*
 * ss_read_all_buffer() - Call ss_read_buffer_static
 * for all channels.
 */
static void ss_read_all_buffer(PROG *sp, SSCB *ss)
{
	unsigned nch;

	DEBUG("ss_read_all_buffer\n");
	for (nch = 0; nch < sp->numChans; nch++)
	{
		CHAN *ch = sp->chan + nch;
		/* Call static version so it gets inlined */
		ss_read_buffer_static(ss, ch, TRUE);
	}
}

/*
 * ss_read_all_buffer_selective() - Call ss_read_buffer_static
 * for all channels that are sync'ed to the given event flag.
 * NOTE: calling code must take ev_flag->lock, as we traverse
 * the set of channels synced to this event flag.
 */
void ss_read_buffer_selective(PROG *sp, SSCB *ss, evflag ev_flag)
{
	int word;
	for (word=0; word < NWORDS(sp->numChans); word++)
	{
		if (ev_flag->synced[word])
		{
			int i;
			for (i=0; i<NBITS; i++)
			{
				if (bitTest(ev_flag->synced + word, i))
				{
					ss_read_buffer_static(ss,
						sp->chan + NBITS * word + i, TRUE);
				}
			}
		}
	}
}

/*
 * ss_write_buffer() - Copy given value and meta data
 * to shared buffer. In safe mode, if dirtify is TRUE then
 * set dirty flag for each state set that has a monitor on the channel.
 */
void ss_write_buffer(CHAN *ch, void *val, PVMETA *meta, boolean dirtify)
{
	PROG *sp = ch->prog;
	char *buf = bufPtr(ch);		/* shared buffer */
	/* Must use dbCount for db channels, else we overwrite
	   elements we didn't get */
	size_t count = ch->dbch ? ch->dbch->dbCount : ch->count;
	size_t var_size = ch->type->size * count;
	ptrdiff_t nch = chNum(ch);
	unsigned nss;

	epicsMutexMustLock(ch->varLock);

	DEBUG("ss_write_buffer: before write %s", ch->varName);
	print_channel_value(DEBUG, ch, buf);

	memcpy(buf, val, var_size);
	if (ch->dbch && meta)
		/* structure copy */
		ch->dbch->metaData = *meta;

	DEBUG("ss_write_buffer: after write %s", ch->varName);
	print_channel_value(DEBUG, ch, buf);

	if (optTest(sp, OPT_SAFE) && dirtify)
		for (nss = 0; nss < sp->numSS; nss++)
			if (sp->ss[nss].monitored[nch])
				sp->ss[nss].dirty[nch] = TRUE;

	epicsMutexUnlock(ch->varLock);
}

void dump_mask(pr_fun *pr, seqMask *mask, unsigned num_words)
{
	int n;
	for (n = num_words-1; n >= 0; n--)
	{
		pr("%s"WORD_BIN_FMT, n?"'":"", WORD_BIN(mask[n]));
	}
}

static void ss_transition(SSCB *ss, double now, SEQ_TRANS_FUNC *transFunc, int *nextState)
{
	boolean	ev_trig;
	PROG *sp = ss->prog;

	ss->wakeupTime = epicsINF;

	/* Setting this semaphore here guarantees that conditions
	 * are always evaluated at least once.
	 */
	epicsEventSignal(ss->syncSem);

	/* Loop until an event is triggered */
	do {
		DEBUG("before epicsEventWaitWithTimeout(ss=%ld,timeout=%f)\n",
			ss - sp->ss, ss->wakeupTime - now);
		epicsEventWaitWithTimeout(ss->syncSem, ss->wakeupTime - now);
		DEBUG("after epicsEventWaitWithTimeout(ss=%ld,timeout=%f)\n",
			ss - sp->ss, ss->wakeupTime - now);

		/* Check whether we have been asked to exit */
		if (sp->die) return;

		/* Check whether we have been asked to hold (suspend) */
		if (sp->hold)
		{
			epicsEventSignal(ss->holding);
			epicsEventWait(ss->holdSem);
		}

		/* Copy dirty variable values from CA buffer
		 * to user (safe mode only).
		 */
		if (optTest(sp, OPT_SAFE))
			ss_read_all_buffer(sp, ss);

		ss->wakeupTime = epicsINF;

		DEBUG("ss=%ld: ", ss - sp->ss);
		dump_mask(DEBUG, ss->mask, sp->numEvWords);
		DEBUG("\n");

		/* Check state change conditions and do transition actions */
		ss->eval_when = TRUE;
		ev_trig = transFunc(ss, nextState);

		if (!ev_trig)
			pvTimeGetCurrentDouble(&now);
	} while (!ev_trig);
}

/*
 * ss_entry() - Thread entry point for all state sets.
 * Provides the main loop for state set processing.
 */
static void ss_entry(void *arg)
{
	SSCB		*ss = (SSCB *)arg;
	PROG		*sp = ss->prog;

	/* Attach to PV system; was already done for the first state set */
	if (ss != sp->ss)
	{
		ss->threadId = epicsThreadGetIdSelf();
		createOrAttachPvSystem(sp);
	}

	/* Register this thread with the EPICS watchdog (no callback func) */
	taskwdInsert(ss->threadId, 0, 0);

	/* In safe mode, update local var buffer with global one before
	   entering the event loop. Must do this using
	   ss_read_all_buffer since CA and other state sets could
	   already post events resp. pvPut. */
	if (optTest(sp, OPT_SAFE))
		ss_read_all_buffer(sp, ss);

	/* Initial state is the first one */
	ss->currentState = 0;
	ss->nextState = -1;
	ss->prevState = -1;

	DEBUG("ss %s: entering main loop\n", ss->ssName);

	/*
	 * ============= Main loop ==============
	 */
	while (TRUE)
	{
		STATE	*st = ss->states + ss->currentState;
		double	now;

		assert(ss->currentState >= 0);

		/* If we've changed state, do any entry actions. Also do these
		 * even if it's the same state if option to do so is enabled.
		 */
		if (st->entryFunc && (ss->prevState != ss->currentState
			|| optTest(st, OPT_DOENTRYFROMSELF)))
		{
			st->entryFunc(ss);
		}

		memset(ss->mask, 0, sizeof(seqMask) * sp->numEvWords);

		/* Flush any outstanding DB requests */
		pvSysFlush(sp->pvSys);

		pvTimeGetCurrentDouble(&now);

		/* Set time we entered this state if transition from a different
		 * state or else if option not to do so is off for this state.
		 */
		if ((ss->currentState != ss->prevState) ||
			!optTest(st, OPT_NORESETTIMERS))
		{
			ss->timeEntered = now;
		}

		ss_transition(ss, now, st->transFunc, &ss->nextState);

		/* Check whether we have been asked to exit */
		if (sp->die) break;

		/* If changing state, do exit actions */
		if (st->exitFunc && (ss->currentState != ss->nextState
			|| optTest(st, OPT_DOEXITTOSELF)))
		{
			st->exitFunc(ss);
		}

		/* Change to next state */
		ss->prevState = ss->currentState;
		ss->currentState = ss->nextState;
	}

	/* Thread exit has been requested */
	taskwdRemove(ss->threadId);
	/* Declare ourselves dead */
	if (ss != sp->ss)
		epicsEventSignal(ss->dead);
}

/*
 * Delete all state set threads and do general clean-up.
 */
epicsShareFunc void epicsShareAPI seqStop(epicsThreadId tid)
{
	PROG	*sp;

	/* Check that this is indeed a state program thread */
	sp = seqFindProg(tid);
	if (sp == NULL)
		return;
	seq_exit(sp->ss);
}

/*
 * ss_wakeup() -- wake up each state set that is waiting on this event
 * based on the current event mask; eventNum = 0 means wake all state sets.
 */
void ss_wakeup(PROG *sp, unsigned eventNum)
{
	unsigned nss;

	/* Check event number against mask for all state sets: */
	for (nss = 0; nss < sp->numSS; nss++)
	{
		SSCB *ss = sp->ss + nss;

		/* If event bit in mask is set, wake that state set */
		DEBUG("ss_wakeup: eventNum=%u, state set=%u, mask=", eventNum, nss);
		dump_mask(DEBUG, ss->mask, sp->numEvWords);
		DEBUG("\n");
		if (eventNum == 0 || bitTest(ss->mask, eventNum))
		{
			DEBUG("ss_wakeup: waking up state set=%u\n", nss);
			epicsEventSignal(ss->syncSem); /* wake up ss thread */
		}
	}
}

/* 
 * Immediately terminate all state sets and jump to global exit block.
 */
epicsShareFunc void seq_exit(SS_ID ss)
{
	PROG *sp = ss->prog;
	/* Ask all state set threads to exit */
	sp->die = TRUE;
	/* Take care that we die even if waiting for initial connect */
	epicsEventSignal(sp->ready);
	/* Wakeup all state sets unconditionally */
	ss_wakeup(sp, 0);
}

/*
 * Prepare for call to seq_wait
 */
epicsShareFunc seqWait const seq_wait_init(SS_ID ss, seqMask *event_mask)
{
	seqWait w;
	double now;

	/* save state set run time variables... */
	w.timeEntered = ss->timeEntered;
	w.eventMask = ss->mask;

	/* .. and replace them with new values */
	pvTimeGetCurrentDouble(&now);
	ss->timeEntered = now;
	ss->mask = event_mask;

	ss->wakeupTime = epicsINF;

	/* Evaluate conditions at least once */
	epicsEventSignal(ss->syncSem);

	return w;
}

/*
 * Wait for an event (for wait statement).
 * Returns whether we have been asked to exit.
 */
epicsShareFunc seqBool seq_wait(SS_ID ss)
{
	PROG *sp = ss->prog;
	double now;

	pvTimeGetCurrentDouble(&now);
	epicsEventWaitWithTimeout(ss->syncSem, ss->wakeupTime - now);

	/* Check whether we have been asked to exit */
	if (ss->prog->die) return TRUE;

	/* Check whether we have been asked to hold (suspend) */
	if (sp->hold)
	{
		epicsEventSignal(ss->holding);
		epicsEventWait(ss->holdSem);
	}

	/* Copy dirty variable values from CA buffer
	 * to user (safe mode only).
	 */
	if (optTest(sp, OPT_SAFE))
		ss_read_all_buffer(sp, ss);

	ss->wakeupTime = epicsINF;
	ss->eval_when = TRUE;
	return FALSE;
}

/*
 * Finish wait block
 */
epicsShareFunc void seq_wait_finish(SS_ID ss, seqWait const w)
{
	/* restore state set run time variables */
	ss->timeEntered = w.timeEntered;
	ss->mask = w.eventMask;
}

epicsShareFunc void seq_done_eval_cond(SS_ID ss)
{
	ss->eval_when = FALSE;
}
