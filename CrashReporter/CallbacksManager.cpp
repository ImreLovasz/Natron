/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * Copyright (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 * Copyright (C) 2018-2020 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#include "CallbacksManager.h"

#include "Global/Macros.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <exception>
#include <stdexcept>

#include <QtCore/QDebug>

#ifndef REPORTER_CLI_ONLY
#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QDesktopServices>
#include <QUrl>
#endif

#include <QCoreApplication>
#include <QLocalSocket>
#include <QLocalServer>
#include <QThread>
#include <QFile>
#include <QDir>
#include <QTemporaryFile>
#include <QFileInfo>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QString>
#include <QVarLengthArray>
#include <QSettings>

#ifdef DEBUG
#include <QTextStream>
#endif

#include <Breakdown/breakdown.h>

#if defined(Q_OS_MAC)
#include "client/mac/crash_generation/crash_generation_server.h"
#include <execinfo.h>
#include <cstdio>
#include <sys/errno.h>
#include <sys/signal.h>
#include <sys/wait.h>
#include <spawn.h>

#elif defined(Q_OS_LINUX)
#include "client/linux/crash_generation/crash_generation_server.h"
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <ucontext.h>
#include <execinfo.h>
#include <cstdio>
#include <errno.h>
#include <sys/wait.h>

#elif defined(Q_OS_WIN32)
#include <windows.h>
#include "client/windows/crash_generation/crash_generation_server.h"
#endif

#ifdef NATRON_CRASH_REPORTER_USE_FORK
#include <sys/types.h>
#include <unistd.h> // getpid

extern char** environ;
#endif

#ifndef REPORTER_CLI_ONLY
#include "CrashDialog.h"
#endif

#include "Global/ProcInfo.h"
#include "Global/GitVersion.h"
#include "Global/StrUtils.h"

CallbacksManager* CallbacksManager::_instance = 0;

#define EXIT_APP(code, exitIfDumpReceived) ( CallbacksManager::instance()->s_emitDoExitCallBackOnMainThread(code, exitIfDumpReceived) )

namespace {
#ifdef NATRON_CRASH_REPORTER_USE_FORK


// strndup doesn't exist on OS X prior to 10.7
static char *
strndup_replacement(const char *str,
                    size_t n)
{
    size_t len;
    char *copy;

    for (len = 0; len < n && str[len]; ++len) {
        continue;
    }

    if ( ( copy = (char *)malloc(len + 1) ) == NULL ) {
        return (NULL);
    }
    memcpy(copy, str, len);
    copy[len] = '\0';

    return (copy);
}

static char*
qstringToMallocCharArray(const QString& str)
{
    std::string stdStr = str.toStdString();

    return strndup_replacement(stdStr.c_str(), stdStr.size() + 1);
}

static void
handleChildDeadSignal( int /*signalId*/ )
{
    /*
         In Practise this handler is only called on OS X when the Natron application exits.
         On Linux this may be called when ptrace dumper suspends the thread of Natron, hence we handle it
         separately
     */
#ifdef Q_OS_LINUX
    int status;
    pid_t pid = waitpid(WAIT_ANY, &status, WUNTRACED | WNOHANG);
    if ( ( pid <= 0) || ( errno == ECHILD) ) {
        //The child process is not really finished, this was probably called from breakpad, ignore it
        return;
    }
#endif
    if (qApp) {
        EXIT_APP(0, false);
    } else {
        std::exit(1);
    }
}

static void
setChildDeadSignal()
{
    struct sigaction sa;

    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handleChildDeadSignal;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        std::perror("setting up termination signal");
        std::exit(1);
    }
}

#if defined(Q_OS_MAC)
/* from http://svn.python.org/projects/python/trunk/Mac/Tools/pythonw.c */
static bool
setup_spawnattr(posix_spawnattr_t* spawnattr)
{
    size_t ocount;
    size_t count;
    cpu_type_t cpu_types[1];
    short flags = 0;
#ifdef __LP64__
    //int   ch;
#endif

    if ((errno = posix_spawnattr_init(spawnattr)) != 0) {
        perror("posix_spawnattr_int");
        return false;
        //err(2, "posix_spawnattr_int");
        /* NOTREACHTED */
    }

    count = 1;

    /* Run the real Natron executable using the same architure as this
     * executable, this allows users to controle the architecture using
     * "arch -ppc Natron"
     */

#if defined(__ppc64__)
    cpu_types[0] = CPU_TYPE_POWERPC64;

#elif defined(__x86_64__)
    cpu_types[0] = CPU_TYPE_X86_64;

#elif defined(__ppc__)
    cpu_types[0] = CPU_TYPE_POWERPC;
#elif defined(__i386__)
    cpu_types[0] = CPU_TYPE_X86;
#else
#       error "Unknown CPU"
#endif

    if (posix_spawnattr_setbinpref_np(spawnattr, count,
                                      cpu_types, &ocount) == -1) {
        perror("posix_spawnattr_setbinpref");
        return false;
        //err(1, "posix_spawnattr_setbinpref");
        /* NOTREACHTED */
    }
    if (count != ocount) {
        fprintf(stderr, "posix_spawnattr_setbinpref failed to copy\n");
        return false;
        //exit(1);
        /* NOTREACHTED */
    }


    /*
     * Set flag that causes posix_spawn to behave like execv
     */
    flags |= POSIX_SPAWN_SETEXEC;
    if ((errno = posix_spawnattr_setflags(spawnattr, flags)) != 0) {
        perror("posix_spawnattr_setflags");
        return false;
        //err(1, "posix_spawnattr_setflags");
        /* NOTREACHTED */
    }
    return true;
}
#endif // Q_OS_MAC

#endif // NATRON_CRASH_REPORTER_USE_FORK
} // anon namespace


void
dumpRequest_xPlatform(const QString& filePath)
{
    CallbacksManager::instance()->s_emitDoDumpCallBackOnMainThread(filePath);
}

using namespace google_breakpad;

CallbacksManager::CallbacksManager()
    : QObject()
#ifndef NATRON_CRASH_REPORTER_USE_FORK
    , _natronProcess(0)
#endif
    , _comServer(0)
    , _comPipeConnection(0)
    , _dumpReceived(false)
    , _mustInitQAppAfterDump(false)
#ifndef REPORTER_CLI_ONLY
    , _dialog(0)
#endif
    , _dumpFilePath()
    , _pipePath()
    , _comPipePath()
    , _crashServer(0)
#ifdef Q_OS_LINUX
    , _serverFD(-1)
    , _clientFD(-1)
#endif
    , _initErr(false)
{
    _instance = this;
}

CallbacksManager::~CallbacksManager()
{
    delete _comServer;

    delete _crashServer;

#ifndef NATRON_CRASH_REPORTER_USE_FORK
    if (_natronProcess) {
        //Wait at most 5sec then exit
        _natronProcess->waitForFinished(5000);
        delete _natronProcess;
    }
#endif


    delete qApp;

    for (std::size_t i = 0; i < _argv.size(); ++i) {
        free(_argv[i]);
    }

    _instance = 0;
}

int
CallbacksManager::exec()
{
    assert(qApp);
    int retCode =  qApp->exec();
#ifndef REPORTER_CLI_ONLY
    if (_mustInitQAppAfterDump) {
        /*
           We are in gui mode, clean-up everything related to Qt, Kill
           the QCoreApplication and create a QApplication to pop-up the crash dialog instead.
           We do this so that the application icon showsup in the taskbar/dock only now and not when
           we started the crash reporter
         */
        _mustInitQAppAfterDump = false;

        /*
           Cleanup everything related
         */
        delete _comServer;
        _comServer = 0;

        delete _crashServer;
        _crashServer = 0;

#ifndef NATRON_CRASH_REPORTER_USE_FORK
        _natronProcess->waitForFinished(2000);
        delete _natronProcess;
        _natronProcess = 0;
#endif

        delete qApp;

        new QApplication(_argc, &_argv.front());
        assert(qApp);
        qApp->setQuitOnLastWindowClosed(false);

        processCrashReport();

        retCode = qApp->exec();
    }
#endif // REPORTER_CLI_ONLY
    return retCode;
}

static QString
getNatronBinaryFilePathFromCrashReporterDirPath(const QString& crashReporterDirPath)
{
    QString ret = crashReporterDirPath;

    if ( !ret.endsWith( QLatin1Char('/') ) ) {
        ret.append( QLatin1Char('/') );
    }

#ifndef REPORTER_CLI_ONLY
#ifdef Q_OS_UNIX
    ret.append( QString::fromUtf8("Natron-bin") );
#elif defined(Q_OS_WIN)
    ret.append( QString::fromUtf8("Natron-bin.exe") );
#endif
#else //REPORTER_CLI_ONLY
#ifdef Q_OS_UNIX
    ret.append( QString::fromUtf8("NatronRenderer-bin") );
#elif defined(Q_OS_WIN)
    ret.append( QString::fromUtf8("NatronRenderer-bin.exe") );
#endif
#endif // REPORTER_CLI_ONLY
    return ret;
}

bool
CallbacksManager::hasReceivedDump() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _dumpReceived;
}

bool
CallbacksManager::hasInit() const
{
    return !_initErr;
}

void
CallbacksManager::onSpawnedProcessFinished(int exitCode,
                                           QProcess::ExitStatus status)
{
    QString code;

    if (status == QProcess::CrashExit) {
        code =  QString::fromUtf8("crashed");
    } else if (status == QProcess::NormalExit) {
        code =  QString::fromUtf8("finished");
    }
    qDebug() << "Spawned process" << code << "with exit code:" << exitCode;

    EXIT_APP(exitCode, false);
}

void
CallbacksManager::onSpawnedProcessError(QProcess::ProcessError error)
{
    bool mustExit = false;

    switch (error) {
    case QProcess::FailedToStart:
        std::cerr << "Spawned process executable does not exist or the system lacks necessary resources, exiting." << std::endl;
        mustExit = true;
        break;
    case QProcess::Crashed:
        std::cerr << "Spawned process crashed, exiting." << std::endl;
        mustExit = true;
        break;
    case QProcess::Timedout:
        std::cerr << "Spawned process timedout, exiting." << std::endl;
        mustExit = true;
        break;
    case QProcess::ReadError:
        break;
    case QProcess::WriteError:
        break;
    case QProcess::UnknownError:
        break;
    }

    if (mustExit) {
        _initErr = true;
        EXIT_APP(1, false);
    }
}

void
CallbacksManager::initW(int& argc, wchar_t** argv)
{
    _argc = argc;
    _argv.resize(argc);
    for (int i = 0; i < argc; ++i) {
        std::wstring ws(argv[i]);
        std::string str = NATRON_NAMESPACE::StrUtils::utf16_to_utf8(ws);
        _argv[i] = strdup(str.c_str());
    }
    initInternal();
}

void
CallbacksManager::init(int& argc,
                       char** argv)
{
    _argc = argc;
    _argv.resize(argc);
    for (int i = 0; i < argc; ++i) {
        _argv[i] = strdup(argv[i]);
    }
    initInternal();
}

void
CallbacksManager::initInternal()
{
#ifdef NATRON_USE_BREAKPAD
    bool enableBreakpad = true;
#else
    bool enableBreakpad = false;
#endif

    /*
       Check the value of the "Enable crash reports" parameter of the preferences of Natron
     */
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );
    if ( settings.contains( QString::fromUtf8("enableCrashReports") ) ) {
        bool userEnableCrashReports = settings.value( QString::fromUtf8("enableCrashReports") ).toBool();
        if (!userEnableCrashReports) {
            enableBreakpad = false;
        }
    }

    // Note it is very important to pass a reference to _argc to QCoreApplication and that this int lives
    // thoughout the lifetime of QCoreApplication because it holds a reference to this int the whole time.
    QCoreApplication* qtApp = new QCoreApplication(_argc, &_argv.front());
    (void)qtApp;


    if (enableBreakpad) {
        //This will throw an exception if it fails to start the crash generation server
        initCrashGenerationServer();
        assert(_crashServer);
    }

    /*
       At this point the crash generation server is created
       qApp has been defined so far
     */
    assert(qApp);

    QString crashReporterBinaryFilePath = qApp->applicationFilePath();
    QString natronBinaryPath = qApp->applicationDirPath();
    natronBinaryPath = getNatronBinaryFilePathFromCrashReporterDirPath(natronBinaryPath);

    if ( !QFile::exists(natronBinaryPath) ) {
        _initErr = true;
        std::stringstream ss;
        ss << natronBinaryPath.toStdString() << ": no such file or directory.";
        throw std::runtime_error( ss.str() );

        return;
    }

#ifndef NATRON_CRASH_REPORTER_USE_FORK

    _natronProcess = new QProcess();
    QObject::connect( _natronProcess, SIGNAL(readyReadStandardOutput()), this, SLOT(onNatronProcessStdOutWrittenTo()) );
    QObject::connect( _natronProcess, SIGNAL(readyReadStandardError()), this, SLOT(onNatronProcessStdErrWrittenTo()) );
    QObject::connect( _natronProcess, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onSpawnedProcessFinished(int,QProcess::ExitStatus)) );
    QObject::connect( _natronProcess, SIGNAL(error(QProcess::ProcessError)), this, SLOT(onSpawnedProcessError(QProcess::ProcessError)) );
    QStringList processArgs;
    for (int i = 0; i < _argc; ++i) {
        processArgs.push_back( QString::fromUtf8(_argv[i]) );
    }
    if (enableBreakpad) {
        processArgs.push_back( QString::fromUtf8("--" NATRON_BREAKPAD_PROCESS_EXEC) );
        processArgs.push_back(crashReporterBinaryFilePath);
        processArgs.push_back( QString::fromUtf8("--" NATRON_BREAKPAD_PROCESS_PID) );
        qint64 crashReporterPid = qApp->applicationPid();
        QString pidStr = QString::number(crashReporterPid);
        processArgs.push_back(pidStr);
        processArgs.push_back( QString::fromUtf8("--" NATRON_BREAKPAD_CLIENT_FD_ARG) );
        processArgs.push_back( QString::number(-1) );
        processArgs.push_back( QString::fromUtf8("--" NATRON_BREAKPAD_PIPE_ARG) );
        processArgs.push_back(_pipePath);
        processArgs.push_back( QString::fromUtf8("--" NATRON_BREAKPAD_COM_PIPE_ARG) );
        processArgs.push_back(_comPipePath);
    }

    _natronProcess->start(natronBinaryPath, processArgs);

    //_natronProcess = natronProcess;
#else // NATRON_CRASH_REPORTER_USE_FORK


    setChildDeadSignal();

    std::vector<char*> argvChild(_argc);
    for (int i = 0; i < _argc; ++i) {
        argvChild[i] = strdup(_argv[i]);
    }

    if (enableBreakpad) {
        argvChild.push_back( strdup("--" NATRON_BREAKPAD_PROCESS_EXEC) );
        argvChild.push_back( qstringToMallocCharArray(crashReporterBinaryFilePath) );

        argvChild.push_back( strdup("--" NATRON_BREAKPAD_PROCESS_PID) );
        QString pidStr = QString::number( (qint64)getpid() );
        argvChild.push_back( qstringToMallocCharArray(pidStr) );

        argvChild.push_back( strdup("--" NATRON_BREAKPAD_CLIENT_FD_ARG) );
        {
            char pipe_fd_string[8];
            sprintf(pipe_fd_string, "%d",
#ifdef Q_OS_LINUX
                    _clientFD
#else
                    -1
#endif
                    );
            argvChild.push_back(pipe_fd_string);
        }
        argvChild.push_back( strdup("--" NATRON_BREAKPAD_PIPE_ARG) );
        argvChild.push_back( qstringToMallocCharArray(_pipePath) );
        argvChild.push_back( strdup("--" NATRON_BREAKPAD_COM_PIPE_ARG) );
        argvChild.push_back( qstringToMallocCharArray(_comPipePath) );
    }
    argvChild.push_back(NULL);

    char** childargv_p = &argvChild.front();
    std::string natronStdBinPath = natronBinaryPath.toStdString();
    const char* natronBinPath_p = natronStdBinPath.c_str();

    /*
       Directly fork this process so that we have no variable yet allocated.
       The child process will exec the actual Natron process. We have to do this in order
       for google-breakpad to correctly work:
       This process will use ptrace to inspect the crashing thread of Natron, but it can only
       do so on some Linux distro if Natron is a child process.
       Also the crash reporter and Natron must share the same file descriptors for the crash generation
       server to work.
     */
    pid_t natronPID = fork();

    if (natronPID == -1) {
        std::stringstream ss;
        ss << "Fork failed";
        throw std::runtime_error( ss.str() );

        return;
    } else if (natronPID == 0) {
        /*
           We are the child process (i.e: Natron)
         */

#ifdef Q_OS_MAC
        // on OSX, try to execute in 32-bit mode if this is executed in 32-bit mode
        
        /* We're weak-linking to posix-spawnv to ensure that
         * an executable build on 10.5 can work on 10.4.
         */
        if (&posix_spawn != NULL) {
            posix_spawnattr_t spawnattr = NULL;
            const char* exec_path = natronBinPath_p;

            bool ok = setup_spawnattr(&spawnattr);
            if (ok) {
                posix_spawn(NULL, exec_path, NULL,
                            &spawnattr, (char* const*)childargv_p, environ);
                perror("posix_spawn");
            }
        }

#endif
        if (execve(natronBinPath_p, childargv_p, environ) < 0) {
            std::stringstream ss;
            ss << "Forked process failed to execute " << natronBinaryPath.toStdString() << ": " << strerror(errno) << " (" << errno << ")";
            throw std::runtime_error( ss.str() );

            return;
        }
    } // child process

#endif // NATRON_CRASH_REPORTER_USE_FORK
} // initInternal



static QString
getVersionString()
{
    QString versionStr( QString::fromUtf8(NATRON_VERSION_STRING) );

    versionStr.append( QString::fromUtf8(" ") );
    versionStr.append( QString::fromUtf8(NATRON_DEVELOPMENT_STATUS) );
    if ( QString::fromUtf8(NATRON_DEVELOPMENT_STATUS) == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_CANDIDATE) ) {
        versionStr += QLatin1Char(' ');
        versionStr += QString::number(NATRON_BUILD_NUMBER);
    }

    return versionStr;
}

#ifdef Q_OS_LINUX
static QString
getLinuxVersionString()
{
    QFile os_release( QString::fromUtf8("/etc/os-release") );
    QFile redhat_release( QString::fromUtf8("/etc/redhat-release") );
    QFile debian_release( QString::fromUtf8("/etc/debian_version") );
    QString linux_release;

    if ( os_release.exists() ) {
        if ( os_release.open(QIODevice::ReadOnly | QIODevice::Text) ) {
            QString os_release_data = QString::fromUtf8( os_release.readAll() );
            if ( !os_release_data.isEmpty() ) {
                QStringList os_release_split = os_release_data.split(QString::fromUtf8("\n"), QString::SkipEmptyParts);
                for (int i = 0; i < os_release_split.size(); ++i) {
                    if ( os_release_split.at(i).startsWith( QString::fromUtf8("PRETTY_NAME=") ) ) {
                        linux_release = os_release_split.at(i);
                        linux_release.replace( QString::fromUtf8("PRETTY_NAME="), QString::fromUtf8("") ).replace( QString::fromUtf8("\""), QString::fromUtf8("") );
                    }
                }
            }
            os_release.close();
        }
    } else if ( redhat_release.exists() ) {
        if ( redhat_release.open(QIODevice::ReadOnly | QIODevice::Text) ) {
            QString redhat_release_data = QString::fromUtf8( redhat_release.readAll() );
            if ( !redhat_release_data.isEmpty() ) {
                linux_release = redhat_release_data;
            }
            redhat_release.close();
        }
    } else if ( debian_release.exists() ) {
        if ( debian_release.open(QIODevice::ReadOnly | QIODevice::Text) ) {
            QString debian_release_data = QString::fromUtf8( debian_release.readAll() );
            if ( !debian_release_data.isEmpty() ) {
                linux_release = QString::fromUtf8("Debian ") + debian_release_data;
            }
            debian_release.close();
        }
    }

    return linux_release;
}

#endif

void
CallbacksManager::onDoDumpOnMainThread(const QString& filePath)
{
    assert( QThread::currentThread() == qApp->thread() );

    _dumpReceived = true;
    _dumpFilePath = filePath;
#ifndef REPORTER_CLI_ONLY
    /*
       We are in need to popup the dialog but are still governed by the QCoreApplication, quit the event loop and re-create a
       QApplication, see exec()
     */
    _mustInitQAppAfterDump = true;
    qApp->exit(0);

    return;
#else
    //In CLI mode we don't need a QApplication...
    processCrashReport();
#endif
}

void
CallbacksManager::processCrashReport()
{
#ifdef REPORTER_CLI_ONLY
    saveCrashReport(false /* don't open */, true /* quit when done */);
    ///@todo We must notify the user the log is available at filePath but we don't have access to the terminal with this process
#else
    assert(!_dialog);
    _dialog = new CrashDialog(_dumpFilePath);
    QObject::connect( _dialog, SIGNAL(rejected()), this, SLOT(onCrashDialogFinished()) );
    QObject::connect( _dialog, SIGNAL(accepted()), this, SLOT(onCrashDialogFinished()) );
    _dialog->raise();
    _dialog->show();
#endif
}

void
CallbacksManager::saveCrashReport(bool openReport,
                                  bool quitWhenDone,
                                  const QString &GLrendererInfo,
                                  const QString &GLversionInfo,
                                  const QString &GLvendorInfo,
                                  const QString &GLshaderInfo,
                                  const QString &/*GLextInfo*/)
{
    if ( !QFile::exists(_dumpFilePath) ) {
        return;
    }

    // get crash result
    std::vector<std::string> storage;
    storage.push_back( QString::fromUtf8("%1/../Resources/symbols").arg( qApp->applicationDirPath() ).toStdString() );
    Breakdown::CrashResult report = Breakdown::generateCrashResult(_dumpFilePath.toStdString(), storage);
    if (report.frames.size() == 0) {
        return;
    }

    // get natron info
    const QString gitHash = QString::fromUtf8(GIT_COMMIT);
    const QString gitBranch = QString::fromUtf8(GIT_BRANCH);
    const QString IOGitHash = QString::fromUtf8(IO_GIT_COMMIT);
    const QString MiscGitHash = QString::fromUtf8(MISC_GIT_COMMIT);
    const QString ArenaGitHash = QString::fromUtf8(ARENA_GIT_COMMIT);
    QString linuxVersion;
#ifdef Q_OS_LINUX
    linuxVersion = getLinuxVersionString();
#endif

    // generate HTML
    QString body;
    body.append( QString::fromUtf8("System Information\n---\n\n") );
    body.append( QString::fromUtf8(" * **Platform**: `%1`\n").arg( linuxVersion.isEmpty()? QString::fromStdString(report.platform) : linuxVersion ) );
    body.append( QString::fromUtf8(" * **Natron**: `%1` @ https://github.com/NatronGitHub/Natron/commit/%2 *(%3)*\n").arg( getVersionString() ).arg(gitHash).arg(gitBranch) );
    if ( !IOGitHash.isEmpty() ) {
        body.append( QString::fromUtf8(" * **openfx-io**: https://github.com/NatronGitHub/openfx-io/commit/%1\n").arg(IOGitHash) );
    }
    if ( !MiscGitHash.isEmpty() ) {
        body.append( QString::fromUtf8(" * **openfx-misc**: https://github.com/NatronGitHub/openfx-misc/commit/%1\n").arg(MiscGitHash) );
    }
    if ( !ArenaGitHash.isEmpty() ) {
        body.append( QString::fromUtf8(" * **openfx-arena**: https://github.com/NatronGitHub/openfx-arena/commit/%1\n").arg(ArenaGitHash) );
    }
    if ( !GLversionInfo.isEmpty() ) {
        body.append( QString::fromUtf8(" * **OpenGL**: `%1`\n").arg(GLversionInfo) );
        if ( !GLrendererInfo.isEmpty() ) {
            body.append( QString::fromUtf8("   * **Renderer**: `%1`\n").arg(GLrendererInfo) );
        }
        if ( !GLvendorInfo.isEmpty() ) {
            body.append( QString::fromUtf8("   * **Vendor**: `%1`\n").arg(GLvendorInfo) );
        }
        if ( !GLshaderInfo.isEmpty() ) {
            body.append( QString::fromUtf8("   * **Shader**:  `%1`\n\n").arg(GLshaderInfo) );
        }
    }

    QString html = QString::fromUtf8("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">\n");
    html.append( QString::fromUtf8("<html>\n<head>\n<title>Natron Crash Report</title>\n</head>\n<body>\n<h1>Natron Crash Report</h1>\n") );
    html.append( QString::fromUtf8("<pre>\n%1\n%2\n</pre>\n").arg(body).arg( QString::fromStdString( Breakdown::generateCrashResultPlainText(report) ) ) );
    html.append( QString::fromUtf8("</body>\n</html>") );

    // save HTML
    QString filename = QString(_dumpFilePath).replace( QString::fromUtf8(".dmp"), QString::fromUtf8(".html") );
    QFile file(filename);
    if ( file.open(QIODevice::WriteOnly | QIODevice::Text) ) {
        file.write( html.toUtf8() );
        file.close();
    }

    // open HTML
    if (QFile::exists(filename) && openReport) {
#ifndef REPORTER_CLI_ONLY
        QDesktopServices::openUrl( QUrl::fromLocalFile(filename) );
#endif
    }

    // quit when done
    if (quitWhenDone) {
        EXIT_APP(0, true);
    }
}

void
CallbacksManager::onCrashDialogFinished()
{
#ifndef REPORTER_CLI_ONLY
    assert( _dialog == qobject_cast<CrashDialog*>( sender() ) );
    if (!_dialog) {
        return;
    }

    bool doSave = false;
    CrashDialog::UserChoice ret = CrashDialog::eUserChoiceIgnore;
    QDialog::DialogCode code =  (QDialog::DialogCode)_dialog->result();
    if (code == QDialog::Accepted) {
        ret = _dialog->getUserChoice();
    }

    switch (ret) {
    case CrashDialog::eUserChoiceSave:
    case CrashDialog::eUserChoiceUpload:
        doSave = true;
        break;
    case CrashDialog::eUserChoiceIgnore:
        break;
    }

    if (doSave) {
        saveCrashReport( true /* open report */, true /* quit when done */, _dialog->getGLrenderer(), _dialog->getGLversion(), _dialog->getGLvendor(), _dialog->getGLshader(), _dialog->getGLext() );
    } else {
        _dialog->deleteLater();
        EXIT_APP(0, true);
    }
#endif
}

void
CallbacksManager::s_emitDoDumpCallBackOnMainThread(const QString& filePath)
{
    if ( QFile::exists(filePath) ) {
        Q_EMIT doDumpCallBackOnMainThread(filePath);
    }
}

void
CallbacksManager::s_emitDoExitCallBackOnMainThread(int exitCode,
                                                   bool exitEvenIfDumpedReceived)
{
    Q_EMIT doExitCallbackOnMainThread(exitCode, exitEvenIfDumpedReceived);
}

void
CallbacksManager::onDoExitOnMainThread(int exitCode,
                                       bool exitEvenIfDumpedReceived)
{
    if (!CallbacksManager::instance()->hasReceivedDump() || exitEvenIfDumpedReceived) {
        qApp->exit(exitCode);
    }
}

#ifdef Q_OS_WIN32
/*Converts a std::string to wide string*/
static inline std::wstring
utf8_to_utf16(const std::string & s)
{
#ifdef Q_OS_WIN32
    std::wstring native;


    native.resize(MultiByteToWideChar (CP_UTF8, 0, s.c_str(), -1, NULL, 0) - 1);
    MultiByteToWideChar ( CP_UTF8, 0, s.c_str(), s.size(), &native[0], (int)native.size() );

    return native;

#else
    std::wstring dest;
    size_t max = s.size() * 4;
    mbtowc (NULL, NULL, max);  /* reset mbtowc */

    const char* cstr = s.c_str();

    while (max > 0) {
        wchar_t w;
        size_t length = mbtowc(&w, cstr, max);
        if (length < 1) {
            break;
        }
        dest.push_back(w);
        cstr += length;
        max -= length;
    }

    return dest;
#endif
} // utf8_to_utf16

#endif


#if defined(Q_OS_MAC)
void
OnClientDumpRequest(void */*context*/,
                    const ClientInfo & /*client_info*/,
                    const std::string &file_path)
{
    dumpRequest_xPlatform( QString::fromUtf8( file_path.c_str() ) );
}

#elif defined(Q_OS_LINUX)
void
OnClientDumpRequest(void* /*context*/,
                    const ClientInfo* /*client_info*/,
                    const string* file_path)
{
    dumpRequest_xPlatform( QString::fromUtf8( file_path->c_str() ) );
}

#elif defined(Q_OS_WIN32)
void
OnClientDumpRequest(void* /*context*/,
                    const google_breakpad::ClientInfo* /*client_info*/,
                    const std::wstring* file_path)
{
    QString str = QString::fromWCharArray( file_path->c_str() );

    dumpRequest_xPlatform(str);
}

#endif

void
CallbacksManager::createCrashGenerationServer()
{
    /*
     * We initialize the CrashGenerationServer now that the connection is made between Natron & the Crash reporter
     */
#if defined(Q_OS_MAC)
    _crashServer = new CrashGenerationServer( _pipePath.toStdString().c_str(),
                                              0, // filter cb
                                              0, // filter ctx
                                              OnClientDumpRequest, // dump cb
                                              0, // dump ctx
                                              0, // exit cb
                                              0, // exit ctx
                                              true, // auto-generate dumps
                                              _dumpDirPath.toStdString() ); // path to dump to
#elif defined(Q_OS_LINUX)
    std::string stdDumpPath = _dumpDirPath.toStdString();
    _crashServer = new CrashGenerationServer(_serverFD,
                                             OnClientDumpRequest, // dump cb
                                             0, // dump ctx
                                             0, // exit cb
                                             0, // exit ctx
                                             true, // auto-generate dumps
                                             &stdDumpPath); // path to dump to
#elif defined(Q_OS_WIN32)
    _pipePath.replace( QLatin1Char('/'), QLatin1Char('\\') );
    std::string pipeName = _pipePath.toStdString();
    std::wstring wpipeName = utf8_to_utf16(pipeName);
    std::string stdDumPath = _dumpDirPath.toStdString();
    std::wstring stdWDumpPath = utf8_to_utf16(stdDumPath);
    _crashServer = new CrashGenerationServer(wpipeName,
                                             0, // SECURITY ATTRS
                                             0, // on client connected cb
                                             0, // on client connected ctx
                                             OnClientDumpRequest, // dump cb
                                             0, // dump ctx
                                             0, // exit cb
                                             0, // exit ctx
                                             0, // upload request cb
                                             0, //  upload request ctx
                                             true, // auto-generate dumps
                                             &stdWDumpPath); // path to dump to
#endif
    if ( !_crashServer->Start() ) {
        std::stringstream ss;
        ss << "Failure to start breakpad server, crash generation will fail." << std::endl;
        ss << "Dump path is: " << _dumpDirPath.toStdString() << std::endl;
        ss << "Pipe name is: " << _pipePath.toStdString() << std::endl;
        throw std::runtime_error( ss.str() );
    }
}

void
CallbacksManager::onNatronProcessStdOutWrittenTo()
{
#ifndef NATRON_CRASH_REPORTER_USE_FORK
    QString str = QString::fromUtf8( _natronProcess->readAllStandardOutput().data() );
    while ( str.endsWith( QLatin1Char('\n') ) ) {
        str.chop(1);
    }
    std::cout << str.toStdString() << std::endl;
#endif
}

void
CallbacksManager::onNatronProcessStdErrWrittenTo()
{
#ifndef NATRON_CRASH_REPORTER_USE_FORK
    QString str = QString::fromUtf8( _natronProcess->readAllStandardError().data() );
    while ( str.endsWith( QLatin1Char('\n') ) ) {
        str.chop(1);
    }
    std::cerr << str.toStdString() << std::endl;
#endif
}

void
CallbacksManager::initCrashGenerationServer()
{
    QObject::connect( this, SIGNAL(doDumpCallBackOnMainThread(QString)), this, SLOT(onDoDumpOnMainThread(QString)) );
    QObject::connect( this, SIGNAL(doExitCallbackOnMainThread(int,bool)), this, SLOT(onDoExitOnMainThread(int,bool)) );

#ifdef Q_OS_LINUX
    if ( !google_breakpad::CrashGenerationServer::CreateReportChannel(&_serverFD, &_clientFD) ) {
        throw std::runtime_error("breakpad: Failure to create named pipe for the CrashGenerationServer (CreateReportChannel).");

        return;
    }
#endif

    /*
       Use a temporary file to get a random file name for the pipes.
       We use 2 different pipe: 1 for the CrashReporter to notify to Natron that it has started correctly, and another
       one that is used by the google_breakpad server itself.
     */
    assert(qApp);
    _dumpDirPath = QDir::tempPath();
    {
        QString tmpFileName;
#if defined(Q_OS_UNIX)
        tmpFileName = _dumpDirPath;
#else
        tmpFileName += QString::fromUtf8("//./pipe");
#endif
        tmpFileName += QLatin1Char('/');
        tmpFileName += QString::fromUtf8(NATRON_APPLICATION_NAME);
        tmpFileName += QString::fromUtf8("_CRASH_PIPE_");
        {
#if defined(Q_OS_UNIX)
            QTemporaryFile tmpf(tmpFileName);
            tmpFileName = tmpf.fileName();
            if ( tmpf.exists() ) {
                tmpf.remove();
            }
#else
            QTemporaryFile tmpf;
            tmpf.open();
            QString tmpFilePath = tmpf.fileName();
            QString baseName;
            int lastSlash = tmpFilePath.lastIndexOf( QLatin1Char('/') );
            if ( (lastSlash != -1) && (lastSlash < tmpFilePath.size() - 1) ) {
                baseName = tmpFilePath.mid(lastSlash + 1);
            } else {
                baseName = tmpFilePath;
            }
            tmpFileName += baseName;
            if ( tmpf.exists() ) {
                tmpf.remove();
            }
#endif
        }

//#ifndef Q_OS_WIN32
        _pipePath = tmpFileName;
/*#else
        int foundLastSlash = tmpFileName.lastIndexOf(QLatin1Char('/'));
        if (foundLastSlash !=1) {
            _pipePath = QString::fromUtf8("//./pipe/");
            if (foundLastSlash < tmpFileName.size() - 1) {
                QString toAppend(QString::fromUtf8("CRASH_PIPE_"));
                toAppend.append(tmpFileName.mid(foundLastSlash + 1));
                _pipePath.append(toAppend);
            } else {
                _pipePath.append(QString::fromUtf8("CRASH_PIPE_"));
                _pipePath.append(tmpFileName);
            }
        }
 #endif*/
    }

    _comPipePath = _pipePath + QString::fromUtf8("IPC_COM");

    _comServer = new QLocalServer;
    QObject::connect( _comServer, SIGNAL(newConnection()), this, SLOT(onComPipeConnectionPending()) );
    _comServer->listen(_comPipePath);

    createCrashGenerationServer();
} // CallbacksManager::initCrashGenerationServer

void
CallbacksManager::onComPipeConnectionPending()
{
    _comPipeConnection = _comServer->nextPendingConnection();
    if ( _comPipeConnection->canReadLine() ) {
        onComPipeDataWrittenTo();
    }
    QObject::connect( _comPipeConnection, SIGNAL(readyRead()), this, SLOT(onComPipeDataWrittenTo()) );
}

void
CallbacksManager::onComPipeDataWrittenTo()
{
    QString str = QString::fromUtf8( _comPipeConnection->readLine() );

    while ( str.endsWith( QLatin1Char('\n') ) ) {
        str.chop(1);
    }
    if ( str == QString::fromUtf8(NATRON_NATRON_TO_BREAKPAD_EXISTENCE_CHECK) ) {
        _comPipeConnection->write(NATRON_NATRON_TO_BREAKPAD_EXISTENCE_CHECK_ACK "\n");
        _comPipeConnection->flush();
    }
}

