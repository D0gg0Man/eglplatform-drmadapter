/* plasma-volume-keys -- bind the hardware volume keys in a session where
 * nothing else does.
 *
 * On this stack KWin provides org.kde.kglobalaccel, but no component ever
 * registers the volume shortcuts (the plasma-pa applet that normally does is
 * not part of the mobile shell here). Register the standard "kmix" component
 * actions and drive PulseAudio via pactl; plasmashell's VolumeOSDListener
 * watches PulseAudio and pops the OSD on any change, so no UI is needed
 * here. If a future plasma-pa registers the same component, its shortcuts
 * simply take over.
 */

#include <KGlobalAccel>
#include <QAction>
#include <QGuiApplication>
#include <QKeySequence>
#include <QProcess>

static void addKey(const char *name, const char *text, Qt::Key key, const QStringList &args)
{
    QAction *a = new QAction(QString::fromLatin1(text), qApp);
    a->setObjectName(QString::fromLatin1(name));
    a->setProperty("componentName", QStringLiteral("kmix"));
    a->setProperty("componentDisplayName", QStringLiteral("Audio Volume"));
    KGlobalAccel::self()->setGlobalShortcut(a, QKeySequence(key));
    QObject::connect(a, &QAction::triggered, [args] {
        QProcess::startDetached(QStringLiteral("pactl"), args);
    });
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("kmix"));
    app.setOrganizationDomain(QStringLiteral("kde.org"));
    app.setQuitOnLastWindowClosed(false);

    addKey("increase_volume", "Increase Volume", Qt::Key_VolumeUp,
           {QStringLiteral("set-sink-volume"), QStringLiteral("@DEFAULT_SINK@"), QStringLiteral("+5%")});
    addKey("decrease_volume", "Decrease Volume", Qt::Key_VolumeDown,
           {QStringLiteral("set-sink-volume"), QStringLiteral("@DEFAULT_SINK@"), QStringLiteral("-5%")});
    addKey("mute", "Mute", Qt::Key_VolumeMute,
           {QStringLiteral("set-sink-mute"), QStringLiteral("@DEFAULT_SINK@"), QStringLiteral("toggle")});

    return app.exec();
}
