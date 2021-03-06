/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2014  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 *
 * Contact : chris@qbittorrent.org
 */

#include <QDebug>
#include <QScopedPointer>

#ifndef DISABLE_GUI
#include <QFont>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QSplashScreen>
#include <QStyle>
#include <QStyleFactory>
#ifdef QBT_STATIC_QT
#include <QtPlugin>
#if (QT_VERSION >= QT_VERSION_CHECK(5, 0, 0))
Q_IMPORT_PLUGIN(QICOPlugin)
#else
Q_IMPORT_PLUGIN(qico)
#endif
#endif // QBT_STATIC_QT
#include "mainwindow.h"
#include "ico.h"
#else // DISABLE_GUI
#include <iostream>
#include <stdio.h>
#include "headlessloader.h"
#endif // DISABLE_GUI

#include "application.h"

#ifdef Q_OS_UNIX
#include <signal.h>
#include <execinfo.h>
#include "stacktrace.h"
#endif // Q_OS_UNIX

#ifdef STACKTRACE_WIN
#include <signal.h>
#include "stacktrace_win.h"
#include "stacktrace_win_dlg.h"
#endif //STACKTRACE_WIN

#include <stdlib.h>
#include "misc.h"
#include "preferences.h"

class MessagesCollector : public QObject
{
    Q_OBJECT
public slots:
    void collectMessage(const QString& message)
    {
        messages.append(message.split("|", QString::SkipEmptyParts));
    }
public:
    QStringList messages;
};

#include "main.moc"

#if defined(Q_OS_WIN) && !defined(QBT_HAS_GETCURRENTPID)
#error You seem to have updated QtSingleApplication without porting our custom QtSingleApplication::getRunningPid() function. Please see previous version to understate how it works.
#endif

// Signal handlers
#if defined(Q_OS_UNIX) || defined(STACKTRACE_WIN)
void sigintHandler(int);
void sigtermHandler(int);
void sigsegvHandler(int);
void sigabrtHandler(int);
#endif

struct QbtCommandLineParameters
{
    bool showVersion;
    bool showHelp;
#ifndef DISABLE_GUI
    bool noSplash;
#else
    bool shouldDaemonize;
#endif
    int webUiPort;
    QStringList torrents;
};

QbtCommandLineParameters parseCommandLine();

#ifndef DISABLE_GUI
void showSplashScreen();
#endif
void displayVersion();
void displayUsage(const char *prg_name);
bool userAgreesWithLegalNotice();
void displayBadArgMessage(const QString& message);

// Main
int main(int argc, char *argv[])
{
    // We must save it here because QApplication constructor may change it
    bool isOneArg = (argc == 2);

    // Create Application
    QScopedPointer<Application> app(new Application("qBittorrent-" + misc::getUserIDString(), argc, argv));

    MessagesCollector* messagesCollector = new MessagesCollector();
    QObject::connect(app.data(), SIGNAL(messageReceived(const QString &)),
                     messagesCollector, SLOT(collectMessage(const QString &)));

    const QbtCommandLineParameters params = parseCommandLine();

    if (params.showVersion) {
        if (isOneArg) {
            displayVersion();
            return EXIT_SUCCESS;
        }
        else {
            displayBadArgMessage(QObject::tr("%1 must be the single command line parameter.")
                                 .arg(QLatin1String("-v (or --version)")));
            return EXIT_FAILURE;
        }
    }

    if (params.showHelp) {
        if (isOneArg) {
            displayUsage(argv[0]);
            return EXIT_SUCCESS;
        }
        else {
            displayBadArgMessage(QObject::tr("%1 must be the single command line parameter.")
                                 .arg(QLatin1String("-h (or --help)")));
            return EXIT_FAILURE;
        }
    }

    if ((params.webUiPort > 0) && (params.webUiPort <= 65535)) {
        Preferences::instance()->setWebUiPort(params.webUiPort);
    }
    else {
        displayBadArgMessage(QObject::tr("%1 must specify the correct port (1 to 65535).")
                             .arg(QLatin1String("--webui-port")));
        return EXIT_FAILURE;
    }

    // Set environment variable
    if (!qputenv("QBITTORRENT", QByteArray(VERSION)))
        std::cerr << "Couldn't set environment variable...\n";

    if (!userAgreesWithLegalNotice())
        return EXIT_SUCCESS;

    // Check if qBittorrent is already running for this user
    if (app->isRunning()) {
#ifdef DISABLE_GUI
        if (params.shouldDaemonize) {
            displayBadArgMessage(QObject::tr("You cannot use %1: qBittorrent is already running for this user.")
                                 .arg(QLatin1String("-d (or --daemon)")));
            return EXIT_FAILURE;
        }
#else
        qDebug("qBittorrent is already running for this user.");
#endif
        misc::msleep(300);
#ifdef Q_OS_WIN
        DWORD pid = (DWORD)app->getRunningPid();
        if (pid > 0) {
            BOOL b = AllowSetForegroundWindow(pid);
            qDebug("AllowSetForegroundWindow() returns %s", b ? "TRUE" : "FALSE");
        }
#endif
        if (!params.torrents.isEmpty()) {
            QString message = params.torrents.join("|");
            qDebug("Passing program parameters to running instance...");
            qDebug("Message: %s", qPrintable(message));
            app->sendMessage(message);
        }
        else { // Raise main window
            app->sendMessage("qbt://show");
        }

        return EXIT_SUCCESS;
    }

    srand(time(0));
#ifdef DISABLE_GUI
    if (params.shouldDaemonize) {
        app.reset(); // Destroy current application
        if ((daemon(1, 0) == 0)) {
            app.reset(new Application("qBittorrent-" + misc::getUserIDString(), argc, argv));
            if (app->isRunning()) {
                // Another instance had time to start.
                return EXIT_FAILURE;
            }
        }
        else {
            qCritical("Something went wrong while daemonizing, exiting...");
            return EXIT_FAILURE;
        }
    }
#else
    if (!(params.noSplash || Preferences::instance()->isSlashScreenDisabled()))
        showSplashScreen();
#endif

#if defined(Q_OS_UNIX) || defined(STACKTRACE_WIN)
    signal(SIGABRT, sigabrtHandler);
    signal(SIGTERM, sigtermHandler);
    signal(SIGINT, sigintHandler);
    signal(SIGSEGV, sigsegvHandler);
#endif

#ifndef DISABLE_GUI
    MainWindow window(0, params.torrents);
    QObject::connect(app.data(), SIGNAL(messageReceived(const QString &)),
                     &window, SLOT(processParams(const QString &)));
    QObject::disconnect(app.data(), SIGNAL(messageReceived(const QString &)),
                        messagesCollector, SLOT(collectMessage(const QString &)));
    window.processParams(messagesCollector->messages);
    delete messagesCollector;
    app->setActivationWindow(&window);
#ifdef Q_OS_MAC
    static_cast<QMacApplication*>(app.data())->setReadyToProcessEvents();
#endif // Q_OS_MAC
#else
    // Load Headless class
    HeadlessLoader loader(params.torrents);
    QObject::connect(app.data(), SIGNAL(messageReceived(const QString &)),
                     &loader, SLOT(processParams(const QString &)));
    QObject::disconnect(app.data(), SIGNAL(messageReceived(const QString &)),
                        messagesCollector, SLOT(collectMessage(const QString &)));
    loader.processParams(messagesCollector->messages);
    delete messagesCollector;
#endif

    int ret = app->exec();
    qDebug("Application has exited");
    return ret;
}

QbtCommandLineParameters parseCommandLine()
{
    QbtCommandLineParameters result = {0};
    QStringList appArguments = qApp->arguments();

    for (int i = 1; i < appArguments.size(); ++i) {
        const QString& arg = appArguments[i];

        if ((arg == QLatin1String("-v")) || (arg == QLatin1String("--version"))) {
            result.showVersion = true;
        }
        else if ((arg == QLatin1String("-h")) || (arg == QLatin1String("--help"))) {
            result.showHelp = true;
        }
        else if (arg.startsWith(QLatin1String("--webui-port="))) {
            QStringList parts = arg.split(QLatin1Char('='));
            if (parts.size() == 2)
                result.webUiPort = parts.last().toInt();
        }
#ifndef DISABLE_GUI
        else if (arg == QLatin1String("--no-splash")) {
            result.noSplash = true;
        }
#else
        else if ((arg == QLatin1String("-d")) || (arg == QLatin1String("--daemon"))) {
            result.shouldDaemonize = true;
        }
#endif
        else {
            QFileInfo torrentPath;
            torrentPath.setFile(arg);

            if (torrentPath.exists())
                result.torrents += torrentPath.absoluteFilePath();
            else
                result.torrents += arg;
        }
    }

    return result;
}

#if defined(Q_OS_UNIX) || defined(STACKTRACE_WIN)
void sigintHandler(int)
{
    signal(SIGINT, 0);
    qDebug("Catching SIGINT, exiting cleanly");
    qApp->exit();
}

void sigtermHandler(int)
{
    signal(SIGTERM, 0);
    qDebug("Catching SIGTERM, exiting cleanly");
    qApp->exit();
}

void sigsegvHandler(int)
{
    signal(SIGABRT, 0);
    signal(SIGSEGV, 0);
#if !defined Q_OS_WIN && !defined Q_OS_HAIKU
    std::cerr << "\n\n*************************************************************\n";
    std::cerr << "Catching SIGSEGV, please report a bug at http://bug.qbittorrent.org\nand provide the following backtrace:\n";
    std::cerr << "qBittorrent version: " << VERSION << std::endl;
    print_stacktrace();
#else
#ifdef STACKTRACE_WIN
    StraceDlg dlg;
    dlg.setStacktraceString(straceWin::getBacktrace());
    dlg.exec();
#endif
#endif
    raise(SIGSEGV);
}

void sigabrtHandler(int)
{
    signal(SIGABRT, 0);
    signal(SIGSEGV, 0);
#if !defined Q_OS_WIN && !defined Q_OS_HAIKU
    std::cerr << "\n\n*************************************************************\n";
    std::cerr << "Catching SIGABRT, please report a bug at http://bug.qbittorrent.org\nand provide the following backtrace:\n";
    std::cerr << "qBittorrent version: " << VERSION << std::endl;
    print_stacktrace();
#else
#ifdef STACKTRACE_WIN
    StraceDlg dlg;
    dlg.setStacktraceString(straceWin::getBacktrace());
    dlg.exec();
#endif
#endif
    raise(SIGABRT);
}
#endif

#ifndef DISABLE_GUI
void showSplashScreen()
{
    QPixmap splash_img(":/Icons/skin/splash.png");
    QPainter painter(&splash_img);
    QString version = VERSION;
    painter.setPen(QPen(Qt::white));
    painter.setFont(QFont("Arial", 22, QFont::Black));
    painter.drawText(224 - painter.fontMetrics().width(version), 270, version);
    QSplashScreen *splash = new QSplashScreen(splash_img, Qt::WindowStaysOnTopHint);
    QTimer::singleShot(1500, splash, SLOT(deleteLater()));
    splash->show();
    qApp->processEvents();
}
#endif

void displayVersion()
{
    std::cout << qPrintable(qApp->applicationName()) << " " << VERSION << std::endl;
}

void displayUsage(const char *prg_name)
{
    std::cout << qPrintable(QObject::tr("Usage:")) << std::endl;
    std::cout << '\t' << prg_name << " -v | --version: " << qPrintable(QObject::tr("displays program version")) << std::endl;
#ifndef DISABLE_GUI
    std::cout << '\t' << prg_name << " --no-splash: " << qPrintable(QObject::tr("disable splash screen")) << std::endl;
#else
    std::cout << '\t' << prg_name << " -d | --daemon: " << qPrintable(QObject::tr("run in daemon-mode (background)")) << std::endl;
#endif
    std::cout << '\t' << prg_name << " -h | --help: " << qPrintable(QObject::tr("displays this help message")) << std::endl;
    std::cout << '\t' << prg_name << " --webui-port=x: " << qPrintable(QObject::tr("changes the webui port (current: %1)").arg(QString::number(Preferences::instance()->getWebUiPort()))) << std::endl;
    std::cout << '\t' << prg_name << " " << qPrintable(QObject::tr("[files or urls]: downloads the torrents passed by the user (optional)")) << std::endl;
}

bool userAgreesWithLegalNotice()
{
    Preferences* const pref = Preferences::instance();
    if (pref->getAcceptedLegal()) // Already accepted once
        return true;

#ifdef DISABLE_GUI
    std::cout << std::endl << "*** " << qPrintable(QObject::tr("Legal Notice")) << " ***" << std::endl;
    std::cout << qPrintable(QObject::tr("qBittorrent is a file sharing program. When you run a torrent, its data will be made available to others by means of upload. Any content you share is your sole responsibility.\n\nNo further notices will be issued.")) << std::endl << std::endl;
    std::cout << qPrintable(QObject::tr("Press %1 key to accept and continue...").arg("'y'")) << std::endl;
    char ret = getchar(); // Read pressed key
    if (ret == 'y' || ret == 'Y') {
        // Save the answer
        pref->setAcceptedLegal(true);
        return true;
    }
#else
    QMessageBox msgBox;
    msgBox.setText(QObject::tr("qBittorrent is a file sharing program. When you run a torrent, its data will be made available to others by means of upload. Any content you share is your sole responsibility.\n\nNo further notices will be issued."));
    msgBox.setWindowTitle(QObject::tr("Legal notice"));
    msgBox.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);
    QAbstractButton *agree_button = msgBox.addButton(QObject::tr("I Agree"), QMessageBox::AcceptRole);
    msgBox.show(); // Need to be shown or to moveToCenter does not work
    msgBox.move(misc::screenCenter(&msgBox));
    msgBox.exec();
    if (msgBox.clickedButton() == agree_button) {
        // Save the answer
        pref->setAcceptedLegal(true);
        return true;
    }
#endif

    return false;
}

void displayBadArgMessage(const QString& message)
{
    std::cerr << qPrintable(QObject::tr("Bad command line: "));
    std::cerr << qPrintable(message) << std::endl;
}
