import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Relay 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 1180; height: 780
    minimumWidth: 640; minimumHeight: 440
    title: "Relay"
    property bool dark: session.dark
    property var backend: session
    property color bg: dark ? "#101814" : "#eff5f0"
    property color panel: dark ? "#17221b" : "#ffffff"
    property color fg: dark ? "#edf3ed" : "#1b3527"
    property color muted: dark ? "#8ea195" : "#627e6a"
    property color line: dark ? "#2b3c30" : "#d7e4d9"
    property color mint: "#b6f3cf"
    property bool hasSession: ["connecting", "loading", "connected", "disconnecting"].indexOf(session.state) >= 0
    property bool fullscreen: visibility === Window.FullScreen
    color: bg
    font.family: "Inter"; font.pixelSize: 14

    function toggleFullscreen() { remote.releaseInput(); visibility = fullscreen ? Window.Windowed : Window.FullScreen }
    function refreshLogin() {
        loginPassword.clear()
        remember.checked = session.remembersPassword(loginUser.text.trim())
        loginError.text = ""
        if (remember.checked) session.loadPassword(loginUser.text.trim())
    }
    function openLogin() {
        if (!session.host) { preferences.open(); return }
        loginUser.text = session.username
        login.open()
        refreshLogin()
        if (loginUser.text) loginPassword.forceActiveFocus()
        else loginUser.forceActiveFocus()
    }
    // Synthetic UI previews never connect to a host or inspect real credentials.
    function previewDialog(name) {
        if (name === "login") { loginUser.text = "utente"; login.open() }
        else if (name === "preferences") preferences.open()
        else if (name === "help") help.open()
    }

    Shortcut { sequence: "F11"; context: Qt.ApplicationShortcut; onActivated: root.toggleFullscreen() }
    Connections {
        target: session
        function onStateChanged() {
            if (session.state === "connected") remote.forceActiveFocus()
            if (session.state === "idle" || session.state === "error") {
                remote.releaseInput()
                if (root.fullscreen) root.visibility = Window.Windowed
            }
        }
        function onPasswordLoaded(user, password) {
            if (login.opened && remember.checked && loginUser.text.trim() === user && !loginPassword.text)
                loginPassword.text = password
        }
        function onCredentialError(message) {
            if (login.opened) loginError.text = message
            else credentialNotice.text = message
        }
    }

    Rectangle {
        id: rail
        visible: !root.hasSession
        width: root.width < 800 ? 68 : 84
        anchors.top: parent.top; anchors.bottom: parent.bottom
        color: root.dark ? "#0c130f" : "#e7efe9"
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: root.line }
        Rectangle {
            width: 44; height: 44; radius: 14; y: 28; anchors.horizontalCenter: parent.horizontalCenter
            color: root.mint
            Icon { anchors.centerIn: parent; name: "screen"; ink: "#163321"; width: 25; height: 25 }
        }
        Column {
            y: 112; anchors.horizontalCenter: parent.horizontalCenter; spacing: 16
            ActionButton { compact: true; iconName: "grid"; hint: "Il tuo Mac"; dark: root.dark; onClicked: session.probe() }
            ActionButton { compact: true; iconName: "settings"; hint: "Impostazioni"; dark: root.dark; onClicked: preferences.open() }
        }
        ActionButton { anchors.bottom: parent.bottom; anchors.bottomMargin: 28; anchors.horizontalCenter: parent.horizontalCenter; compact: true; iconName: "help"; hint: "Guida rapida"; dark: root.dark; onClicked: help.open() }
    }

    Flickable {
        id: home
        visible: !root.hasSession
        anchors.left: rail.right; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        clip: true; contentWidth: width; contentHeight: Math.max(height, homeColumn.implicitHeight + 2 * homePadding)
        property real homePadding: width < 800 ? 28 : 42
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ColumnLayout {
            id: homeColumn
            width: Math.min(1120, home.width - 2 * home.homePadding)
            x: (home.width - width) / 2; y: home.homePadding
            spacing: 28
            RowLayout {
                Layout.fillWidth: true; spacing: 16
                Text { text: "relay"; color: root.fg; font.pixelSize: 30; font.weight: Font.DemiBold; font.letterSpacing: -1 }
                Text { visible: homeColumn.width > 640; text: "/  I tuoi dispositivi"; color: root.muted; font.pixelSize: 14 }
                Item { Layout.fillWidth: true }
                Rectangle {
                    implicitWidth: badgeLabel.implicitWidth + 42; implicitHeight: 32; radius: 16
                    color: root.dark ? "#21372a" : "#dbece0"
                    Row { anchors.centerIn: parent; spacing: 7
                        Icon { name: "lock"; ink: root.dark ? root.mint : "#3d6e4d"; width: 13; height: 13; anchors.verticalCenter: parent.verticalCenter }
                        Text { id: badgeLabel; text: session.host.endsWith(".ts.net") ? "Via Tailscale" : "Connessione VNC"; color: root.dark ? "#b9d8c1" : "#3d6e4d"; font.pixelSize: 12 }
                    }
                }
                ActionButton { compact: true; iconName: "sun"; hint: "Cambia aspetto"; dark: root.dark; onClicked: session.setDark(!session.dark) }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 16; Layout.bottomMargin: 12; spacing: 32
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 18
                    Text { text: "Il tuo Mac.\nUn po’ più vicino."; color: root.fg; font.pixelSize: homeColumn.width < 700 ? 34 : 44; font.weight: Font.DemiBold; font.letterSpacing: -1.5; Layout.fillWidth: true; wrapMode: Text.WordWrap; lineHeight: 1.1 }
                    Text { text: "Una finestra sul tuo desktop,\novunque tu sia."; color: root.muted; font.pixelSize: 15; lineHeight: 1.4; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                }
                MachineArt { visible: homeColumn.width >= 650; Layout.preferredWidth: homeColumn.width * .40; Layout.preferredHeight: 210; dark: root.dark }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "IL TUO DISPOSITIVO"; color: root.muted; font.pixelSize: 10; font.letterSpacing: 1.5 }
                Item { Layout.fillWidth: true }
                ActionButton { compact: true; dark: root.dark; iconName: "refresh"; hint: "Verifica collegamento"; enabled: session.host !== ""; onClicked: session.probe() }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: cardLayout.implicitHeight + 48
                color: root.panel; radius: 18; border.color: root.line
                GridLayout {
                    id: cardLayout
                    anchors.fill: parent; anchors.margins: 24
                    columns: homeColumn.width >= 700 ? 3 : 2
                    columnSpacing: 22; rowSpacing: 20
                    Rectangle {
                        Layout.preferredWidth: 64; Layout.preferredHeight: 64
                        radius: 14; color: root.dark ? "#22352a" : "#e8f3ec"
                        Icon { anchors.centerIn: parent; name: "screen"; width: 32; height: 32; ink: root.dark ? root.mint : "#3b724c" }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 9
                        Text { text: session.host ? session.displayName : "Aggiungi il tuo Mac"; color: root.fg; font.pixelSize: 21; font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight }
                        Text { text: session.host || "Indirizzo MagicDNS o IP"; color: root.muted; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideMiddle }
                        Row {
                            spacing: 7
                            Rectangle { width: 6; height: 6; radius: 3; color: session.online ? root.mint : root.muted; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: session.host ? (session.online ? "Disponibile" : "In attesa del Mac") : "Da configurare"; color: root.muted; font.pixelSize: 11 }
                        }
                    }
                    ActionButton {
                        label: session.host ? "Connetti" : "Configura"; iconName: "arrow"; accent: true
                        Layout.preferredWidth: 154
                        Layout.columnSpan: homeColumn.width >= 700 ? 1 : 2
                        Layout.alignment: Qt.AlignRight
                        onClicked: root.openLogin()
                    }
                }
            }
            Text {
                text: session.state === "error" ? session.message : "La tua password, nel portachiavi solo se lo scegli."
                color: session.state === "error" ? "#eab5a5" : root.muted
                font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
            Text { id: credentialNotice; text: ""; visible: text !== ""; color: root.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 30
                Text { text: "MENO DISTANZA. PIÙ SPAZIO."; color: root.muted; font.pixelSize: 10; font.letterSpacing: 1.5 }
                Item { Layout.fillWidth: true }
                Text { text: "Relay / 0.2"; color: root.muted; font.pixelSize: 11 }
            }
        }
    }

    Item {
        anchors.fill: parent; visible: root.hasSession
        Rectangle {
            id: sessionBar
            height: 76; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            color: root.panel
            Rectangle { width: parent.width; height: 1; anchors.bottom: parent.bottom; color: root.line }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 22; anchors.rightMargin: 22; spacing: 13
                Icon { name: "screen"; ink: root.dark ? root.mint : "#3b724c"; Layout.preferredWidth: 26; Layout.preferredHeight: 26 }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 3
                    Text { text: session.displayName; color: root.fg; font.pixelSize: 16; font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight }
                    Text { text: session.state === "connected" ? "Connesso  ·  " + session.resolution + "  ·  " + (session.lowBandwidth ? "Fluido" : "Qualità") : (session.state === "disconnecting" ? "Chiusura sessione…" : "Connessione in corso…"); color: root.muted; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                }
                ActionButton { compact: true; dark: root.dark; iconName: "camera"; hint: "Salva schermata"; enabled: session.state === "connected"; onClicked: { remote.releaseInput(); session.saveScreenshot(); remote.forceActiveFocus() } }
                ActionButton { compact: true; dark: root.dark; iconName: "expand"; hint: "Schermo intero · F11"; onClicked: { root.toggleFullscreen(); remote.forceActiveFocus() } }
                ActionButton { label: root.width < 760 ? "" : "Disconnetti"; compact: root.width < 760; hint: "Disconnetti"; iconName: "power"; dark: root.dark; enabled: session.state !== "disconnecting"; onClicked: { remote.releaseInput(); session.disconnectRemote() } }
            }
        }
        RemoteView { id: remote; session: root.backend; anchors.top: sessionBar.bottom; anchors.bottom: bottomHint.top; anchors.left: parent.left; anchors.right: parent.right; visible: session.state === "connected"; focus: true }
        Column {
            visible: session.state !== "connected"; anchors.centerIn: parent; spacing: 22
            BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: parent.visible; palette.dark: root.mint; palette.light: root.line }
            Text { text: session.state === "loading" ? "Il desktop sta arrivando." : (session.state === "disconnecting" ? "Chiusura sessione…" : "Un momento, raggiungo il Mac."); color: root.fg; font.pixelSize: 20; anchors.horizontalCenter: parent.horizontalCenter }
            Text { text: session.host; color: root.muted; font.pixelSize: 12; anchors.horizontalCenter: parent.horizontalCenter }
        }
        Rectangle {
            id: bottomHint
            height: 32; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; color: root.panel
            RowLayout { anchors.fill: parent; anchors.leftMargin: 22; anchors.rightMargin: 22
                Text { visible: root.width >= 800; text: "Tastiera e mouse → Mac"; color: root.muted; font.pixelSize: 10 }
                Item { Layout.fillWidth: true }
                Text { text: (session.macShortcuts ? "Ctrl = ⌘" : "Super = ⌘") + "  ·  Alt = ⌥  ·  F11 schermo intero"; color: root.muted; font.pixelSize: 10 }
            }
        }
    }

    PanelDialog {
        id: login; objectName: "loginDialog"
        dark: root.dark; preferredWidth: 460; windowWidth: root.width; windowHeight: root.height
        onClosed: { loginPassword.clear(); loginError.text = "" }
        Icon { name: "lock"; Layout.preferredWidth: 30; Layout.preferredHeight: 30; ink: root.dark ? root.mint : "#3b724c" }
        Text { text: "Bentornato sul Mac."; color: root.fg; font.pixelSize: 26; font.weight: Font.DemiBold; font.letterSpacing: -.8; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Text { text: "Usa le credenziali dell’account macOS."; color: root.muted; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.WordWrap; Layout.bottomMargin: 6 }
        ColumnLayout { spacing: 8; Layout.fillWidth: true
            Text { text: "Nome utente"; color: root.muted; font.pixelSize: 12 }
            Field { id: loginUser; dark: root.dark; Layout.fillWidth: true; placeholderText: "Nome dell’account"; Accessible.name: "Nome utente Mac"; onTextEdited: root.refreshLogin() }
        }
        ColumnLayout { spacing: 8; Layout.fillWidth: true
            Text { text: "Password"; color: root.muted; font.pixelSize: 12 }
            Field { id: loginPassword; dark: root.dark; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: "Password del Mac"; Accessible.name: "Password Mac"; onAccepted: { if (connectButton.enabled) connectButton.clicked() } }
        }
        Toggle { id: remember; dark: root.dark; text: "Ricorda password"; Layout.fillWidth: true; onToggled: { if (checked) session.loadPassword(loginUser.text.trim()) } }
        Text { text: "Salvata nel portachiavi di sistema dopo un accesso riuscito."; color: root.muted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Text { id: loginError; text: ""; visible: text !== ""; color: "#eab5a5"; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        actions: [
            ActionButton { label: "Annulla"; dark: root.dark; onClicked: login.close(); Layout.fillWidth: true },
            ActionButton { id: connectButton; objectName: "loginAction"; label: "Apri il Mac"; iconName: "arrow"; accent: true; enabled: loginUser.text.trim().length > 0 && loginPassword.text.length > 0; Layout.fillWidth: true; onClicked: { if (!enabled) return; session.connectRemote(loginUser.text, loginPassword.text, remember.checked); login.close() } }
        ]
    }

    PanelDialog {
        id: preferences; objectName: "preferencesDialog"
        dark: root.dark; preferredWidth: 520; windowWidth: root.width; windowHeight: root.height
        onOpened: { prefHost.text = session.host; prefPort.text = session.port.toString(); prefUser.text = session.username; prefName.text = session.displayName }
        Text { text: "Il tuo collegamento."; color: root.fg; font.pixelSize: 26; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        ColumnLayout { spacing: 8; Layout.fillWidth: true
            Text { text: "Nome del dispositivo"; color: root.muted; font.pixelSize: 12 }
            Field { id: prefName; dark: root.dark; Layout.fillWidth: true; Accessible.name: "Nome del dispositivo" }
        }
        ColumnLayout { spacing: 8; Layout.fillWidth: true
            Text { text: "Indirizzo MagicDNS o IP"; color: root.muted; font.pixelSize: 12 }
            Field { id: prefHost; dark: root.dark; Layout.fillWidth: true; placeholderText: "mac.example.ts.net"; Accessible.name: "Indirizzo del Mac" }
        }
        RowLayout { Layout.fillWidth: true; spacing: 14
            ColumnLayout { Layout.fillWidth: true; spacing: 8
                Text { text: "Nome utente"; color: root.muted; font.pixelSize: 12 }
                Field { id: prefUser; dark: root.dark; Layout.fillWidth: true; Accessible.name: "Utente predefinito" }
            }
            ColumnLayout { Layout.preferredWidth: 90; Layout.minimumWidth: 90; Layout.maximumWidth: 90; spacing: 8
                Text { text: "Porta"; color: root.muted; font.pixelSize: 12 }
                Field { id: prefPort; Layout.minimumWidth: 0; dark: root.dark; Layout.fillWidth: true; validator: IntValidator { bottom: 1; top: 65535 } Accessible.name: "Porta VNC" }
            }
        }
        Text { text: "Le modifiche valgono dalla prossima connessione."; color: root.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        RowLayout { Layout.fillWidth: true; spacing: 12
            ActionButton { label: "Fluido"; dark: root.dark; accent: session.lowBandwidth; Layout.fillWidth: true; onClicked: session.setLowBandwidth(true) }
            ActionButton { label: "Qualità"; dark: root.dark; accent: !session.lowBandwidth; Layout.fillWidth: true; onClicked: session.setLowBandwidth(false) }
        }
        Text { text: session.lowBandwidth ? "Meno dati, colori a 16 bit. Consigliato su Internet." : "Tutte le sfumature, con maggiore traffico di rete."; color: root.muted; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        Toggle { text: "Usa Ctrl come Command (⌘) sul Mac"; dark: root.dark; checked: session.macShortcuts; Layout.fillWidth: true; onToggled: session.setMacShortcuts(checked) }
        ActionButton { label: "Dimentica password"; dark: root.dark; Layout.fillWidth: true; enabled: session.remembersPassword(session.username); onClicked: session.forgetPassword(session.username) }
        actions: [ ActionButton { objectName: "preferencesAction"; Layout.fillWidth: true; label: "Salva impostazioni"; accent: true; enabled: prefHost.text.trim().length > 0 && !/[\s/]/.test(prefHost.text.trim()) && prefPort.acceptableInput; onClicked: { session.savePreferences(prefHost.text, Number(prefPort.text), prefUser.text, session.dark); session.setDisplayName(prefName.text); preferences.close() } } ]
    }

    PanelDialog {
        id: help; objectName: "helpDialog"; dark: root.dark; windowWidth: root.width; windowHeight: root.height
        Text { text: "Una finestra sul Mac."; color: root.fg; font.pixelSize: 25; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Text { text: "Connetti entrambi i computer a Tailscale e attiva Condivisione schermo sul Mac. Puoi usare anche una rete locale fidata."; wrapMode: Text.WordWrap; color: root.muted; Layout.fillWidth: true; lineHeight: 1.4 }
        Text { text: "F11    Schermo intero / torna alla finestra\n" + (session.macShortcuts ? "Ctrl    Command (⌘)\nSuper    Control del Mac" : "Super    Command (⌘)\nCtrl    Control del Mac") + "\nAlt    Option (⌥)"; color: root.fg; lineHeight: 1.8; font.pixelSize: 14; Layout.fillWidth: true }
        Text { text: "Le scorciatoie riservate da Hyprland restano gestite da Linux."; color: root.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12 }
        actions: [ ActionButton { objectName: "helpAction"; label: "Tutto chiaro"; accent: true; Layout.fillWidth: true; onClicked: help.close() } ]
    }

    Rectangle {
        visible: session.notice !== ""
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 52
        width: Math.min(root.width - 32, noticeText.implicitWidth + 44); height: noticeText.implicitHeight + 28; radius: 12; color: root.mint
        Text { id: noticeText; anchors.centerIn: parent; width: Math.min(implicitWidth, root.width - 76); text: session.notice; color: "#183321"; font.pixelSize: 13; wrapMode: Text.WordWrap }
    }
}
