// The Crackdown port's launcher. Three pages:
//  - Play: the profile to play as, and Play (build/crackdown, its log in build/play.log).
//  - Install: the folder dumped from the Crackdown disc (required), title update 6 (needed to build: the
//    port is made for that version) and downloadable content (optional), each of which can be added later,
//    then the build. All of it is devtools/setup.sh's work (the launcher shows its progress lines).
//  - Profiles: the names the game's stats, saves and achievements are kept under (<data>/crackdown-recomp/
//    profiles/<name>/, the runtime's profile.cpp); the one played is settings.ini's "profile = <name>".
// The port's folder is found from the launcher's own (it lives in <port>/build/launcher/), or
// CRACKDOWN_ROOT.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

namespace
{
// ---------------------------------------------------------------------------------------------- places
QString port_root()
{
    if ( qEnvironmentVariableIsSet( "CRACKDOWN_ROOT" ) ) return qEnvironmentVariable( "CRACKDOWN_ROOT" );
    QDir d( QCoreApplication::applicationDirPath() );
    for ( int i = 0; i < 5; ++i, d.cdUp() )
        if ( QFileInfo::exists( d.filePath( "devtools/setup.sh" ) ) ) return d.absolutePath();
    return QDir::currentPath();
}
QString xdg( const char *var, const char *fallback )
{
    const QString v = qEnvironmentVariable( var );
    return ( v.isEmpty() ? QDir::homePath() + "/" + fallback : v ) + "/crackdown-recomp";
}
QString data_dir() { return xdg( "XDG_DATA_HOME", ".local/share" ); }
QString settings_file() { return xdg( "XDG_CONFIG_HOME", ".config" ) + "/settings.ini"; }
QString profiles_dir() { return data_dir() + "/profiles"; }

// "key = value" lines.
QMap<QString, QString> read_ini( const QString &path )
{
    QMap<QString, QString> m;
    QFile f( path );
    if ( !f.open( QIODevice::ReadOnly | QIODevice::Text ) ) return m;
    for ( const QString &line : QString::fromUtf8( f.readAll() ).split( '\n' ) )
    {
        const int eq = line.indexOf( " = " );
        if ( eq > 0 ) m[line.left( eq ).trimmed()] = line.mid( eq + 3 ).trimmed();
    }
    return m;
}
// Sets one "key = value" line of settings.ini, the rest left as it is.
bool set_setting( const QString &key, const QString &value )
{
    QFile f( settings_file() );
    QStringList lines;
    if ( f.open( QIODevice::ReadOnly | QIODevice::Text ) ) lines = QString::fromUtf8( f.readAll() ).split( '\n' ), f.close();
    while ( !lines.isEmpty() && lines.last().isEmpty() ) lines.removeLast();
    bool found = false;
    for ( QString &line : lines )
        if ( line.startsWith( key + " = " ) ) line = key + " = " + value, found = true;
    if ( !found ) lines << key + " = " + value;
    QDir().mkpath( QFileInfo( settings_file() ).path() );
    if ( !f.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) ) return false;
    f.write( ( lines.join( '\n' ) + '\n' ).toUtf8() );
    return true;
}

// -------------------------------------------------------------------------------------------- profiles
QString current_profile()
{
    const QString p = read_ini( settings_file() ).value( "profile" );
    return p.isEmpty() ? "Player" : p;
}
QStringList profiles()
{
    QStringList l = QDir( profiles_dir() ).entryList( QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase );
    if ( !l.contains( current_profile() ) ) l.prepend( current_profile() );
    return l;
}
// As the game takes names (settings.cpp): letters, digits and spaces, 15 at most (a gamertag's length).
QString name_problem( const QString &name, const QString &except = {} )
{
    if ( name.isEmpty() ) return "Type a name.";
    if ( name.size() > 15 ) return "15 characters at most.";
    if ( !QRegularExpression( "^[A-Za-z0-9 ]+$" ).match( name ).hasMatch() ) return "Letters, digits and spaces only.";
    for ( const QString &p : profiles() )
        if ( p.compare( name, Qt::CaseInsensitive ) == 0 && p != except ) return "There is a profile of that name already.";
    if ( name.compare( "Player", Qt::CaseInsensitive ) == 0 && except != "Player" ) return "\"Player\" is kept for the first profile.";
    return {};
}
// The profile's XUID, as the runtime makes it (profile.cpp): kept in its folder, so a rename keeps it.
void pin_xuid( const QString &name )
{
    const QString file = profiles_dir() + "/" + name + "/xuid";
    if ( QFileInfo::exists( file ) ) return;
    quint64 v;
    if ( name == "Player" ) v = 0xE000000000000001ull;
    else
    {
        quint64 h = 1469598103934665603ull;
        for ( char ch : name.toUtf8() ) h = ( h ^ (unsigned char)ch ) * 1099511628211ull;
        v = 0xE000000000000000ull | ( h & 0x0000FFFFFFFFFFFFull ) | 2;
    }
    QDir().mkpath( profiles_dir() + "/" + name );
    QFile f( file );
    if ( f.open( QIODevice::WriteOnly ) ) f.write( QString( "%1\n" ).arg( v, 16, 16, QChar( '0' ) ).toUpper().toUtf8() );
}
QString profile_line( const QString &name )
{
    const QString dir = profiles_dir() + "/" + name;
    const auto s = read_ini( dir + "/summary.ini" );
    QString text = name;
    if ( name == current_profile() ) text += "   (playing as)";
    if ( !s.isEmpty() )
    {
        text += QString( "\n%1 of %2 achievements, %3 of %4 G" ).arg( s.value( "achievements" ), s.value( "achievements_all" ), s.value( "points" ), s.value( "points_all" ) );
        const qint64 t = s.value( "last_played" ).toLongLong();
        if ( t ) text += ", last played " + QDateTime::fromSecsSinceEpoch( t ).toString( "d MMM yyyy" );
    }
    else
    {
        QFile f( dir + "/achievements.txt" );
        int n = 0;
        if ( f.open( QIODevice::ReadOnly ) )
            for ( const QByteArray &l : f.readAll().split( '\n' ) ) n += !l.trimmed().isEmpty();
        text += n ? QString( "\n%1 achievements" ).arg( n ) : QString( "\nNot played yet" );
    }
    return text;
}

// ------------------------------------------------------------------------------------------------ window
class Launcher : public QMainWindow
{
public:
    Launcher()
    {
        setWindowTitle( "Crackdown" );
        resize( 760, 620 );
        tabs = new QTabWidget;
        setCentralWidget( tabs );
        tabs->addTab( make_play(), "Play" );
        tabs->addTab( make_install(), "Install" );
        tabs->addTab( make_profiles(), "Profiles" );
        refresh();
        run_setup( { "check" }, false, [this]( bool, const QString & ) {} );
        if ( !QFileInfo::exists( root + "/build/crackdown" ) ) tabs->setCurrentIndex( 1 );
    }

public:
    // Debug: each page saved as <dir>/<n>.png (CRACKDOWN_LAUNCHER_SHOTS).
    void shots( const QString &dir )
    {
        QDir().mkpath( dir );
        for ( int i = 0; i < tabs->count(); ++i )
        {
            tabs->setCurrentIndex( i );
            QApplication::processEvents();
            grab().save( dir + QString( "/%1.png" ).arg( i ) );
        }
    }

private:
    QString root = port_root();
    QTabWidget *tabs;
    // Play
    QComboBox *profile_box;
    QLabel *play_state;
    QPushButton *play;
    QProcess *game = nullptr;
    // Install
    QLabel *disc_state, *update_state, *dlc_note, *check_state, *step;
    QListWidget *dlc_list;
    QPushButton *disc_pick, *update_add, *dlc_add, *build;
    QProgressBar *progress;
    QPlainTextEdit *log;
    QProcess *setup = nullptr;
    QStringList missing;
    // Profiles
    QListWidget *profile_list;
    // setup.sh status
    QMap<QString, QString> status;
    QStringList dlcs;

    QWidget *make_play()
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout( w );
        auto *title = new QLabel( "CRACKDOWN" );
        QFont f = title->font();
        f.setPointSize( 34 ), f.setBold( true ), f.setLetterSpacing( QFont::PercentageSpacing, 110 );
        title->setFont( f );
        title->setAlignment( Qt::AlignCenter );
        auto *sub = new QLabel( "Native PC port" );
        sub->setAlignment( Qt::AlignCenter );
        v->addStretch();
        v->addWidget( title );
        v->addWidget( sub );
        v->addSpacing( 30 );
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget( new QLabel( "Profile:" ) );
        profile_box = new QComboBox;
        profile_box->setMinimumWidth( 200 );
        row->addWidget( profile_box );
        auto *manage = new QPushButton( "Manage…" );
        row->addWidget( manage );
        row->addStretch();
        v->addLayout( row );
        v->addSpacing( 20 );
        play = new QPushButton( "Play" );
        play->setMinimumSize( 220, 56 );
        QFont pf = play->font();
        pf.setPointSize( 16 ), pf.setBold( true );
        play->setFont( pf );
        auto *prow = new QHBoxLayout;
        prow->addStretch(), prow->addWidget( play ), prow->addStretch();
        v->addLayout( prow );
        play_state = new QLabel;
        play_state->setAlignment( Qt::AlignCenter );
        play_state->setWordWrap( true );
        v->addWidget( play_state );
        v->addStretch();
        connect( manage, &QPushButton::clicked, this, [this] { tabs->setCurrentIndex( 2 ); } );
        connect( profile_box, &QComboBox::activated, this, [this]( int ) {
            set_setting( "profile", profile_box->currentText() );
            refresh_profiles();
        } );
        connect( play, &QPushButton::clicked, this, [this] { start_game(); } );
        return w;
    }

    QGroupBox *group( const QString &title, QVBoxLayout *&v )
    {
        auto *g = new QGroupBox( title );
        v = new QVBoxLayout( g );
        return g;
    }
    QLabel *note( const QString &text )
    {
        auto *l = new QLabel( text );
        l->setWordWrap( true );
        l->setStyleSheet( "color: palette(mid);" );
        return l;
    }

    QWidget *make_install()
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout( w );
        QVBoxLayout *g;
        // 1. The disc.
        v->addWidget( group( "1. Game disc (required)", g ) );
        g->addWidget( note( "The folder dumped from the Crackdown disc: the one holding Default.xex. Its files are read where they are." ) );
        auto *r = new QHBoxLayout;
        disc_state = new QLabel;
        disc_state->setWordWrap( true );
        disc_pick = new QPushButton( "Choose folder…" );
        r->addWidget( disc_state, 1 ), r->addWidget( disc_pick );
        g->addLayout( r );
        // 2. The title update.
        v->addWidget( group( "2. Title update 6", g ) );
        g->addWidget( note( "Needed to build: the port is made for the game as title update 6 left it. Its package, or the zip "
                            "holding it. You can add it later, the build waits for it." ) );
        r = new QHBoxLayout;
        update_state = new QLabel;
        update_add = new QPushButton( "Add title update…" );
        r->addWidget( update_state, 1 ), r->addWidget( update_add );
        g->addLayout( r );
        // 3. Downloadable content.
        v->addWidget( group( "3. Downloadable content (optional)", g ) );
        dlc_note = note( "Packages, or zips of them. Add them now or any time later: no rebuild needed." );
        g->addWidget( dlc_note );
        r = new QHBoxLayout;
        dlc_list = new QListWidget;
        dlc_list->setMaximumHeight( 70 );
        dlc_add = new QPushButton( "Add content…" );
        r->addWidget( dlc_list, 1 ), r->addWidget( dlc_add, 0, Qt::AlignTop );
        g->addLayout( r );
        // 4. The build.
        v->addWidget( group( "4. Build", g ) );
        check_state = new QLabel;
        check_state->setWordWrap( true );
        g->addWidget( check_state );
        r = new QHBoxLayout;
        step = new QLabel;
        build = new QPushButton( "Build" );
        r->addWidget( step, 1 ), r->addWidget( build );
        g->addLayout( r );
        progress = new QProgressBar;
        progress->setRange( 0, 100 );
        progress->setValue( 0 );
        g->addWidget( progress );
        auto *details = new QCheckBox( "Show details" );
        g->addWidget( details );
        log = new QPlainTextEdit;
        log->setReadOnly( true );
        log->setMaximumBlockCount( 5000 );
        log->setVisible( false );
        QFont mono( "monospace" );
        mono.setStyleHint( QFont::Monospace );
        log->setFont( mono );
        g->addWidget( log, 1 );
        connect( details, &QCheckBox::toggled, log, &QWidget::setVisible );

        connect( disc_pick, &QPushButton::clicked, this, [this] {
            const QString d = QFileDialog::getExistingDirectory( this, "The folder dumped from the Crackdown disc", status.value( "disc" ).isEmpty() ? QDir::homePath() : status.value( "disc" ) );
            if ( !d.isEmpty() ) run_setup( { "disc", d }, true, [this]( bool ok, const QString &e ) { done( ok, e, "The disc folder is set." ); } );
        } );
        connect( update_add, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getOpenFileName( this, "Title update 6 (its package or zip)", QDir::homePath() );
            if ( !f.isEmpty() ) run_setup( { "update", f }, true, [this]( bool ok, const QString &e ) { done( ok, e, "Title update 6 is installed." ); } );
        } );
        connect( dlc_add, &QPushButton::clicked, this, [this] {
            const QStringList fs = QFileDialog::getOpenFileNames( this, "Downloadable content (packages or zips)", QDir::homePath() );
            add_dlc( fs, 0 );
        } );
        connect( build, &QPushButton::clicked, this, [this] {
            progress->setValue( 0 );
            run_setup( { "build" }, true, [this]( bool ok, const QString &e ) {
                progress->setValue( ok ? 100 : progress->value() );
                done( ok, e, "The game is built: play it from the Play page." );
                if ( ok ) tabs->setCurrentIndex( 0 );
            } );
        } );
        return w;
    }
    void add_dlc( const QStringList &files, int i )
    {
        if ( i >= files.size() ) return;
        run_setup( { "dlc", files[i] }, true, [this, files, i]( bool ok, const QString &e ) {
            if ( !ok ) return done( false, e, {} );
            if ( i + 1 < files.size() ) add_dlc( files, i + 1 );
        } );
    }
    void done( bool ok, const QString &error, const QString &message )
    {
        refresh();
        if ( !ok ) QMessageBox::warning( this, "Crackdown", error.isEmpty() ? "That didn't work: see the details." : error );
        else if ( !message.isEmpty() ) step->setText( message );
    }

    QWidget *make_profiles()
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout( w );
        v->addWidget( note( "Your stats, saves and achievements are kept under the profile you play as." ) );
        profile_list = new QListWidget;
        profile_list->setSpacing( 3 );
        v->addWidget( profile_list, 1 );
        auto *r = new QHBoxLayout;
        auto *use = new QPushButton( "Play as" ), *add = new QPushButton( "New…" ), *ren = new QPushButton( "Rename…" ), *del = new QPushButton( "Delete…" );
        r->addWidget( use ), r->addStretch(), r->addWidget( add ), r->addWidget( ren ), r->addWidget( del );
        v->addLayout( r );
        auto selected = [this]() -> QString {
            auto *it = profile_list->currentItem();
            return it ? it->data( Qt::UserRole ).toString() : QString();
        };
        connect( profile_list, &QListWidget::itemDoubleClicked, this, [this]( QListWidgetItem *it ) {
            set_setting( "profile", it->data( Qt::UserRole ).toString() );
            refresh_profiles();
        } );
        connect( use, &QPushButton::clicked, this, [this, selected] {
            if ( selected().isEmpty() ) return;
            set_setting( "profile", selected() );
            refresh_profiles();
        } );
        connect( add, &QPushButton::clicked, this, [this] {
            const QString name = ask_name( "New profile", "Your name (it is your gamertag in the game):", {} );
            if ( name.isEmpty() ) return;
            QDir().mkpath( profiles_dir() + "/" + name );
            pin_xuid( name );
            set_setting( "profile", name );
            refresh_profiles();
        } );
        connect( ren, &QPushButton::clicked, this, [this, selected] {
            const QString old = selected();
            if ( old.isEmpty() || game_running() ) return;
            const QString name = ask_name( "Rename profile", "New name for " + old + ":", old );
            if ( name.isEmpty() || name == old ) return;
            rename_profile( old, name );
        } );
        connect( del, &QPushButton::clicked, this, [this, selected] {
            const QString name = selected();
            if ( name.isEmpty() || game_running() ) return;
            if ( QMessageBox::warning( this, "Delete profile", "Delete " + name + "? Its saves, stats and achievements go with it.",
                                       QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel ) != QMessageBox::Yes )
                return;
            QDir( profiles_dir() + "/" + name ).removeRecursively();
            if ( name == "Player" ) QDir( data_dir() + "/content/00000001" ).removeRecursively();  // its saves (profile.cpp)
            if ( name == current_profile() )
            {
                const QStringList left = QDir( profiles_dir() ).entryList( QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name );
                set_setting( "profile", left.isEmpty() ? "Player" : left.first() );
            }
            refresh_profiles();
        } );
        return w;
    }
    QString ask_name( const QString &title, const QString &label, const QString &except )
    {
        QString name = except;
        while ( true )
        {
            bool ok = false;
            name = QInputDialog::getText( this, title, label, QLineEdit::Normal, name, &ok ).trimmed();
            if ( !ok ) return {};
            const QString problem = name_problem( name, except );
            if ( problem.isEmpty() ) return name;
            QMessageBox::information( this, title, problem );
        }
    }
    void rename_profile( const QString &old, const QString &name )
    {
        pin_xuid( old );  // the profile keeps its XUID (its saves are its)
        const QString from = profiles_dir() + "/" + old, to = profiles_dir() + "/" + name;
        QDir().mkpath( profiles_dir() );
        if ( !QDir().rename( from, to ) ) return void( QMessageBox::warning( this, "Rename profile", "Couldn't rename the profile's folder." ) );
        // "Player" kept its saves in the shared content folder (profile.cpp); a profile of another name keeps
        // them in its own.
        if ( old == "Player" && QFileInfo::exists( data_dir() + "/content/00000001" ) )
        {
            QDir().mkpath( to + "/content" );
            if ( !QDir().rename( data_dir() + "/content/00000001", to + "/content/00000001" ) )
                QMessageBox::warning( this, "Rename profile", "Couldn't move the saves into the renamed profile: they are still in " + data_dir() + "/content/00000001." );
        }
        if ( current_profile() == old ) set_setting( "profile", name );
        refresh_profiles();
    }

    // ------------------------------------------------------------------------------------- setup.sh
    void run_setup( const QStringList &args, bool shown, std::function<void( bool, const QString & )> then )
    {
        if ( setup ) return;
        setup = new QProcess( this );
        setup->setWorkingDirectory( root );
        setup->setProcessChannelMode( QProcess::MergedChannels );
        auto last_error = std::make_shared<QString>();
        auto buffer = std::make_shared<QByteArray>();
        const bool is_build = args.value( 0 ) == "build", is_check = args.value( 0 ) == "check";
        if ( is_check ) missing.clear();
        auto steps = std::make_shared<std::pair<int, int>>( 0, 1 );
        connect( setup, &QProcess::readyRead, this, [=, this] {
            *buffer += setup->readAll();
            int nl;
            while ( ( nl = buffer->indexOf( '\n' ) ) >= 0 )
            {
                const QString line = QString::fromUtf8( buffer->left( nl ) );
                buffer->remove( 0, nl + 1 );
                if ( line.startsWith( "PROGRESS " ) )
                {
                    const int pct = line.mid( 9 ).toInt();
                    progress->setValue( ( ( steps->first - 1 ) * 100 + pct ) / steps->second );
                    continue;
                }
                if ( shown ) log->appendPlainText( line );
                if ( line.startsWith( "STEP " ) )
                {
                    const QRegularExpressionMatch m = QRegularExpression( "^STEP (\\d+)/(\\d+) (.*)$" ).match( line );
                    if ( m.hasMatch() )
                    {
                        *steps = { m.captured( 1 ).toInt(), m.captured( 2 ).toInt() };
                        step->setText( m.captured( 3 ) );
                        progress->setValue( ( steps->first - 1 ) * 100 / steps->second );
                    }
                }
                else if ( line.startsWith( "ERROR " ) ) *last_error = line.mid( 6 );
                else if ( line.startsWith( "missing = " ) ) missing << line.mid( 10 );
            }
        } );
        connect( setup, &QProcess::finished, this, [=, this]( int code, QProcess::ExitStatus st ) {
            setup->deleteLater();
            setup = nullptr;
            if ( is_check ) check_state->setText( missing.isEmpty() ? "Everything the build needs is installed."
                                                                    : "Missing for the build: " + missing.join( ", " ) + "." );
            set_busy( false );
            then( code == 0 && st == QProcess::NormalExit, *last_error );
        } );
        if ( shown ) log->appendPlainText( "$ setup.sh " + args.join( ' ' ) );
        if ( is_build ) step->setText( "Starting…" );
        set_busy( true );
        setup->start( "bash", QStringList{ root + "/devtools/setup.sh" } + args );
    }
    void set_busy( bool busy )
    {
        for ( QPushButton *b : { disc_pick, update_add, dlc_add } ) b->setEnabled( !busy && !game_running() );
        refresh_buttons( busy );
    }

    // ----------------------------------------------------------------------------------------- state
    void refresh()
    {
        QProcess p;
        p.setWorkingDirectory( root );
        p.start( "bash", { root + "/devtools/setup.sh", "status" } );
        p.waitForFinished( 15000 );
        status.clear();
        dlcs.clear();
        for ( const QString &line : QString::fromUtf8( p.readAllStandardOutput() ).split( '\n' ) )
        {
            const int eq = line.indexOf( " = " );
            if ( eq < 0 ) continue;
            if ( line.left( eq ) == "dlc" ) dlcs << line.mid( eq + 3 );
            else status[line.left( eq )] = line.mid( eq + 3 );
        }
        const bool disc = !status.value( "disc" ).isEmpty(), update = status.value( "update" ) == "6";
        disc_state->setText( disc ? "✔ " + status.value( "disc" ) : "Not chosen yet" );
        update_state->setText( update ? "✔ Title update 6 installed" : "Not installed yet (needed to build)" );
        dlc_list->clear();
        for ( const QString &d : dlcs ) dlc_list->addItem( "✔ " + d );
        if ( dlcs.isEmpty() ) dlc_list->addItem( "None yet" );
        refresh_buttons( setup != nullptr );
        refresh_profiles();
    }
    bool built() const { return QFileInfo::exists( root + "/build/crackdown" ); }
    bool game_running() const { return game && game->state() != QProcess::NotRunning; }
    void refresh_buttons( bool busy )
    {
        const bool disc = !status.value( "disc" ).isEmpty(), update = status.value( "update" ) == "6";
        build->setEnabled( !busy && disc && update && !game_running() );
        progress->setVisible( busy || ( progress->value() > 0 && progress->value() < 100 ) );
        build->setText( built() ? "Rebuild" : "Build" );
        if ( !busy )
        {
            if ( !disc ) step->setText( "Choose the disc folder to build." );
            else if ( !update ) step->setText( "Add title update 6 to build." );
            else if ( !built() ) step->setText( "Ready to build (it takes a while: the game's code is recompiled)." );
            else if ( step->text().isEmpty() || step->text().startsWith( "Ready" ) || step->text().startsWith( "Add" ) || step->text().startsWith( "Choose" ) )
                step->setText( status.value( "built" ).isEmpty() ? "Built." : "Built " + status.value( "built" ) + "." );
        }
        play->setEnabled( built() && disc && !busy && !game_running() );
        if ( game_running() ) play_state->setText( "Playing…" );
        else if ( !disc || !update ) play_state->setText( "Install the game first (the Install page)." );
        else if ( !built() ) play_state->setText( "Build the game first (the Install page)." );
        else play_state->setText( "" );
    }
    void refresh_profiles()
    {
        const QString cur = current_profile();
        profile_box->clear();
        profile_box->addItems( profiles() );
        profile_box->setCurrentText( cur );
        profile_list->clear();
        for ( const QString &p : profiles() )
        {
            auto *it = new QListWidgetItem( profile_line( p ) );
            it->setData( Qt::UserRole, p );
            if ( p == cur )
            {
                QFont f = it->font();
                f.setBold( true );
                it->setFont( f );
            }
            profile_list->addItem( it );
        }
    }

    // ------------------------------------------------------------------------------------------ play
    void start_game()
    {
        if ( game_running() || !built() ) return;
        set_setting( "profile", profile_box->currentText() );
        pin_xuid( profile_box->currentText() );
        const QString logf = root + "/build/play.log";
        if ( QFileInfo::exists( logf ) )
        {
            QFile::remove( root + "/build/play.prev.log" );
            QFile::rename( logf, root + "/build/play.prev.log" );
        }
        game = new QProcess( this );
        game->setWorkingDirectory( root );
        game->setStandardOutputFile( QProcess::nullDevice() );
        game->setStandardErrorFile( logf );
        connect( game, &QProcess::finished, this, [this]( int code, QProcess::ExitStatus st ) {
            game->deleteLater();
            game = nullptr;
            refresh();
            if ( st != QProcess::NormalExit || code != 0 )
                play_state->setText( "The game stopped unexpectedly (its log: build/play.log)." );
        } );
        game->start( root + "/build/crackdown", { "game_tu6/xex/default.xex" } );
        if ( !game->waitForStarted( 5000 ) )
        {
            QMessageBox::warning( this, "Crackdown", "The game didn't start: " + game->errorString() );
            game->deleteLater();
            game = nullptr;
        }
        set_busy( false );
    }
};
} // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );
    QApplication::setApplicationName( "Crackdown" );
    Launcher w;
    w.show();
    if ( const QString dir = qEnvironmentVariable( "CRACKDOWN_LAUNCHER_SHOTS" ); !dir.isEmpty() )
        QTimer::singleShot( 1500, [&w, &app, dir] { w.shots( dir ); app.quit(); } );
    return app.exec();
}
