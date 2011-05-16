/*
	Commandline interpreter interface.
	Copyright (c) 2003-2006 stefan kersten.

	====================================================================

	SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
	http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "SC_TerminalClient.h"

#include <errno.h>
#include <fcntl.h>

#ifdef SC_WIN32
# define __GNU_LIBRARY__
# include "getopt.h"
# include "SC_Win32Utils.h"
# include <io.h>
# include <windows.h>
#else
# include <sys/param.h>
# include <sys/poll.h>
# include <unistd.h>
# ifdef HAVE_READLINE
#   include <readline/readline.h>
#   include <readline/history.h>
#   include <signal.h>
# endif
#endif

#include <string.h>
#include <time.h>

#include "GC.h"
#include "PyrKernel.h"
#include "PyrPrimitive.h"
#include "PyrLexer.h"
#include "PyrSlot.h"
#include "VMGlobals.h"
#include "SC_DirUtils.h"   // for gIdeName

#define STDIN_FD 0

static FILE* gPostDest = stdout;

static const int ticks_per_second = 50; // every 20 milliseconds

SC_TerminalClient::SC_TerminalClient(const char* name)
	: SC_LanguageClient(name),
	  mShouldBeRunning(false),
	  mReturnCode(0),
	  mUseReadline(false),
	  mInput(false),
	  mSched(true)
{
	pthread_cond_init (&mCond, NULL);
	pthread_mutex_init(&mSignalMutex, NULL);
	pthread_mutex_init(&mInputMutex, NULL);
}

SC_TerminalClient::~SC_TerminalClient()
{
	pthread_cond_destroy (&mCond);
	pthread_mutex_destroy(&mSignalMutex);
	pthread_mutex_destroy(&mInputMutex);
}

void SC_TerminalClient::postText(const char* str, size_t len)
{
	fwrite(str, sizeof(char), len, gPostDest);
}

void SC_TerminalClient::postFlush(const char* str, size_t len)
{
	fwrite(str, sizeof(char), len, gPostDest);
	fflush(gPostDest);
}

void SC_TerminalClient::postError(const char* str, size_t len)
{
	fprintf(gPostDest, "ERROR: ");
	fwrite(str, sizeof(char), len, gPostDest);
}

void SC_TerminalClient::flush()
{
	fflush(gPostDest);
}

void SC_TerminalClient::printUsage()
{
	Options opt;

	const size_t bufSize = 128;
	char memGrowBuf[bufSize];
	char memSpaceBuf[bufSize];

	snprintMemArg(memGrowBuf, bufSize, opt.mMemGrow);
	snprintMemArg(memSpaceBuf, bufSize, opt.mMemSpace);

	fprintf(stdout, "Usage:\n   %s [options] [file..] [-]\n\n", getName());
	fprintf(stdout,
			"Options:\n"
			"   -d <path>                      Set runtime directory\n"
			"   -D                             Enter daemon mode (no input)\n"
			"   -g <memory-growth>[km]         Set heap growth (default %s)\n"
			"   -h                             Display this message and exit\n"
			"   -l <path>                      Set library configuration file\n"
			"   -m <memory-space>[km]          Set initial heap size (default %s)\n"
			"   -r                             Call Main.run on startup\n"
			"   -s                             Call Main.stop on shutdown\n"
			"   -u <network-port-number>       Set UDP listening port (default %d)\n"
			"   -i <ide-name>                  Specify IDE name (for enabling IDE-specific class code, default \"%s\")\n",
			memGrowBuf,
			memSpaceBuf,
			opt.mPort,
			gIdeName
		);
}

bool SC_TerminalClient::parseOptions(int& argc, char**& argv, Options& opt)
{
	const char* optstr = ":d:Dg:hl:m:rsu:i:";
	int c;

	// inhibit error reporting
	opterr = 0;

	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
			case 'd':
				opt.mRuntimeDir = optarg;
				break;
			case 'D':
				opt.mDaemon = true;
				break;
			case 'g':
				if (!parseMemArg(optarg, &opt.mMemGrow)) {
					optopt = c;
					goto optArgInvalid;
				}
				break;
			case 'h':
				goto help;
			case 'l':
				opt.mLibraryConfigFile = optarg;
				break;
			case 'm':
				if (!parseMemArg(optarg, &opt.mMemSpace)) {
					optopt = c;
					goto optArgInvalid;
				}
				break;
			case 'r':
				opt.mCallRun = true;
				break;
			case 's':
				opt.mCallStop = true;
				break;
			case 'u':
				if (!parsePortArg(optarg, &opt.mPort)) {
					optopt = c;
					goto optArgInvalid;
				}
				break;
			case '?':
				goto optInvalid;
				break;
			case ':':
				goto optArgExpected;
				break;
			case 'i':
				gIdeName = optarg;
				break;
			default:
				::post("%s: unknown error (getopt)\n", getName());
				quit(255);
				return false;
		}
	}

	argv += optind;
	argc -= optind;

	return true;

 help:
	printUsage();
	quit(0);
	return false;

 optInvalid:
	::post("%s: invalid option -%c\n", getName(), optopt);
	quit(1);
	return false;

 optArgExpected:
	::post("%s: missing argument for option -%c\n", getName(), optopt);
	quit(1);
	return false;

 optArgInvalid:
	::post("%s: invalid argument for option -%c -- %s\n", getName(), optopt, optarg);
	quit(1);
	return false;
}

int SC_TerminalClient::run(int argc, char** argv)
{
	Options& opt = mOptions;

	if (!parseOptions(argc, argv, opt)) {
		return mReturnCode;
	}

	// finish argv processing
	const char* codeFile = 0;

	if (argc > 0) {
		codeFile = argv[0];
		opt.mDaemon = true;
		argv++; argc--;
	}

	opt.mArgc = argc;
	opt.mArgv = argv;

	// read library configuration file
	bool success;
	if (opt.mLibraryConfigFile) {
		readLibraryConfig(opt.mLibraryConfigFile, opt.mLibraryConfigFile);
	} else {
		readDefaultLibraryConfig();
	}

	// initialize runtime
	initRuntime(opt);

	// startup library
	mShouldBeRunning = true;
	compileLibrary();

	// enter main loop
	if (codeFile) executeFile(codeFile);
	if (opt.mCallRun) runMain();

	if (opt.mDaemon) {
		daemonLoop();
	}
	else {
		initCmdLine();
		if( shouldBeRunning() ) startCmdLine();
		if( shouldBeRunning() ) commandLoop();
		endCmdLine();
		cleanupCmdLine();
	}

	if (opt.mCallStop) stopMain();

	// shutdown library
	shutdownLibrary();
	flush();

	shutdownRuntime();

	return mReturnCode;
}

void SC_TerminalClient::quit(int code)
{
	mReturnCode = code;
	mShouldBeRunning = false;
}

void SC_TerminalClient::interpretCmdLine(PyrSymbol* method, SC_StringBuffer& cmdLine)
{
	setCmdLine(cmdLine);
	cmdLine.reset();
	runLibrary(method);
	flush();
}

void SC_TerminalClient::interpretCmdLine(PyrSymbol* method, const char* cmdLine)
{
	setCmdLine(cmdLine);
	runLibrary(method);
	flush();
}


void SC_TerminalClient::interpretLocked(PyrSymbol* method, const char *buf, size_t size)
{
	// NOTE: re-implementing interpretCmdLine() to do the same without locking lang
	if (isLibraryCompiled()) {
		VMGlobals *g = gMainVMGlobals;

		PyrString* strobj = newPyrStringN(g->gc, size, 0, true);
		memcpy(strobj->s, buf, size);

		SetObject(&slotRawInterpreter(&g->process->interpreter)->cmdLine, strobj);
		g->gc->GCWrite(slotRawObject(&g->process->interpreter), strobj);

		::runLibrary(method);
		flush();
	}
}

// WARNING: Call with input locked!
void SC_TerminalClient::interpretInput()
{
	char *data = mCmdLine.getData();
	int c = mCmdLine.getSize();
	int i = 0;
	while( i < c ) {
		if( data[i] == kInterpretCmdLine ) {
			lock();
			interpretLocked(s_interpretCmdLine, data, i);
			unlock();
			data += i+1;
			c -= i+1;
			i = 0;
		}
		else if( data[i] == kInterpretPrintCmdLine ) {
			lock();
			interpretLocked(s_interpretPrintCmdLine, data, i);
			unlock();
			data += i+1;
			c -= i+1;
			i = 0;
		}
		else ++i;
	}
	mCmdLine.reset();
}

void SC_TerminalClient::onLibraryStartup()
{
	SC_LanguageClient::onLibraryStartup();
	int base, index = 0;
	base = nextPrimitiveIndex();
	definePrimitive(base, index++, "_Argv", &SC_TerminalClient::prArgv, 1, 0);
	definePrimitive(base, index++, "_Exit", &SC_TerminalClient::prExit, 1, 0);
	definePrimitive(base, index++, "_AppClock_SchedNotify",
		&SC_TerminalClient::prScheduleChanged, 1, 0);
}

void SC_TerminalClient::onScheduleChanged()
{
	lockSignal();
	mSched = true;
	pthread_cond_signal( &mCond );
	unlockSignal();
}

void SC_TerminalClient::onInput()
{
	lockSignal();
	mInput = true;
	pthread_cond_signal( &mCond );
	unlockSignal();
}

void SC_TerminalClient::onQuit( int exitCode )
{
	lockSignal();
	postfl("client: quit request %i\n", exitCode);
	quit( exitCode );
	pthread_cond_signal( &mCond );
	unlockSignal();
}

extern void ElapsedTimeToTimespec(double elapsed, struct timespec *spec);

void SC_TerminalClient::commandLoop()
{
	bool haveNext = false;
	struct timespec nextAbsTime;

	lockSignal();

	while( shouldBeRunning() )
	{

		while ( mInput || mSched ) {
			bool input = mInput;
			bool sched = mSched;

			unlockSignal();

			if (input) {
				//postfl("input\n");
				lockInput();
				interpretInput();
				// clear input signal, as we've processed anything signalled so far.
				lockSignal();
				mInput = false;
				unlockSignal();
				unlockInput();
			}

			if (sched) {
				//postfl("tick\n");
				double secs;
				lock();
				haveNext = tickLocked( &secs );
				// clear scheduler signal, as we've processed all items scheduled up to this time.
				// and will enter the wait according to schedule.
				lockSignal();
				mSched = false;
				unlockSignal();
				unlock();
				//postfl("tick -> next time = %f\n", haveNext ? secs : -1);
				ElapsedTimeToTimespec( secs, &nextAbsTime );
			}

			lockSignal();
		}

		if( !shouldBeRunning() ) {
			break;
		}
		else if( haveNext ) {
			int result = pthread_cond_timedwait( &mCond, &mSignalMutex, &nextAbsTime );
			if( result == ETIMEDOUT ) mSched = true;
		}
		else {
			pthread_cond_wait( &mCond, &mSignalMutex );
		}
	}

	unlockSignal();
}

void SC_TerminalClient::daemonLoop()
{
	commandLoop();
}

#ifdef HAVE_READLINE

void sc_rl_signalhandler(int sig);
void sc_rl_signalhandler(int sig){
	// ensure ctrl-C clears line rather than quitting (ctrl-D will quit nicely)
	rl_replace_line("", 0);
	rl_reset_line_state();
	rl_crlf();
	rl_redisplay();
}

int sc_rl_mainstop(int i1, int i2);
int sc_rl_mainstop(int i1, int i2){
	SC_TerminalClient::instance()->stopMain();
	// We also push a newline so that there's some UI feedback
	rl_reset_line_state();
	rl_crlf();
	rl_redisplay();
	return 0;
}

void SC_TerminalClient::readlineCb( char *cmdLine )
{
	SC_TerminalClient *client = static_cast<SC_TerminalClient*>(instance());

	if( cmdLine == NULL ) {
		printf("\nExiting sclang (ctrl-D)\n");
		client->onQuit(0);
		return;
	}

	if(*cmdLine!=0){
		// If line wasn't empty, store it so that uparrow retrieves it
		add_history(cmdLine);
		int len = strlen(cmdLine);

		client->lockInput();
		client->mCmdLine.append(cmdLine, len);
		client->mCmdLine.append(kInterpretPrintCmdLine);
		client->onInput();
		client->unlockInput();
	}
}

void *SC_TerminalClient::readlineFunc( void *arg )
{
	SC_TerminalClient *client = static_cast<SC_TerminalClient*>(arg);

	// Setup readline
	rl_readline_name = "sclang";
	rl_basic_word_break_characters = " \t\n\"\\'`@><=;|&{}().";
	//rl_attempted_completion_function = sc_rl_completion;
	rl_bind_key(0x02, &sc_rl_mainstop);
	// TODO 0x02 is ctrl-B;
	// ctrl-. would be nicer but keycode not working here (plain "." is 46 (0x2e))
	rl_callback_handler_install( "sc3> ", &readlineCb );

	// Set our handler for SIGINT that will clear the line instead of terminating.
	// NOTE: We prevent readline from setting its own signal handlers,
	// to not override ours.
	rl_catch_signals = 0;
	struct sigaction sact;
	memset( &sact, 0, sizeof(struct sigaction) );
	sact.sa_handler = &sc_rl_signalhandler;
	sigaction( SIGINT, &sact, 0 );

	fd_set fds;
	FD_ZERO(&fds);

	while(true) {
		FD_SET(STDIN_FD, &fds);
		FD_SET(client->mCmdPipe[0], &fds);

		if( select(FD_SETSIZE, &fds, NULL, NULL, NULL) < 0 ) {
			postfl("readline: select() error:\n%s\n", strerror(errno));
			client->onQuit(1);
			break;
		}

		if( FD_ISSET(client->mCmdPipe[0], &fds) ) {
			postfl("readline: quit requested\n");
			break;
		}

		if( FD_ISSET(STDIN_FD, &fds) ) {
			rl_callback_read_char();
		}
	}

	printf("readline stopped.\n");
}
/*
// Completion from sclang dictionary TODO
char ** sc_rl_completion (const char *text, int start, int end);
char ** sc_rl_completion (const char *text, int start, int end){
	char **matches = (char **)NULL;
	printf("sc_rl_completion(%s, %i, %i)\n", text, start, end);
	return matches;
}
*/
#endif

#ifndef _WIN32

void *SC_TerminalClient::pipeFunc( void *arg )
{
	SC_TerminalClient *client = static_cast<SC_TerminalClient*>(arg);

	ssize_t bytes;
	const size_t toRead = 256;
	char buf[toRead];
	SC_StringBuffer stack;

	fd_set fds;
	FD_ZERO(&fds);

	bool shouldRead = true;
	while(shouldRead) {
		FD_SET(STDIN_FD, &fds);
		FD_SET(client->mCmdPipe[0], &fds);

		if( select(FD_SETSIZE, &fds, NULL, NULL, NULL) < 0 ) {
			postfl("pipe-in: select() error:\n%s\n", strerror(errno));
			client->onQuit(1);
			break;
		}

		if( FD_ISSET(client->mCmdPipe[0], &fds) ) {
			postfl("pipe-in: quit requested\n");
			break;
		}

		if( FD_ISSET(STDIN_FD, &fds) ) {

			while(true) {
				bytes = read( STDIN_FD, buf, toRead );

				if( bytes > 0 ) {
					client->pushCmdLine( stack, buf, bytes );
				}
				else if( bytes == 0 ) {
					postfl("pipe-in: EOF. Will stop reading.\n");
					shouldRead = false;
					break;
				}
				else {
					if( errno == EAGAIN ) {
						break; // no more to read this time;
					}
					else if( errno != EINTR ){
						postfl("pipe-in: read() error:\n%s\n", strerror(errno));
						client->onQuit(1);
						shouldRead = false;
						break;
					}
				}
			}
		}
	}

	postfl("pipe-in: stopped.\n");
}

#else

void *SC_TerminalClient::pipeFunc( void *arg )
{
	SC_TerminalClient *client = static_cast<SC_TerminalClient*>(arg);

	SC_StringBuffer stack;
	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE hnds[] = { client->mQuitInputEvent, hStdIn };

	bool shouldRun = true;
	while (shouldRun) {
		DWORD result = WaitForMultipleObjects( 2, hnds, false, INFINITE );

		if( result == WAIT_FAILED ) {
			postfl("pipe-in: wait error.\n");
			client->onQuit(1);
			break;
		}

		int hIndex = result - WAIT_OBJECT_0;

		if( hIndex == 0 ) {
			postfl("pipe-in: quit requested.\n");
			break;
		}

		if( hIndex == 1 ) {
			DWORD nAvail;
			if (!PeekNamedPipe(hStdIn, NULL, 0, NULL, &nAvail, NULL)) {
				postfl("pipe-in: error trying to peek stdin.\n");
				client->onQuit(1);
				break;
			}

			while (nAvail > 0)
			{
				char buf[256];
				DWORD nRead = sc_min(256, nAvail);
				if (!ReadFile(hStdIn, buf, nRead, &nRead, NULL)) {
					postfl("pipe-in: error trying to peek stdin.\n");
					client->onQuit(1);
					shouldRun = false;
					break;
				}
				else if (nRead > 0) {
					client->pushCmdLine( stack, buf, nRead );
				}

				nAvail -= nRead;
			}
		}
	}
}

#endif

void SC_TerminalClient::pushCmdLine( SC_StringBuffer &buf, const char *newData, size_t size)
{
	lockInput();

	bool signal = false;

	while (size--) {
		char c = *newData++;
		if (c == kInterpretCmdLine || c == kInterpretPrintCmdLine) {
			mCmdLine.append( buf.getData(), buf.getSize() );
			mCmdLine.append(c);
			signal = true;
			buf.reset();
		} else {
			buf.append(c);
		}
	}

	if(signal) onInput();

	unlockInput();
}




void SC_TerminalClient::initCmdLine()
{

#ifndef _WIN32

	if( pipe( mCmdPipe ) == -1 ) {
		postfl("Error creating pipe for input thread control:\n%s\n", strerror(errno));
		quit(1);
	}

#ifdef HAVE_READLINE

	if (strcmp(gIdeName, "none") == 0) {
		// Other clients (emacs, vim, ...) won't want to interact through rl
		mUseReadline = true;
		return;
	}

#endif

	if( fcntl( STDIN_FD, F_SETFL, O_NONBLOCK ) == -1 ) {
		postfl("Error setting up non-blocking pipe reading:\n%s\n", strerror(errno));
		quit(1);
	}

#else // !_WIN32

	mQuitInputEvent = CreateEvent( NULL, false, false, NULL );
	if( mQuitInputEvent == NULL ) {
		postfl("Error creating event for input thread control.\n");
		quit(1);
	}
	postfl("Created input thread control event.\n");

#endif // !_WIN32
}


void SC_TerminalClient::startCmdLine()
{
#ifdef HAVE_READLINE
	if( mUseReadline )
		pthread_create( &mInputThread, NULL, &SC_TerminalClient::readlineFunc, this );
	else
#endif
		pthread_create( &mInputThread, NULL, &SC_TerminalClient::pipeFunc, this );
}

void SC_TerminalClient::endCmdLine()
{
#ifndef _WIN32
	postfl("client: closing input-thread control pipe\n");
	close( mCmdPipe[1] );
#else
	postfl("client: signalling input thread quit event\n");
	SetEvent( mQuitInputEvent );
#endif

	postfl("client: stopped, waiting for input thread to join...\n");

	pthread_join( mInputThread, NULL );

	postfl("client: input thread joined.\n");
}

void SC_TerminalClient::cleanupCmdLine()
{
#ifdef HAVE_READLINE
	if( mUseReadline ) rl_callback_handler_remove();
#endif
}

int SC_TerminalClient::prArgv(struct VMGlobals* g, int)
{
	int argc = ((SC_TerminalClient*)SC_TerminalClient::instance())->options().mArgc;
	char** argv = ((SC_TerminalClient*)SC_TerminalClient::instance())->options().mArgv;

	PyrSlot* argvSlot = g->sp;

	PyrObject* argvObj = newPyrArray(g->gc, argc * sizeof(PyrObject), 0, true);
	SetObject(argvSlot, argvObj);

	for (int i=0; i < argc; i++) {
		PyrString* str = newPyrString(g->gc, argv[i], 0, true);
		SetObject(argvObj->slots+i, str);
		argvObj->size++;
		g->gc->GCWrite(argvObj, (PyrObject*)str);
	}

	return errNone;
}

int SC_TerminalClient::prExit(struct VMGlobals* g, int)
{
	int code;

	int err = slotIntVal(g->sp, &code);
	if (err) return err;

	((SC_TerminalClient*)SC_LanguageClient::instance())->onQuit( code );

	return errNone;
}

int SC_TerminalClient::prScheduleChanged( struct VMGlobals *g, int numArgsPushed)
{
	static_cast<SC_TerminalClient*>(instance())->onScheduleChanged();
}

// EOF
